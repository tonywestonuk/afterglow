/*
 * afterglow's first process (PID 1). For now it sets up a minimal environment, starts the USB serial
 * console (kernel log out, commands in), and plays the boot animation. Development builds restart
 * after afterglow.test_seconds=N (kernel command line), into afterglow.restart_to if given.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/reboot.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "afterglow.h"

static int kmsg_fd = -1;

void klog(const char *fmt, ...)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "<6>afterglow: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(buf) - 1)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';   /* without it the kernel joins consecutive messages into one record */
    if (kmsg_fd >= 0 && write(kmsg_fd, buf, n) < 0)
        return;   /* nowhere else to report it */
}

double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void make_node(const char *path, unsigned major, unsigned minor)
{
    if (mknod(path, S_IFCHR | 0600, makedev(major, minor)) != 0 && errno != EEXIST)
        klog("mknod %s: %s", path, strerror(errno));
}

/* Create a block device node for a partition, found by its GPT name (PARTNAME in sysfs uevent). */
static int make_partition_node(const char *partname, const char *path)
{
    DIR *dir = opendir("/sys/class/block");
    struct dirent *ent;
    size_t len = strlen(partname);
    int found = -1;
    while (dir && found != 0 && (ent = readdir(dir)) != NULL) {
        char file[300], line[128];
        unsigned major = 0, minor = 0;
        int match = 0;
        snprintf(file, sizeof(file), "/sys/class/block/%s/uevent", ent->d_name);
        FILE *f = fopen(file, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "MAJOR=%u", &major);
            sscanf(line, "MINOR=%u", &minor);
            if (strncmp(line, "PARTNAME=", 9) == 0 && strncmp(line + 9, partname, len) == 0 &&
                (line[9 + len] == '\n' || line[9 + len] == '\0'))
                match = 1;
        }
        fclose(f);
        if (match && mknod(path, S_IFBLK | 0400, makedev(major, minor)) == 0)
            found = 0;
    }
    if (dir)
        closedir(dir);
    return found;
}

/* Value of "key": <number> in a small JSON file */
static int json_number(const char *path, const char *key, double *value)
{
    char buf[2048], quoted[64];
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    char *p = strstr(buf, quoted);
    if (!p || !(p = strchr(p + strlen(quoted), ':')))
        return -1;
    *value = strtod(p + 1, NULL);
    return 0;
}

/*
 * Each Quest 1 has factory display calibration in its private partition, including a "roll" per
 * panel: calibration/display/<side>/<side>_screen_offset.json. Its units aren't documented. Read as
 * radians it doesn't match what a viewer sees (2026-10-18: left 0.010, right 0.176 would mean a
 * 10° difference between the eyes, which isn't there), so for now it's only logged. The partition
 * is mounted read-only without journal replay ("noload"), so nothing is written to it.
 */
static void read_factory_roll(double roll[2])
{
    static const char *sides[2] = { "left", "right" };
    roll[0] = roll[1] = 0;
    mkdir("/mnt", 0755);
    mkdir("/mnt/private", 0755);
    if (make_partition_node("private", "/dev/private") != 0 ||
        mount("/dev/private", "/mnt/private", "ext4", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "noload") != 0) {
        klog("no display calibration: can't mount private (%s)", strerror(errno));
        return;
    }
    for (int eye = 0; eye < 2; eye++) {
        char path[128];
        snprintf(path, sizeof(path), "/mnt/private/calibration/display/%s/%s_screen_offset.json", sides[eye], sides[eye]);
        if (json_number(path, "roll", &roll[eye]) != 0)
            klog("no roll in %s", path);
    }
    umount("/mnt/private");
    klog("factory display roll (unapplied): left %.4f, right %.4f", roll[0], roll[1]);
}

/* Meta's kernel has no devtmpfs, so create /dev/fb0 from the numbers sysfs reports. */
static int make_fb_node(void)
{
    unsigned major, minor;
    FILE *f = fopen("/sys/class/graphics/fb0/dev", "r");
    if (!f)
        return -1;
    int ok = fscanf(f, "%u:%u", &major, &minor) == 2;
    fclose(f);
    if (!ok)
        return -1;
    make_node("/dev/fb0", major, minor);
    return 0;
}

static double cmdline_number(const char *key, double fallback)
{
    char buf[4096];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f)
        return fallback;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t len = strlen(key);
    for (char *p = buf; (p = strstr(p, key)) != NULL; p += len) {
        if ((p == buf || p[-1] == ' ') && p[len] == '=')
            return atof(p + len + 1);
    }
    return fallback;
}

/* Value of key=value on the kernel command line, or NULL */
static const char *cmdline_string(const char *key, char *out, size_t out_len)
{
    char buf[4096];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t len = strlen(key);
    for (char *p = buf; (p = strstr(p, key)) != NULL; p += len) {
        if ((p == buf || p[-1] == ' ') && p[len] == '=') {
            snprintf(out, out_len, "%.*s", (int)strcspn(p + len + 1, " \n"), p + len + 1);
            return out;
        }
    }
    return NULL;
}

void restart(const char *target)
{
    klog("restarting%s%s", target ? " to " : "", target ? target : "");
    sync();
    if (target)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, target);
    reboot(RB_AUTOBOOT);
    for (;;)
        pause();
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    make_node("/dev/kmsg", 1, 11);
    make_node("/dev/null", 1, 3);
    make_node("/dev/console", 5, 1);
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    klog("init started");
    if (usb_console_start() != 0)
        klog("no USB console");

    double seconds = cmdline_number("afterglow.test_seconds", 0);
    double deadline = seconds > 0 ? now() + seconds : 0;
    if (seconds > 0)
        klog("test build: restarting after %.0f s", seconds);

    double factory[2];
    read_factory_roll(factory);
    /* Per-eye correction in degrees, counterclockwise as the viewer sees it, measured with calibrate.c */
    double degrees[2] = { cmdline_number("afterglow.roll_left_deg", 0), cmdline_number("afterglow.roll_right_deg", 0) };

    if (make_fb_node() != 0) {
        klog("no framebuffer: /sys/class/graphics/fb0/dev unreadable (%s)", strerror(errno));
    } else if (cmdline_number("afterglow.calibrate", 0) != 0) {
        if (calibrate_run("/dev/fb0", degrees, deadline) != 0)
            klog("calibration failed");
    } else {
        double roll[2] = { degrees[0] * M_PI / 180, degrees[1] * M_PI / 180 };
        if (bootanim_play("/afterglow/boot.anim", "/dev/fb0", roll, deadline) != 0)
            klog("boot animation failed");
    }

    if (deadline > 0) {
        char target[32];
        while (now() < deadline)
            sleep(1);
        restart(cmdline_string("afterglow.restart_to", target, sizeof(target)));
    }
    for (;;)
        pause();
}
