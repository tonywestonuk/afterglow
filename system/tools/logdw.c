/*
 * logdw: a stand-in for Android's logd that forwards to the kernel log. Android binaries (Qualcomm's
 * daemons run from the headset's system partition) log by sending datagrams to /dev/socket/logdw;
 * this receives them and writes "<tag>: <message>" to /dev/kmsg, so they appear in afterglow's USB
 * console. Packet (Android 10 liblog): u8 log id, u16 thread id, u32 sec, u32 nsec, then for text
 * logs u8 priority, tag NUL, message NUL.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define HEADER 11   /* id + tid + realtime, packed */

int main(void)
{
    static const char *levels = "  VDIWEF";   /* Android priorities 2..7 */
    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = "/dev/socket/logdw" };
    mkdir("/dev/socket", 0755);
    unlink(addr.sun_path);
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (sock < 0 || kmsg < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("logdw");
        return 1;
    }
    chmod(addr.sun_path, 0666);

    char packet[4096], line[1100];
    for (;;) {
        ssize_t n = recv(sock, packet, sizeof(packet) - 1, 0);
        if (n <= HEADER + 1)
            continue;
        packet[n] = '\0';
        unsigned id = (unsigned char)packet[0];
        if (id == 2 || id == 4 || id == 6)   /* events, stats, security: binary records */
            continue;
        unsigned prio = (unsigned char)packet[HEADER];
        const char *tag = packet + HEADER + 1;
        const char *msg = tag + strnlen(tag, n - HEADER - 1) + 1;
        if (msg >= packet + n)
            msg = "";
        int len = snprintf(line, sizeof(line), "<6>android %c %s: %.900s\n",
                           prio < 8 ? levels[prio] : '?', tag, msg);
        if (len > 0 && write(kmsg, line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1) < 0)
            continue;
    }
}
