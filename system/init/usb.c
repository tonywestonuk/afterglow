/*
 * USB serial console. Presents the headset to the computer it's plugged into as a CDC-ACM serial
 * port (a configfs gadget, like Android's adb setup; CONFIG_USB_CONFIGFS_ACM comes from
 * kernel/configs/afterglow.config) and streams the kernel log to it: /dev/cu.usbmodem* on a Mac,
 * /dev/ttyACM* on Linux. Lines typed into the port are commands; see run_command().
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "afterglow.h"

#define GADGET "/config/usb_gadget/afterglow"

static int tty = -1;

static int put(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, value, strlen(value)) < 0) {
        klog("usb: %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void tty_write(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(tty, s, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return;
        s += w, n -= w;
    }
}

/*
 * Forward every kernel log record ("priority,sequence,microseconds,flags;text") as
 * "[seconds] text". Writes block until the computer reads, so nothing is lost before it connects.
 */
static void *forward_kmsg(void *arg)
{
    (void)arg;
    int fd = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
    char record[2048], line[2200];
    while (fd >= 0) {
        ssize_t n = read(fd, record, sizeof(record) - 1);
        if (n < 0 && (errno == EINTR || errno == EPIPE))   /* EPIPE: older records were overwritten */
            continue;
        if (n <= 0)
            break;
        record[n] = '\0';
        unsigned long long usec = 0;
        char *text = strchr(record, ';');
        if (!text || sscanf(record, "%*u,%*u,%llu", &usec) != 1)
            continue;
        text++;
        int len = snprintf(line, sizeof(line), "[%5llu.%06llu] %.*s\r\n", usec / 1000000, usec % 1000000,
                           (int)strcspn(text, "\n"), text);   /* drop the record's KEY=VALUE lines */
        tty_write(line, len < (int)sizeof(line) ? (size_t)len : sizeof(line) - 1);
    }
    return NULL;
}

static void reply(const char *s)
{
    tty_write(s, strlen(s));
}

static void run_command(const char *cmd)
{
    klog("console command: %s", cmd);
    if (strcmp(cmd, "bootloader") == 0)
        restart("bootloader");
    else if (strcmp(cmd, "reboot") == 0)
        restart(NULL);
    else if (strcmp(cmd, "help") == 0)
        reply("commands: bootloader (restart to fastboot), reboot (restart to Android), help\r\n");
    else
        reply("unknown command; try help\r\n");
}

static void *read_commands(void *arg)
{
    (void)arg;
    char line[128];
    size_t len = 0;
    for (;;) {
        char c;
        ssize_t n = read(tty, &c, 1);
        if (n <= 0) {
            if (n < 0 && errno != EINTR)
                sleep(1);   /* not connected yet */
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            if (len)
                run_command(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
    return NULL;
}

/* The first (on the Quest 1, only) USB device controller, e.g. "a800000.dwc3" */
static int find_udc(char *name, size_t size)
{
    DIR *dir = opendir("/sys/class/udc");
    struct dirent *ent;
    int found = -1;
    while (dir && found != 0 && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] != '.') {
            snprintf(name, size, "%s", ent->d_name);
            found = 0;
        }
    }
    if (dir)
        closedir(dir);
    return found;
}

int usb_console_start(void)
{
    char udc[256], dev[32];   /* udc: a sysfs directory name, up to NAME_MAX */
    unsigned major, minor;
    mkdir("/config", 0755);
    if (mount("configfs", "/config", "configfs", 0, NULL) != 0 && errno != EBUSY) {
        klog("usb: mount configfs: %s", strerror(errno));
        return -1;
    }
    const char *dirs[] = { GADGET, GADGET "/strings/0x409", GADGET "/configs/c.1",
                           GADGET "/configs/c.1/strings/0x409", GADGET "/functions/acm.gs0" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) {
            klog("usb: mkdir %s: %s", dirs[i], strerror(errno));
            return -1;
        }
    }
    if (put(GADGET "/idVendor", "0x1d6b") ||                       /* Linux Foundation */
        put(GADGET "/idProduct", "0x0104") ||                      /* multifunction composite gadget */
        put(GADGET "/bcdDevice", "0x0100") || put(GADGET "/bcdUSB", "0x0200") ||
        put(GADGET "/strings/0x409/manufacturer", "afterglow") ||
        put(GADGET "/strings/0x409/product", "afterglow console") ||
        put(GADGET "/strings/0x409/serialnumber", "afterglow") ||
        put(GADGET "/configs/c.1/strings/0x409/configuration", "console") ||
        put(GADGET "/configs/c.1/MaxPower", "500"))
        return -1;
    if (symlink(GADGET "/functions/acm.gs0", GADGET "/configs/c.1/acm.gs0") != 0 && errno != EEXIST) {
        klog("usb: link acm.gs0: %s", strerror(errno));
        return -1;
    }
    if (find_udc(udc, sizeof(udc)) != 0 || put(GADGET "/UDC", udc) != 0) {
        klog("usb: no device controller to bind");
        return -1;
    }

    FILE *f = fopen("/sys/class/tty/ttyGS0/dev", "r");
    int ok = f && fscanf(f, "%u:%u", &major, &minor) == 2;
    if (f)
        fclose(f);
    snprintf(dev, sizeof(dev), "/dev/ttyGS0");
    if (!ok || (mknod(dev, S_IFCHR | 0600, makedev(major, minor)) != 0 && errno != EEXIST) ||
        (tty = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
        klog("usb: no ttyGS0: %s", strerror(errno));
        return -1;
    }
    struct termios t;
    if (tcgetattr(tty, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(tty, TCSANOW, &t);
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, forward_kmsg, NULL) == 0)
        pthread_detach(thread);
    if (pthread_create(&thread, NULL, read_commands, NULL) == 0)
        pthread_detach(thread);
    klog("usb console on %s (ttyGS0)", udc);
    return 0;
}
