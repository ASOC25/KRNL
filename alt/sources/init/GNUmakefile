override NAME := init.elf

#SYSROOT VARIABLE MUST BE DEFINED BEFORE INCLUDING THIS MAKEFILE
ifneq ($(filter-out clean,$(MAKECMDGOALS)),)
ifndef SYSROOT
$(error SYSROOT variable is not defined. Please set it to the path of the system root directory.)
endif
else ifeq ($(MAKECMDGOALS),)
ifndef SYSROOT
$(error SYSROOT variable is not defined. Please set it to the path of the system root directory.)
endif
endif

override HOME = .
SRCDIR = $(HOME)/src
BUILDDIR = $(HOME)/build
OBJDIR = $(BUILDDIR)/obj

# Tools Config
CFLAGS = -O2 -g -Wall -Wextra -Wpedantic -std=c11
NASMFLAGS = -F dwarf -g
LDFLAGS ?= -lc

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

init: $(OBJS) link

# Target
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@ echo !==== COMPILING $^
	@ mkdir -p $(@D)
	@ $(CC) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%_asm.o: $(SRCDIR)/%.asm
	@ echo !==== ASSEMBLING $^
	@ mkdir -p $(@D)
	@nasm $(NASMFLAGS) $^ -o $@

$(OBJDIR)/%_s.o: $(SRCDIR)/%.S
	@ echo !==== ASSEMBLING $^
	@ mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $^ -o $@

link:
	@ echo !==== SYSROOT: $(SYSROOT)
	@ $(CC) $(LDFLAGS) -o $(BUILDDIR)/$(NAME) $(OBJS)

clean:
	@ echo !==== CLEANING BUILD DIRECTORY $(shell realpath $(BUILDDIR))
	@ rm -rf $(BUILDDIR)

all: init