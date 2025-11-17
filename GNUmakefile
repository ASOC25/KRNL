ABSDIR := $(shell pwd)
SRCDIR := $(ABSDIR)/src
BUILDDIR := $(ABSDIR)/build
OBJDIR := $(BUILDDIR)/obj
INCDIR := $(SRCDIR)/include

override CFLAGS +=       \
    -I.                  \
	-I$(INCDIR)          \
    -std=c11             \
    -ffreestanding       \
    -g                   \
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
override HEADER_DEPS := $(CFILES:.c=.d) $(ASFILES:.S=.d)

kernel: $(OBJS) link

$(OBJDIR)/apps/%.o: $(SRCDIR)/apps/%.c
#	@ echo !==== COMPILING $^
	@ mkdir -p $(@D)
	@$(CC) $(CFLAGS) -mgeneral-regs-only -c $^ -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
#	@ echo !==== COMPILING $^
	@ mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%_asm.o: $(SRCDIR)/%.asm
#	@ echo !==== COMPILING $^
	@ mkdir -p $(@D)
	@$(ASMC) $^ -felf64 -o $@

$(OBJDIR)/%_s.o: $(SRCDIR)/%.S
#	@ echo !==== COMPILING $^
	@ mkdir -p $(@D)
	@$(ASMC) $(NASMFLAGS) $^ -f elf64 -o $@

link: 
#	@ echo !==== LINKING $^
	@$(LD) $(LDFLAGS) -o $(BUILDDIR)/kernel.elf $(OBJS)

image: kernel
	@sudo $(ABSDIR)/scripts/make-efi-img.sh --force

run: image
	@sudo $(ABSDIR)/scripts/run-qemu.sh

debug: image
	@sudo $(ABSDIR)/scripts/debug-qemu.sh

clean:
	@sudo $(ABSDIR)/scripts/clean-artifacts.sh --mode normal

distclean:
	@sudo $(ABSDIR)/scripts/clean-artifacts.sh --mode dist

.PHONY: kernel link image run
.PHONY: kernel link image run debug clean distclean