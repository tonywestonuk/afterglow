/*
 * restart [TARGET]: restart the headset now, into TARGET if given ("bootloader" for the fastboot menu),
 * as /init's restart does. For the shell and afterglowd: BusyBox's reboot signals PID 1, which
 * afterglow's /init doesn't handle, and can't name a target.
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    sync();
    if (argc > 1)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, argv[1]);
    else
        reboot(RB_AUTOBOOT);
    perror("restart");
    return 1;
}
