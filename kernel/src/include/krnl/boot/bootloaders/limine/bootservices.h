#ifndef _LIMINE_BOOTSERVICES_H
#define _LIMINE_BOOTSERVICES_H

#include <krnl/boot/bootloaders/abstract.h>
#include <krnl/boot/bootloaders/limine/limine.h>

struct bootloader_operations * get_boot_ops();

#endif
