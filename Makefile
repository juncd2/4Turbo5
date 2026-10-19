# DMC5 Turbo Mode PRX for GoldHEN
TARGET := DMC5_turbo.prx

LIBS := -lSceLibcInternal -lkernel -lSceSysmodule -lScePad -lSceUserService

LOG_TYPE = -D__USE_KLOG__
DEBUG_FLAGS = -DDEBUG=0
EXTRAFLAGS := $(DEBUG_FLAGS) $(LOG_TYPE)

TOOLCHAIN := $(OO_PS4_TOOLCHAIN)
INTDIR := build
CFILES := source/main.c source/Patcher.c source/GoldHEN.c source/Utilities.c source/Syscall.c crt/crtprx.c
OBJS := $(patsubst %.c, $(INTDIR)/%.o, $(CFILES))

CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -Iinclude
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS)

UNUSED := $(shell mkdir -p $(INTDIR))

UNAME_S := $(shell uname -s)
CC := clang
LD := ld.lld
AR := llvm-ar

all: $(TARGET)

$(INTDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -o $@ $<

$(INTDIR)/turbo.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# create-fself writes the fake SELF directly to --lib=<name>; the -out file
# is only the intermediate raw ELF (the old Makefile copied that to the
# deployment name, which is why the loader rejected it with 0x80020008).
$(TARGET): $(INTDIR)/turbo.elf
	$(TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(INTDIR)/turbo.oelf --lib=$@ --paid 0x3800000000000011
	@echo "[+] built $@ (fake SELF)"

clean:
	rm -rf $(INTDIR) $(TARGET)

.PHONY: all clean
