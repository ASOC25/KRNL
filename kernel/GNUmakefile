override KERNEL := kernel.elf

#RAMDISK VARIABLE MUST BE DEFINED BEFORE INCLUDING THIS MAKEFILE
ifneq ($(filter-out clean,$(MAKECMDGOALS)),)
ifndef RAMDISK
$(error RAMDISK variable is not defined. Please set it to the path of the ramdisk image.)
endif
else ifeq ($(MAKECMDGOALS),)
ifndef RAMDISK
$(error RAMDISK variable is not defined. Please set it to the path of the ramdisk image.)
endif
endif

KERNEL_ENTRY := _start

CFLAGS ?= -O0 -g -Wall -Werror -Wextra -pipe -Wno-packed-bitfield-compat -std=c11
NASMFLAGS ?= -F dwarf -g
LDFLAGS ?=
CPPFLAGS ?=

override HOME = .

SRCDIR := $(HOME)/src
INCDIR := $(SRCDIR)/include
BUILDDIR := $(HOME)/build
OBJDIR := $(BUILDDIR)/obj


override CFLAGS +=       \
    -I.                  \
	-I$(INCDIR)          \
    -std=c11             \
    -ffreestanding       \
    -fno-pie             \
    -fno-pic             \
    -m64                 \
    -Wno-int-conversion  \
    -fno-stack-protector \
    -march=x86-64        \
    -mabi=sysv           \
    -mno-80387           \
    -mno-red-zone        \
    -mcmodel=kernel      \
    -MMD

override LDFLAGS +=         \
    -nostdlib               \
    -static                 \
    -z max-page-size=0x1000 \
    -T linker.ld

override NASMFLAGS += \
    -f elf64

DIRS := $(wildcard $(SRCDIR)/*)
rwildcard=$(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
check_defined = \
    $(strip $(foreach 1,$1, \
        $(call __check_defined,$1,$(strip $(value 2)))))
__check_defined = \
    $(if $(value $1),, \
      $(error Undefined $1$(if $2, ($2))))

override CFILES :=$(call rwildcard,$(SRCDIR),*.c)        
override ASFILES := $(call rwildcard,$(SRCDIR),*.S)
override NASMFILES := $(call rwildcard,$(SRCDIR),*.asm)
override OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(CFILES))
override OBJS += $(patsubst $(SRCDIR)/%.S, $(OBJDIR)/%_s.o, $(ASFILES))
override OBJS += $(patsubst $(SRCDIR)/%.asm, $(OBJDIR)/%_asm.o, $(NASMFILES))
override HEADER_DEPS := $(OBJS:.o=.d)

RAMDISK_OBJ := $(OBJDIR)/ramdisk.o

kernel: $(OBJS) $(RAMDISK_OBJ) link

-include $(HEADER_DEPS)

$(RAMDISK_OBJ): $(RAMDISK)
	@objcopy -I binary -O elf64-x86-64 --rename-section .data=.ramdisk,alloc,load,contents,readonly $^ $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@ echo !==== COMPILING $<
	@ mkdir -p $(@D)
	@ $(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%_asm.o: $(SRCDIR)/%.asm
	@ echo !==== ASSEMBLING $<
	@ mkdir -p $(@D)
	@ nasm $(NASMFLAGS) $< -o $@

$(OBJDIR)/%_s.o: $(SRCDIR)/%.S
	@ echo !==== ASSEMBLING $<
	@ mkdir -p $(@D)
	@ $(CC) $(CFLAGS) -c $< -o $@

link:
	@ $(LD) $(LDFLAGS) -o $(BUILDDIR)/$(KERNEL) $(OBJS) $(RAMDISK_OBJ)
	@ nm --defined-only -n $(BUILDDIR)/$(KERNEL) | python3 $(HOME)/scripts/gen_ksyms.py > $(SRCDIR)/debug/kernel_syms.c
	@ $(CC) $(CFLAGS) -c $(SRCDIR)/debug/kernel_syms.c -o $(OBJDIR)/debug/kernel_syms.o
	@ $(LD) $(LDFLAGS) -o $(BUILDDIR)/$(KERNEL) $(OBJS) $(RAMDISK_OBJ)

clean:
	@ rm -rf $(BUILDDIR)

all: kernel
	@ echo !==== BUILD COMPLETE: $(BUILDDIR)/$(KERNEL)