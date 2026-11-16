/*
 * sensorhub — QNX 8.0 resource-manager daemon exposing two virtual sensors:
 *   /dev/sensor0  temperature (simulated, slow upward drift)
 *   /dev/sensor1  humidity + pressure (simulated)
 *
 * QNX-specific mechanisms used (the point of the hackathon entry):
 *   - resource manager framework (resmgr_attach / dispatch / iofunc)
 *   - devctl() for configuration (sampling rate, threshold, stats)
 *   - QNX pulses for threshold notification (server -> client channel)
 *   - POSIX timer delivering a pulse to the dispatch channel
 *
 * Read protocol: each read() at offset 0 returns one fresh one-line
 * reading; a second read without lseek() returns 0 (EOF), like a
 * regular device. sensorctl lseeks to 0 before every read.
 *
 * Build: source qnxsdp-env.sh, then `make -C build` (ARCH=aarch64 default).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/resmgr.h>
#include <sys/iofunc.h>
#include <sys/procmgr.h>

#include "sensorhub.h"

/* Pulse code for the internal sampling timer (distinct from SENSOR_PULSE_CODE). */
#define TIMER_PULSE_CODE (_PULSE_CODE_MINAVAIL + 2)

static resmgr_attr_t    resmgr_attr;
static dispatch_t      *dpp;
static iofunc_attr_t    attr0, attr1;
static int              id0 = -1, id1 = -1;
static timer_t          timerid;

typedef struct {
    float    temp;      /* degrees C */
    float    hum;       /* percent */
    float    press;     /* hPa */
    uint64_t seq;       /* ticks since start */
    uint64_t reads;
    uint64_t devctls;
    uint64_t pulses;
    int      rate_ms;
    float    threshold;
    int      sub_active;
    int      sub_coid;  /* connection to subscriber channel, -1 = none */
    int      sub_code;
    int      notified;  /* threshold already notified for this crossing */
} sensor_state_t;

static sensor_state_t st;

