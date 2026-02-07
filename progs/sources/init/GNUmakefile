override PROGRAM := init.elf

ABSDIR := $(shell pwd)
SRCDIR := src
BUILDHOME := build
BUILDDIR := build/bin
OBJDIR := build/lib
SYMDIR := build/sym

DIRS := $(wildcard $(SRCDIR)/*)

rwildcard=$(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Check that given variables are set and all have non-empty values,
# die with an error otherwise.
#
# Params:
#   1. Variable name(s) to test.
#   2. (optional) Error message to print.
check_defined = \
    $(strip $(foreach 1,$1, \
        $(call __check_defined,$1,$(strip $(value 2)))))
__check_defined = \
    $(if $(value $1),, \
      $(error Undefined $1$(if $2, ($2))))

CFLAGS = -O2 -g -Wall -Wextra -Wpedantic -std=c11
NASMFLAGS = -F dwarf -g
LDFLAGS = -lc

override SRC_CFILES :=$(call rwildcard,$(SRCDIR),*.c)        
override SRC_ASFILES := $(call rwildcard,$(SRCDIR),*.S)
override SRC_NASMFILES := $(call rwildcard,$(SRCDIR),*.asm)

override OBJS += $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRC_CFILES))
override OBJS += $(patsubst $(SRCDIR)/%.S, $(OBJDIR)/%_s.o, $(SRC_ASFILES))
override OBJS += $(patsubst $(SRCDIR)/%.asm, $(OBJDIR)/%_asm.o, $(SRC_NASMFILES))

# You may comment @make run in this target if you intend
# to use virtualbox or real hardware.
all:
	@echo "Building program..."
	@echo "Absdir: $(ABSDIR)"
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(OBJDIR)
	@mkdir -p $(SYMDIR)
	@make program
	@echo "Generating symbol table..."
# Create symbol table for entering in gdb
	@objcopy --only-keep-debug $(BUILDDIR)/$(PROGRAM) $(SYMDIR)/$(PROGRAM).sym
	@echo "Done."

program: $(OBJS) link

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
	@$(LD) $(LDFLAGS) -o $(BUILDDIR)/$(PROGRAM) $(OBJS)

clean:
	@rm -rf $(OBJDIR)/*
	@rm -rf $(OBJS)
	@rm -rf $(BUILDDIR)/*
	@rm -rf $(SYMDIR)