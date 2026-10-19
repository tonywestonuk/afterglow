/*
 * afterglow's first process (PID 1). It sets up a minimal environment, starts the USB serial console
 * (kernel log out, commands in) with a BusyBox shell on its second port, starts the hardware bring-up
 * (Wi-Fi, afterglowd: system/scripts/bringup.sh), and plays the boot animation until that's done,
 * then shows the status screen. It runs until it's told to stop: Power pressed twice switches the
 * headset off, and so do BusyBox's poweroff and halt (reboot restarts it); `restart bootloader` goes
 * to the fastboot menu. Experiments that might hang can restart after afterglow.test_seconds=N
 * (kernel command line), into afterglow.restart_to if given.
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
#include <pthread.h>
#include <signal.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#include "afterglow.h"
#include "display.h"

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

/* The loopback interface (127.0.0.1, which the kernel adds when it comes up): local services need it */
static void loopback_up(void)
{
    struct ifreq ifr = { .ifr_name = "lo" };
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0 || ioctl(s, SIOCGIFFLAGS, &ifr) != 0)
        klog("loopback: %s", strerror(errno));
    else if (ifr.ifr_flags |= IFF_UP, ioctl(s, SIOCSIFFLAGS, &ifr) != 0)
        klog("loopback up: %s", strerror(errno));
    if (s >= 0)
        close(s);
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

void power_off(const char *why)
{
    klog("switching off: %s", why);
    sync();
    reboot(RB_POWER_OFF);
    for (;;)
        pause();
}

/* Power pressed twice within this many seconds switches the headset off */
#define DOUBLE_PRESS 0.6

static void *power_button_thread(void *arg)
{
    (void)arg;
    if (input_open() == 0) {
        klog("no buttons found: pressing Power twice won't switch the headset off");
        return NULL;
    }
    double last = -10;
    for (;;) {
        if (input_key(-1) != KEY_POWER)
            continue;
        if (now() - last < DOUBLE_PRESS)
            power_off("Power pressed twice");
        last = now();
    }
    return NULL;
}

/* What BusyBox's reboot (SIGTERM), halt (SIGUSR1) and poweroff (SIGUSR2) ask of PID 1 */
static volatile sig_atomic_t asked;

static void on_signal(int sig)
{
    asked = sig;
}

struct display_job {
    double degrees[2];   /* per-eye roll correction, counterclockwise as the viewer sees it */
    double deadline;
    int calibrate;
};

/* Set when the hardware bring-up has finished: the boot animation then gives way to the status screen */
static volatile int system_ready;

/* The displays run on their own thread, so init's main loop is free to look after processes */
static void *display_thread(void *arg)
{
    struct display_job *job = arg;
    struct fb fb;
    if (make_fb_node() != 0) {
        klog("no framebuffer: /sys/class/graphics/fb0/dev unreadable (%s)", strerror(errno));
    } else if (job->calibrate) {
        if (calibrate_run("/dev/fb0", job->degrees, job->deadline) != 0)
            klog("calibration failed");
    } else if (fb_open(&fb, "/dev/fb0") == 0) {
        /* One framebuffer for both, so the handover doesn't flash black */
        double roll[2] = { job->degrees[0] * M_PI / 180, job->degrees[1] * M_PI / 180 };
        if (bootanim_play("/afterglow/boot.anim", &fb, roll, job->deadline, &system_ready) != 0)
            klog("boot animation failed");
        if (status_run(&fb, roll, job->deadline) != 0)
            klog("status screen failed");
    }
    return NULL;
}

/*
 * The hardware bring-up: firmware, Wi-Fi, afterglowd (/afterglow/bringup.sh all), detached, logging to
 * /tmp/bringup.log. Off with afterglow.bringup=0 on the kernel command line.
 */
