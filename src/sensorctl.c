/*
 * sensorctl — client for the qnx-sensorhub resource manager.
 *
 *   sensorctl [-d /dev/sensor0] [-n COUNT] [-r RATE_MS] [-t THRESH_C]
 *             [-s] [-S] [-R]
 *
 *   -n COUNT     read COUNT readings (default 10), 1 s apart
 *   -r RATE_MS   set sampling rate via devctl, then read
 *   -t THRESH_C  set temperature threshold via devctl, then read
 *   -s           print daemon stats (devctl GET_STATS) and exit
 *   -R           reset daemon stats (devctl RESET_STATS) and exit
 *   -S           subscribe to the threshold pulse: registers this
 *                process's channel with the daemon, then blocks until
 *                the pulse fires (Ctrl-C to stop). Use together with
 *                -t to set the threshold first.
 *
 * Readings are polled with lseek(fd, 0) before each read() because the
 * device returns EOF at a non-zero offset (one reading per offset 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <devctl.h>

#include "sensorhub.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-d device] [-n count] [-r rate_ms] [-t thresh_C] [-s] [-S] [-R]\n"
        "  -d device   device to open (default %s)\n"
        "  -n count    number of readings (default 10)\n"
        "  -r rate_ms  set sampling rate via devctl\n"
        "  -t thresh_C set temperature threshold via devctl\n"
        "  -s          print daemon stats and exit\n"
        "  -R          reset daemon stats and exit\n"
        "  -S          subscribe to threshold pulse and wait (Ctrl-C to stop)\n",
        prog, SENSOR_DEV0);
}

static int devctl_or_die(int fd, int dcmd, void *data, size_t nbytes,
                         const char *what)
{
    int rc = devctl(fd, dcmd, data, nbytes, NULL);
    if (rc != EOK) {
        fprintf(stderr, "sensorctl: devctl %s failed: %s\n",
                what, strerror(rc));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = SENSOR_DEV0;
    int count = 10, rate = -1, do_stats = 0, do_sub = 0, do_reset = 0;
    float thresh = 0.0f;
    int have_thresh = 0;
    int fd, chid = -1, i, opt;

    while ((opt = getopt(argc, argv, "d:n:r:t:sSRh")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'n': count = atoi(optarg); break;
        case 'r': rate = atoi(optarg); break;
        case 't': thresh = (float)atof(optarg); have_thresh = 1; break;
        case 's': do_stats = 1; break;
        case 'S': do_sub = 1; break;
        case 'R': do_reset = 1; break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sensorctl: open %s: %s\n", dev, strerror(errno));
        return EXIT_FAILURE;
    }

    if (rate > 0) {
        if (devctl_or_die(fd, DCMD_SENSOR_SET_RATE, &rate, sizeof rate,
                          "SET_RATE") != 0)
            return EXIT_FAILURE;
        printf("sensorctl: sampling rate set to %d ms\n", rate);
    }
    if (have_thresh) {
        if (devctl_or_die(fd, DCMD_SENSOR_SET_THRESH, &thresh, sizeof thresh,
                          "SET_THRESH") != 0)
            return EXIT_FAILURE;
        printf("sensorctl: threshold set to %.2fC\n", thresh);
    }
    if (do_reset) {
        if (devctl_or_die(fd, DCMD_SENSOR_RESET_STATS, NULL, 0,
                          "RESET_STATS") != 0)
            return EXIT_FAILURE;
        printf("sensorctl: stats reset\n");
    }
    if (do_stats) {
        sensor_stats_t s;
        if (devctl_or_die(fd, DCMD_SENSOR_GET_STATS, &s, sizeof s,
                          "GET_STATS") != 0)
            return EXIT_FAILURE;
        printf("reads=%llu devctls=%llu pulses=%llu rate=%dms "
               "threshold=%.2fC temp=%.2fC hum=%.1f%% press=%.2fhPa\n",
               (unsigned long long)s.reads,
               (unsigned long long)s.devctls,
               (unsigned long long)s.pulses,
               s.rate_ms, s.threshold, s.temp, s.hum, s.press);
        return EXIT_SUCCESS;
    }

    if (do_sub) {
        struct _pulse pulse;
        int rcvid;
        sensor_subscribe_t sub;

        if (!have_thresh) {
            /* Read current temperature so the demo can pick a close
             * threshold; default to current + 1C. */
            sensor_stats_t s;
            if (devctl(fd, DCMD_SENSOR_GET_STATS, &s, sizeof s, NULL) == EOK)
                thresh = s.temp + 1.0f;
            else
                thresh = SENSOR_DEFAULT_THRESH;
        }
        chid = ChannelCreate(0);
        if (chid < 0) {
            fprintf(stderr, "sensorctl: ChannelCreate: %s\n",
                    strerror(errno));
            return EXIT_FAILURE;
        }
        sub.pid = getpid();
        sub.chid = chid;
        sub.code = SENSOR_PULSE_CODE;
        sub.threshold = thresh;
        if (devctl_or_die(fd, DCMD_SENSOR_SUBSCRIBE, &sub, sizeof sub,
                          "SUBSCRIBE") != 0)
            return EXIT_FAILURE;
        printf("sensorctl: subscribed, threshold %.2fC — waiting for pulse "
               "(Ctrl-C to stop)...\n", thresh);
        fflush(stdout);
        for (;;) {
            rcvid = MsgReceive(chid, &pulse, sizeof pulse, NULL);
            if (rcvid == -1) {
                fprintf(stderr, "sensorctl: MsgReceive: %s\n",
                        strerror(errno));
                return EXIT_FAILURE;
            }
            if (rcvid == 0 && pulse.code == SENSOR_PULSE_CODE) {
                printf("THRESHOLD CROSSED: temp=%.2fC\n",
                       pulse.value.sival_int / 100.0);
                return EXIT_SUCCESS;
            }
            /* Ignore any other pulse. */
        }
    }

    for (i = 0; i < count; i++) {
        char buf[256];
        ssize_t n;

        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            fprintf(stderr, "sensorctl: lseek: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        n = read(fd, buf, sizeof buf - 1);
        if (n < 0) {
            fprintf(stderr, "sensorctl: read: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        if (n == 0)
            break;
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
        if (i < count - 1)
            sleep(1);
    }
    return EXIT_SUCCESS;
}
