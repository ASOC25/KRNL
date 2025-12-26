#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>

#define FRAMEBUFFER_DRIVER_MAJOR 5
#define FRAMEBUFFER_MAX_DEVICES 4

#define FB_BC 0xffffffff
#define FB_FC 0x00000000

struct framebuffer_colors {
    uint32_t background_color;
    uint32_t foreground_color;
};

struct framebuffer_pixel {
    uint32_t x;
    uint32_t y;
    uint32_t color;
};

#define FRAMEBUFFER_IOCTL_CLEARSCREEN 0x1
#define FRAMEBUFFER_IOCTL_SCROLL 0x2
#define FRAMEBUFFER_IOCTL_PUTPIXEL 0x3
#define FRAMEBUFFER_IOCTL_PUTCHAR 0x4
#define FRAMEBUFFER_IOCTL_GET_WIDTH 0x5
#define FRAMEBUFFER_IOCTL_GET_HEIGHT 0x6
#define FRAMEBUFFER_IOCTL_SETCOLORS 0x7

status_t framebuffer_init(void);
status_t framebuffer_init_pnp(void);

#endif