static pid_t spawn_bringup(void)
{
    static char *const argv[] = { "sh", "/afterglow/bringup.sh", "all", NULL };
    static char *const envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", "HOME=/", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        /* Only async-signal-safe calls between fork and exec: init has other threads */
        setsid();
        int in = open("/dev/null", O_RDONLY), out = open("/tmp/bringup.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in < 0 || out < 0)
            _exit(127);
        dup2(in, 0), dup2(out, 1), dup2(out, 2);
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    if (pid < 0)
        klog("fork bring-up: %s", strerror(errno));
    return pid;
}

/* A BusyBox shell on the development serial port, as the session leader with it as its terminal */
static pid_t spawn_shell(const char *tty)
{
    static char *const argv[] = { "sh", NULL };
    static char *const envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", "HOME=/", "TERM=vt100",
                                  "PS1=afterglow:\\w# ", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        /* Only async-signal-safe calls between fork and exec: init has other threads */
        setsid();
        int fd = open(tty, O_RDWR);
        if (fd < 0)
            _exit(127);
        ioctl(fd, TIOCSCTTY, 0);
        dup2(fd, 0), dup2(fd, 1), dup2(fd, 2);
        if (fd > 2)
            close(fd);
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    if (pid < 0)
        klog("fork shell: %s", strerror(errno));
    return pid;
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    make_node("/dev/kmsg", 1, 11);
    make_node("/dev/console", 5, 1);
    /* Usable by everyone, as on any Linux system: Android daemons that drop root need them */
    static const struct { const char *path; unsigned major, minor; } shared[] = {
        { "/dev/null", 1, 3 }, { "/dev/zero", 1, 5 }, { "/dev/full", 1, 7 },
        { "/dev/random", 1, 8 }, { "/dev/urandom", 1, 9 }, { "/dev/tty", 5, 0 },
    };
    for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); i++) {
        make_node(shared[i].path, shared[i].major, shared[i].minor);
        chmod(shared[i].path, 0666);
    }
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    klog("init started");
    loopback_up();
    if (usb_console_start() != 0)
        klog("no USB console");

    double seconds = cmdline_number("afterglow.test_seconds", 0);
    double deadline = seconds > 0 ? now() + seconds : 0;
    if (seconds > 0)
        klog("test build: restarting after %.0f s", seconds);

    double factory[2];
    read_factory_roll(factory);
    static struct display_job job;
    job.degrees[0] = cmdline_number("afterglow.roll_left_deg", 0);
    job.degrees[1] = cmdline_number("afterglow.roll_right_deg", 0);
    job.deadline = deadline;
    job.calibrate = cmdline_number("afterglow.calibrate", 0) != 0;
    pthread_t display, power_button;
    if (pthread_create(&display, NULL, display_thread, &job) == 0)
        pthread_detach(display);
    /* (Calibration reads the buttons itself) */
    if (!job.calibrate && pthread_create(&power_button, NULL, power_button_thread, NULL) == 0)
        pthread_detach(power_button);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);

    /* As PID 1: reap every exited process, keep the development shell running, end test builds */
    const char *shell_tty = usb_shell_tty();
    pid_t shell = -1, bringup = -1;
    if (cmdline_number("afterglow.bringup", 1) != 0)
        bringup = spawn_bringup();
    double next_shell = 0;
    for (;;) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid == shell) {
                klog("shell exited (status %d)", status);
                shell = -1;
            } else if (pid == bringup) {
                klog("bring-up finished (status %d; log /tmp/bringup.log)", status);
                bringup = -1;
                system_ready = 1;
            }
        }
        if (shell_tty && shell < 0 && now() >= next_shell) {
            shell = spawn_shell(shell_tty);
            next_shell = now() + 1;   /* at most one restart a second */
        }
        if (asked == SIGTERM)
            restart(NULL);
        else if (asked)
            power_off(asked == SIGUSR1 ? "halt" : "poweroff");
        if (deadline > 0 && now() >= deadline) {
            char target[32];
            restart(cmdline_string("afterglow.restart_to", target, sizeof(target)));
        }
        usleep(200 * 1000);
    }
}