static float frand(float lo, float hi)
{
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

static void arm_timer(void)
{
    struct itimerspec its;
    its.it_value.tv_sec = st.rate_ms / 1000;
    its.it_value.tv_nsec = (st.rate_ms % 1000) * 1000000L;
    its.it_interval = its.it_value;
    timer_settime(timerid, 0, &its, NULL);
}

static void sensor_tick(void)
{
    /* Random walk with a slow upward drift, so a low threshold is
     * crossed within a minute or two at the default 1 s rate. */
    st.temp += frand(-0.30f, 0.30f) + 0.10f;
    if (st.temp < 15.0f) st.temp = 15.0f;
    if (st.temp > 45.0f) st.temp = 45.0f;
    st.hum += frand(-1.0f, 1.0f);
    if (st.hum < 20.0f) st.hum = 20.0f;
    if (st.hum > 80.0f) st.hum = 80.0f;
    st.press += frand(-0.5f, 0.5f);
    if (st.press < 990.0f) st.press = 990.0f;
    if (st.press > 1030.0f) st.press = 1030.0f;
    st.seq++;

    /* Threshold watch: notify once per crossing (edge-triggered),
     * re-armed when the temperature falls back below the threshold. */
    if (st.sub_active && st.sub_coid >= 0) {
        if (st.temp >= st.threshold && !st.notified) {
            int value = (int)(st.temp * 100.0f); /* centi-degrees */
            if (MsgSendPulse(st.sub_coid, 10, st.sub_code, value) == 0) {
                st.pulses++;
                st.notified = 1;
            }
        } else if (st.temp < st.threshold) {
            st.notified = 0;
        }
    }
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status;
    char buf[128];
    int nbytes, sensor;
    iofunc_ocb_t *ocb_io = (iofunc_ocb_t *)ocb; /* RESMGR_OCB_T is void in SDP 8 */

    if ((status = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK)
        return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;

    sensor = (ctp->id == id0) ? 0 : 1;
    st.reads++;

    /* One reading per open offset: a second read returns EOF until the
     * client lseeks back to 0. */
    if (ocb_io->offset != 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    if (sensor == 0)
        nbytes = snprintf(buf, sizeof buf, "seq=%llu temp=%.2fC rate=%dms\n",
                          (unsigned long long)st.seq, st.temp, st.rate_ms);
    else
        nbytes = snprintf(buf, sizeof buf, "seq=%llu hum=%.1f%% press=%.2fhPa rate=%dms\n",
                          (unsigned long long)st.seq, st.hum, st.press, st.rate_ms);

    if (nbytes > msg->i.nbytes)
        nbytes = msg->i.nbytes;
    ocb_io->offset += nbytes;
    _IO_SET_READ_NBYTES(ctp, nbytes);
    SETIOV(ctp->iov, buf, nbytes);
    return _RESMGR_NPARTS(1);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    int status;
    sensor_stats_t stats;
    sensor_subscribe_t sub;
    int rate;
    float thresh;

    if ((status = iofunc_devctl_verify(ctp, msg, ocb, 0)) != EOK)
        return status;

    st.devctls++;

    switch (msg->i.dcmd) {
    case DCMD_SENSOR_GET_RATE:
        rate = st.rate_ms;
        resmgr_msgwrite(ctp, &rate, sizeof rate, sizeof(msg->o));
        msg->o.nbytes = sizeof rate;
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_SET_RATE:
        if (msg->i.nbytes < (int)sizeof rate)
            return EINVAL;
        resmgr_msgread(ctp, &rate, sizeof rate, sizeof(msg->i));
        if (rate < 50 || rate > 60000)
            return EINVAL;
        st.rate_ms = rate;
        arm_timer();
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_GET_STATS:
        stats.reads = st.reads;
        stats.devctls = st.devctls;
        stats.pulses = st.pulses;
        stats.rate_ms = st.rate_ms;
        stats.threshold = st.threshold;
        stats.temp = st.temp;
        stats.hum = st.hum;
        stats.press = st.press;
        resmgr_msgwrite(ctp, &stats, sizeof stats, sizeof(msg->o));
        msg->o.nbytes = sizeof stats;
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_RESET_STATS:
        st.reads = st.devctls = st.pulses = 0;
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_GET_THRESH:
        thresh = st.threshold;
        resmgr_msgwrite(ctp, &thresh, sizeof thresh, sizeof(msg->o));
        msg->o.nbytes = sizeof thresh;
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_SET_THRESH:
        if (msg->i.nbytes < (int)sizeof thresh)
            return EINVAL;
        resmgr_msgread(ctp, &thresh, sizeof thresh, sizeof(msg->i));
        if (thresh < 15.0f || thresh > 45.0f)
            return EINVAL;
        st.threshold = thresh;
        st.notified = 0;
        msg->o.ret_val = EOK;
        return EOK;

    case DCMD_SENSOR_SUBSCRIBE:
        if (msg->i.nbytes < (int)sizeof sub)
            return EINVAL;
        resmgr_msgread(ctp, &sub, sizeof sub, sizeof(msg->i));
        if (st.sub_coid >= 0) {
            ConnectDetach(st.sub_coid);
            st.sub_coid = -1;
        }
        st.sub_coid = ConnectAttach(ND_LOCAL_NODE, sub.pid, sub.chid,
                                    _NTO_SIDE_CHANNEL, 0);
        if (st.sub_coid < 0)
            return errno;
        st.sub_code = sub.code;
        st.threshold = sub.threshold;
        st.sub_active = 1;
        st.notified = 0;
        msg->o.ret_val = EOK;
        return EOK;

    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }
}

int main(int argc, char **argv)
{
    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t io_funcs;
    dispatch_context_t *ctp;
    struct sigevent sev;
    int opt, daemonize = 0;
    int chid = -1;

    memset(&st, 0, sizeof st);
    st.temp = 22.0f;
    st.hum = 45.0f;
    st.press = 1013.25f;
    st.rate_ms = SENSOR_DEFAULT_RATE_MS;
    st.threshold = SENSOR_DEFAULT_THRESH;
    st.sub_coid = -1;
    srand((unsigned)time(NULL));

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd')
            daemonize = 1;
    }
    if (daemonize) {
        if (procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCLOSE) == -1) {
            perror("procmgr_daemon");
            return EXIT_FAILURE;
        }
    }

    /* SDP 8: dispatch_t is opaque (no dpp->chid). Own the channel and
     * attach the dispatch to it, so the timer pulse has a known coid. */
    if ((chid = ChannelCreate(0)) == -1) {
        perror("ChannelCreate");
        return EXIT_FAILURE;
    }
    if ((dpp = dispatch_create_channel(chid, 0)) == NULL) {
        perror("dispatch_create_channel");
        return EXIT_FAILURE;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&attr0, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, NULL, NULL);
    iofunc_attr_init(&attr1, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, NULL, NULL);

    id0 = resmgr_attach(dpp, &resmgr_attr, SENSOR_DEV0, _FTYPE_ANY, 0,
                        &connect_funcs, &io_funcs, &attr0);
    if (id0 == -1) {
        perror("resmgr_attach /dev/sensor0");
        return EXIT_FAILURE;
    }
    id1 = resmgr_attach(dpp, &resmgr_attr, SENSOR_DEV1, _FTYPE_ANY, 0,
                        &connect_funcs, &io_funcs, &attr1);
    if (id1 == -1) {
        perror("resmgr_attach /dev/sensor1");
        return EXIT_FAILURE;
    }

    /* Sampling timer: fires a pulse into the dispatch channel.
     * sigev_coid must be a CONNECTION id (ConnectAttach), NOT the raw
     * channel id — a chid passed as coid silently delivers no pulse,
     * so the sampling timer never fired (seq stayed 0, temp frozen). */
    int timer_coid = ConnectAttach(ND_LOCAL_NODE, 0, chid,
                                   _NTO_SIDE_CHANNEL, 0);
    if (timer_coid == -1) {
        perror("ConnectAttach timer");
        return EXIT_FAILURE;
    }
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_PULSE;
    sev.sigev_coid = timer_coid;
    sev.sigev_priority = 10;
    sev.sigev_code = TIMER_PULSE_CODE;
    if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
        perror("timer_create");
        return EXIT_FAILURE;
    }
    arm_timer();

    printf("sensorhub: /dev/sensor0 + /dev/sensor1 up "
           "(rate %d ms, threshold %.1fC)\n", st.rate_ms, st.threshold);
    fflush(stdout);

    ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            fprintf(stderr, "sensorhub: dispatch_block: %s\n",
                    strerror(errno));
            continue;
        }
        if (ctp->resmgr_context.rcvid == 0) {
            /* Pulse: sampling timer (or stray pulse, ignored). */
            if (ctp->resmgr_context.msg->pulse.code == TIMER_PULSE_CODE)
                sensor_tick();
        } else {
            dispatch_handler(ctp);
        }
    }
    /* NOTREACHED */
    return EXIT_SUCCESS;
}
