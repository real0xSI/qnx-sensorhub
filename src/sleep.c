/* Minimal sleep(1) for the sensorhub demo .script (no sleep binary in SDP target tree). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sleep seconds\n"); return 1; }
    unsigned s = (unsigned)atoi(argv[1]);
    while (s > 0) s = sleep(s);
    return 0;
}
