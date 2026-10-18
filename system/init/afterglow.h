#ifndef AFTERGLOW_H
#define AFTERGLOW_H

/* Log a line to the kernel log. */
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Seconds on CLOCK_MONOTONIC. */
double now(void);

/* Restart: to Android (target NULL), or into a bootloader mode such as "bootloader" (fastboot). */
void restart(const char *target) __attribute__((noreturn));

/* USB serial console: kernel log out, commands in (see usb.c). Returns 0 once running. */
int usb_console_start(void);

/*
 * Play the pre-rendered stereo boot animation (packed by tools/pack_anim.py) on the framebuffer:
 * the intro once, then the loop, until the deadline (now() seconds; 0 = forever). Each eye's image
 * is turned by roll[eye] radians (counterclockwise as the viewer sees it) about its lens centre,
 * to cancel that panel's roll. Returns 0 when the deadline is reached, -1 on error.
 */
int bootanim_play(const char *anim_path, const char *fb_path, const double roll[2], double deadline);

/*
 * Interactive display roll calibration with the headset's buttons (see calibrate.c), starting from
 * and updating degrees[eye], until the deadline. Returns 0 at the deadline, -1 on error.
 */
int calibrate_run(const char *fb_path, double degrees[2], double deadline);

/* Open every input device; returns how many. */
int input_open(void);
/* Wait up to timeout_ms for a key press; returns its KEY_* code, or 0. */
int input_key(int timeout_ms);

#endif
