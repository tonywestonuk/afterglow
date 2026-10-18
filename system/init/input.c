/*
 * Buttons. With no Android running, read the kernel's input devices directly: on the Quest 1,
 * Volume Down and Power come from "qpnp_pon" (the power management chip) and Volume Up from
 * "gpio-keys". Device nodes are created from sysfs, as there is no devtmpfs.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "afterglow.h"

#define MAX_INPUTS 16

static struct pollfd inputs[MAX_INPUTS];
static int n_inputs;

int input_open(void)
{
    DIR *dir = opendir("/sys/class/input");
    struct dirent *ent;
    mkdir("/dev/input", 0755);
    while (dir && n_inputs < MAX_INPUTS && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[300], name[80] = "?";
        unsigned major, minor;
        snprintf(path, sizeof(path), "/sys/class/input/%s/dev", ent->d_name);
        FILE *f = fopen(path, "r");
        int ok = f && fscanf(f, "%u:%u", &major, &minor) == 2;
        if (f)
            fclose(f);
        if (!ok)
            continue;
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", ent->d_name);
        if ((f = fopen(path, "r")) != NULL) {
            if (fgets(name, sizeof(name), f))
                name[strcspn(name, "\n")] = '\0';
            fclose(f);
        }
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        mknod(path, S_IFCHR | 0600, makedev(major, minor));
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            klog("open %s (%s): %s", path, name, strerror(errno));
            continue;
        }
        inputs[n_inputs++] = (struct pollfd){ .fd = fd, .events = POLLIN };
        klog("input %s: %s", ent->d_name, name);
    }
    if (dir)
        closedir(dir);
    return n_inputs;
}

int input_key(int timeout_ms)
{
    if (poll(inputs, n_inputs, timeout_ms) <= 0)
        return 0;
    for (int i = 0; i < n_inputs; i++) {
        struct input_event ev;
        while (read(inputs[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1)   /* pressed (not released or auto-repeat) */
                return ev.code;
        }
    }
    return 0;
}
