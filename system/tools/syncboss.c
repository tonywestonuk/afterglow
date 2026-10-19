/*
 * syncboss [-t SECONDS] [-x] [-n] [-i N]: switch on SyncBoss, the headset's sensor microcontroller (an
 * nRF52 on SPI, with Meta's kernel driver), ask it for IMU data, and report what it streams: per packet
 * type, how many arrived, how often and how long they are, with the first of each in hex. -x hex-dumps
 * every packet; -n doesn't ask for the IMU; -i N prints every Nth IMU sample decoded (seconds,
 * acceleration in g, rotation in degrees/s, temperature in C).
 *
 * The stream settings and the IMU command are those Meta's own library (libsyncboss) uses
 * (docs/hardware.md).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define CONTROL "/sys/devices/virtual/misc/syncboss0/spi/control/"
#define IMU_ENABLE 110
#define IMU_SAMPLE 80   /* stream type: u64 time (us), float accel[3] (g), gyro[3] (degrees/s), temperature (C) */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Make /dev/NAME from /sys/class/misc/NAME/dev (there's no devtmpfs) and open it */
static int open_misc(const char *name, int flags)
{
    char path[128];
    unsigned major, minor;
    snprintf(path, sizeof(path), "/sys/class/misc/%s/dev", name);
    FILE *f = fopen(path, "r");
    if (!f || fscanf(f, "%u:%u", &major, &minor) != 2) {
        fprintf(stderr, "syncboss: no %s\n", path);
        if (f)
            fclose(f);
        return -1;
    }
    fclose(f);
    snprintf(path, sizeof(path), "/dev/%s", name);
    mknod(path, S_IFCHR | 0600, makedev(major, minor));
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        fprintf(stderr, "syncboss: %s: %s\n", path, strerror(errno));
    return fd;
}

static int set(const char *name, int value)
{
    char path[160];
    snprintf(path, sizeof(path), CONTROL "%s", name);
    FILE *f = fopen(path, "w");
    int ok = f && fprintf(f, "%d\n", value) > 0;
    if (f && fclose(f) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "syncboss: %s: %s\n", path, strerror(errno));
    return ok ? 0 : -1;
}

static int next_sequence(void)
{
    int seq = -1;
    FILE *f = fopen(CONTROL "next_avail_seq_num", "r");
    if (!f || fscanf(f, "%d", &seq) != 1)
        seq = -1;
    if (f)
        fclose(f);
    return seq;
}

static void hex(const char *what, const unsigned char *p, int n)
{
    printf("%s", what);
    for (int i = 0; i < n; i++)
        printf("%s%02x", i && i % 4 == 0 ? " " : "", p[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    double seconds = 3;
    int dump = 0, imu = 1, every = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "-x") == 0)
            dump = 1;
        else if (strcmp(argv[i], "-n") == 0)
            imu = 0;
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            every = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: syncboss [-t SECONDS] [-x] [-n] [-i N]\n");
            return 2;
        }
    }
    /* Meta's settings: a 248-byte transaction every millisecond, at least 30 us apart */
    if (set("transaction_period_us", 1000) || set("transaction_length", 248) ||
        set("minimum_time_between_transactions_us", 30))
        return 1;
    int stream = open_misc("syncboss_stream0", O_RDONLY | O_NONBLOCK);
    int control = open_misc("syncboss_control0", O_RDONLY | O_NONBLOCK);
    double t0 = now();
    int dev = open_misc("syncboss0", O_RDWR);   /* releases the MCU from reset and starts streaming */
    if (stream < 0 || control < 0 || dev < 0)
        return 1;
    printf("streaming after %.0f ms\n", 1000 * (now() - t0));
    if (imu) {
        int seq = next_sequence();
        unsigned char cmd[3] = { IMU_ENABLE, (unsigned char)seq, 0 };
        if (seq < 0 || write(dev, cmd, sizeof(cmd)) != sizeof(cmd))
            fprintf(stderr, "syncboss: asking for the IMU: %s\n", strerror(errno));
        else
            printf("asked for the IMU (sequence %d)\n", seq);
    }

    struct { int count, min_len, max_len; double first, last; unsigned char sample[256]; int sample_len; } seen[256];
    memset(seen, 0, sizeof(seen));
    struct pollfd fds[2] = { { stream, POLLIN, 0 }, { control, POLLIN, 0 } };
    t0 = now();
    while (now() - t0 < seconds) {
        if (poll(fds, 2, 100) <= 0)
            continue;
        for (int k = 0; k < 2; k++) {
            unsigned char buf[512];
            ssize_t n;
            while ((n = read(fds[k].fd, buf, sizeof(buf))) > 3) {
                const unsigned char *p = buf + buf[1];   /* past the driver's header */
                int len = (int)n - buf[1];
                if (len < 3)
                    continue;
                if (dump) {
                    char what[48];
                    snprintf(what, sizeof(what), "%8.3f %s type %3d seq %3d len %3d: ", now() - t0,
                             k ? "control" : "stream ", p[0], p[1], p[2]);
                    hex(what, p + 3, len - 3);
                }
                if (every > 0 && k == 0 && p[0] == IMU_SAMPLE && p[2] == 36) {
                    static unsigned long samples;
                    if (samples++ % every == 0) {
                        unsigned long long us;
                        float f[7];
                        memcpy(&us, p + 3, 8);
                        memcpy(f, p + 11, sizeof(f));
                        printf("%.3f %+.4f %+.4f %+.4f %+8.2f %+8.2f %+8.2f %.2f\n", us / 1e6, f[0], f[1], f[2], f[3],
                               f[4], f[5], f[6]);
                        fflush(stdout);
                    }
                }
                if (k == 1) {
                    if (!dump)
                        hex("control reply: ", p, len);
                    continue;
                }
                typeof(seen[0]) *s = &seen[p[0]];
                if (s->count == 0) {
                    s->first = now(), s->min_len = s->max_len = p[2];
                    s->sample_len = len - 3;
                    memcpy(s->sample, p + 3, len - 3);
                }
                s->count++, s->last = now();
                if (p[2] < s->min_len)
                    s->min_len = p[2];
                if (p[2] > s->max_len)
                    s->max_len = p[2];
            }
        }
    }
    for (int t = 0; t < 256; t++) {
        if (!seen[t].count)
            continue;
        double span = seen[t].last - seen[t].first;
        char what[96];
        snprintf(what, sizeof(what), "type %3d: %6d packets, %7.1f/s, %d-%d bytes; first: ", t, seen[t].count,
                 span > 0 ? (seen[t].count - 1) / span : 0, seen[t].min_len, seen[t].max_len);
        hex(what, seen[t].sample, seen[t].sample_len);
    }
    close(dev);   /* the last client: streaming stops and the MCU is held in reset again */
    return 0;
}
