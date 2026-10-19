/*
 * worldctl COMMAND...: send one command to the world's control socket and print its reply, from the
 * shell or afterglowd's run (e.g. worldctl list, worldctl recentre). afterglowd's tools use the same
 * socket (system/world/world.c lists the commands).
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: worldctl COMMAND [ARGUMENTS]\n");
        return 2;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/afterglow/world.sock");
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("worldctl: the world isn't running");
        return 1;
    }
    char line[1024] = "";
    for (int i = 1; i < argc; i++) {
        strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
        strncat(line, i + 1 < argc ? " " : "\n", sizeof(line) - strlen(line) - 1);
    }
    if (write(s, line, strlen(line)) < 0) {
        perror("worldctl");
        return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(s, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, n, stdout);
    close(s);
    return 0;
}
