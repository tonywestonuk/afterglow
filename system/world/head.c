/*
 * Head tracking, turning only (3DoF), from the IMU. SyncBoss, the headset's sensor microcontroller,
 * streams IMU samples a thousand times a second (system/tools/syncboss.c, docs/hardware.md); a thread
 * here fuses them into the head's orientation: the gyroscope turns it, gravity (the accelerometer)
 * keeps it level, and the gyroscope's offset is learnt whenever the head is still. The eyes' positions
 * follow from a neck model. Without the cameras there's no moving about, and the heading drifts
 * slowly; recentring puts "ahead" back to -z.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "world.h"

#define CONTROL "/sys/devices/virtual/misc/syncboss0/spi/control/"
#define IMU_ENABLE 110      /* command: start sending IMU samples */
#define IMU_SAMPLE 80       /* stream: u64 time (us), float accel[3] (g), gyro[3] (degrees/s), temperature */
#define DEGREES ((float)M_PI / 180)

/* The neck model: the eyes are this far above and in front of the point the head turns about */
#define NECK_UP 0.075f
#define NECK_FORWARD 0.0805f

/* Fusion: how hard gravity pulls the estimate level (rad/s per unit of error), more at first */
#define LEVEL_GAIN 0.6f
#define LEVEL_GAIN_START 8.0f
/* Still: turning slower than this (degrees/s, offset removed) with only gravity on the accelerometer */
#define STILL_RATE 2.5f
#define BIAS_SECONDS 2.0f   /* how quickly the learnt offset follows */

typedef struct { float w, x, y, z; } quat;

static quat q_mul(quat a, quat b)
{
    return (quat){ a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                   a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

static quat q_normalize(quat q)
{
    float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return n > 0 ? (quat){ q.w / n, q.x / n, q.y / n, q.z / n } : (quat){ 1, 0, 0, 0 };
}

/* A turn of |v| radians about v */
static quat q_rotation(const float v[3])
{
    float angle = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (angle < 1e-9f)
        return (quat){ 1, v[0] / 2, v[1] / 2, v[2] / 2 };
    float s = sinf(angle / 2) / angle;
    return (quat){ cosf(angle / 2), v[0] * s, v[1] * s, v[2] * s };
}

static void q_rotate(quat q, const float v[3], float out[3])
{
    quat p = q_mul(q_mul(q, (quat){ 0, v[0], v[1], v[2] }), (quat){ q.w, -q.x, -q.y, -q.z });
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}

/* Shared with the render loop */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static quat orientation = { 1, 0, 0, 0 };   /* head to world, before recentring */
static quat heading = { 1, 0, 0, 0 };       /* the recentring turn about y */
static float rate[3];                       /* smoothed turning rate, head axes, rad/s */
static double rate_time;                    /* world_time() of the latest sample */
static int tracking, recentre = 1;
static pthread_t thread;

/* Make /dev/NAME from /sys/class/misc/NAME/dev (there's no devtmpfs) and open it */
static int open_misc(const char *name, int flags)
{
    char path[128];
    unsigned major, minor;
    snprintf(path, sizeof(path), "/sys/class/misc/%s/dev", name);
    FILE *f = fopen(path, "r");
    int ok = f && fscanf(f, "%u:%u", &major, &minor) == 2;
    if (f)
        fclose(f);
    if (!ok)
        return -1;
    snprintf(path, sizeof(path), "/dev/%s", name);
    mknod(path, S_IFCHR | 0600, makedev(major, minor));
    return open(path, flags | O_CLOEXEC);
}

static int set(const char *name, int value)
{
    char path[160];
    snprintf(path, sizeof(path), CONTROL "%s", name);
    FILE *f = fopen(path, "w");
    int ok = f && fprintf(f, "%d\n", value) > 0;
    if (f && fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

static int start_imu(int *stream)
{
    /* Meta's settings (libsyncboss): a 248-byte transaction every millisecond, at least 30 us apart */
    if (set("transaction_period_us", 1000) || set("transaction_length", 248) ||
        set("minimum_time_between_transactions_us", 30)) {
        wlog("head tracking: SyncBoss's settings: %s", strerror(errno));
        return -1;
    }
    *stream = open_misc("syncboss_stream0", O_RDONLY);
    int dev = open_misc("syncboss0", O_RDWR);   /* releases the MCU from reset and starts streaming */
    int seq = -1;
    FILE *f = fopen(CONTROL "next_avail_seq_num", "r");
    if (!f || fscanf(f, "%d", &seq) != 1)
        seq = -1;
    if (f)
        fclose(f);
    unsigned char cmd[3] = { IMU_ENABLE, (unsigned char)seq, 0 };
    if (*stream < 0 || dev < 0 || seq < 0 || write(dev, cmd, sizeof(cmd)) != sizeof(cmd)) {
        wlog("head tracking: no IMU: %s", strerror(errno));
        return -1;
    }
    return dev;
}

static void *imu_thread(void *arg)
{
    (void)arg;
    int stream, dev = start_imu(&stream);
    if (dev < 0)
        return NULL;
    float bias[3] = { 0, 0, 0 }, smooth[3] = { 0, 0, 0 };
    unsigned long long last_us = 0;
    double still_since = -1, started = world_time();
    quat q = { 1, 0, 0, 0 };
    int levelled = 0;
    for (;;) {
        unsigned char buf[256];
        ssize_t n = read(stream, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 3) {
            wlog("head tracking: the IMU stream ended: %s", n < 0 ? strerror(errno) : "short read");
            break;
        }
        const unsigned char *p = buf + buf[1];   /* past the driver's header: type, sequence, length, data */
        if (n - buf[1] < 3 + 36 || p[0] != IMU_SAMPLE || p[2] != 36)
            continue;
        unsigned long long us;
        float f[7];
        memcpy(&us, p + 3, 8);
        memcpy(f, p + 11, sizeof(f));
        /* The IMU's axes as the head's (x right, y up, z back): measured 2026-10-19 */
        float a[3] = { -f[1], -f[0], -f[2] }, g[3] = { -f[4], -f[3], -f[5] };
        float dt = last_us && us > last_us ? (us - last_us) / 1e6f : 0;
        last_us = us;
        if (dt > 0.01f)
            dt = 0.01f;
        double t = world_time();

        /* Learn the gyroscope's offset while the head is still */
        float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        float moving = sqrtf((g[0] - bias[0]) * (g[0] - bias[0]) + (g[1] - bias[1]) * (g[1] - bias[1]) +
                             (g[2] - bias[2]) * (g[2] - bias[2]));
        if (moving < STILL_RATE && fabsf(an - 1) < 0.03f) {
            if (still_since < 0)
                still_since = t;
            if (t - still_since > 0.25)
                for (int k = 0; k < 3; k++)
                    bias[k] += (g[k] - bias[k]) * dt / BIAS_SECONDS;
        } else
            still_since = -1;

        float w[3];
        for (int k = 0; k < 3; k++)
            w[k] = (g[k] - bias[k]) * DEGREES;

        /* Gravity keeps it level: turn the estimate's "up" towards the measured one */
        if (an > 0.85f && an < 1.15f) {
            float up[3], world_up[3] = { 0, 1, 0 };
            q_rotate((quat){ q.w, -q.x, -q.y, -q.z }, world_up, up);   /* where the estimate puts up, in head axes */
            if (!levelled) {   /* the first sample: start level, facing -z */
                float axis[3] = { up[1] * a[2] / an - up[2] * a[1] / an, up[2] * a[0] / an - up[0] * a[2] / an,
                                  up[0] * a[1] / an - up[1] * a[0] / an };
                float s = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
                float c = (up[0] * a[0] + up[1] * a[1] + up[2] * a[2]) / an, angle = atan2f(s, c);
                if (s > 1e-6f)
                    for (int k = 0; k < 3; k++)
                        axis[k] *= -angle / s;
                q = q_normalize(q_rotation(axis));
                levelled = 1;
            } else {
                float gain = t - started < 1 ? LEVEL_GAIN_START : LEVEL_GAIN;
                w[0] += gain * (a[1] * up[2] - a[2] * up[1]) / an;
                w[1] += gain * (a[2] * up[0] - a[0] * up[2]) / an;
                w[2] += gain * (a[0] * up[1] - a[1] * up[0]) / an;
            }
        }
        float step[3] = { w[0] * dt, w[1] * dt, w[2] * dt };
        q = q_normalize(q_mul(q, q_rotation(step)));
        for (int k = 0; k < 3; k++)   /* for prediction: about 5 ms of smoothing */
            smooth[k] += ((g[k] - bias[k]) * DEGREES - smooth[k]) * fminf(1, dt / 0.005f);

        pthread_mutex_lock(&lock);
        orientation = q;
        memcpy(rate, smooth, sizeof(rate));
        rate_time = t;
        if (!tracking) {
            tracking = 1;
            wlog("head tracking: started (IMU at %.1f C)", f[6]);
        }
        pthread_mutex_unlock(&lock);
    }
    close(stream);
    close(dev);
    return NULL;
}

int head_start(void)
{
    return pthread_create(&thread, NULL, imu_thread, NULL) == 0 ? 0 : -1;
}

void head_recentre(void)
{
    pthread_mutex_lock(&lock);
    recentre = 1;
    pthread_mutex_unlock(&lock);
}

/*
 * The head's pose for showing `ahead` seconds from now: rotation (head to world, 3x3, column-major)
 * and the point between the eyes. Returns 1 if it's tracked, 0 if it's the fixed default.
 */
int head_pose(double ahead, float rotation[9], float centre[3])
{
    pthread_mutex_lock(&lock);
    quat q = orientation;
    int tracked = tracking;
    if (tracked) {
        float lead = (float)(world_time() + ahead - rate_time);
        float step[3] = { rate[0] * lead, rate[1] * lead, rate[2] * lead };
        q = q_normalize(q_mul(q, q_rotation(step)));
        if (recentre) {   /* turn about y so that the head faces -z now */
            float ahead_dir[3] = { 0, 0, -1 }, f[3];
            q_rotate(orientation, ahead_dir, f);
            float yaw = atan2f(-f[0], -f[2]);
            heading = (quat){ cosf(-yaw / 2), 0, sinf(-yaw / 2), 0 };
            recentre = 0;
        }
        q = q_mul(heading, q);
    }
    pthread_mutex_unlock(&lock);

    float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int c = 0; c < 3; c++)
        q_rotate(q, axes[c], &rotation[c * 3]);
    /* The neck stays put; the eyes swing about it */
    float neck[3] = { 0, EYE_HEIGHT - NECK_UP, NECK_FORWARD }, offset[3] = { 0, NECK_UP, -NECK_FORWARD }, turned[3];
    q_rotate(q, offset, turned);
    for (int k = 0; k < 3; k++)
        centre[k] = neck[k] + turned[k];
    return tracked;
}

void head_quaternion(double ahead, float xyzw[4])
{
    float r[9], centre[3];
    head_pose(ahead, r, centre);
    /* From the rotation matrix (columns are the head's axes in the world) */
    float trace = r[0] + r[4] + r[8], w, x, y, z;
    if (trace > 0) {
        float s = sqrtf(trace + 1) * 2;
        w = s / 4, x = (r[5] - r[7]) / s, y = (r[6] - r[2]) / s, z = (r[1] - r[3]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2;
        w = (r[5] - r[7]) / s, x = s / 4, y = (r[3] + r[1]) / s, z = (r[6] + r[2]) / s;
    } else if (r[4] > r[8]) {
        float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2;
        w = (r[6] - r[2]) / s, x = (r[3] + r[1]) / s, y = s / 4, z = (r[7] + r[5]) / s;
    } else {
        float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2;
        w = (r[1] - r[3]) / s, x = (r[6] + r[2]) / s, y = (r[7] + r[5]) / s, z = s / 4;
    }
    xyzw[0] = x, xyzw[1] = y, xyzw[2] = z, xyzw[3] = w;
}
