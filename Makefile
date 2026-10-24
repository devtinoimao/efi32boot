# ==========================================================================
#  Makefile -- 64 bit UEFI bootloader that hands control over to a 32 bit
#               Windows PE (WinPE x86) image.
#
#  Targets
#    all      -> BOOTX64.EFI   (the 64 bit UEFI application)
#    payload  -> bootx32.efi   (the IA-32 Windows PE test payload)
#    test     -> build everything and run the QEMU verification script
#    clean    -> remove the build directory
#
#  The cross compiler can be pointed at a private tool chain prefix:
#      make TOOLCHAIN_PREFIX=/opt/mingw/usr/bin/
# ==========================================================================

TOOLCHAIN_PREFIX ?=

CROSS_COMPILE   ?= $(TOOLCHAIN_PREFIX)x86_64-w64-mingw32-
CROSS_COMPILE32 ?= $(TOOLCHAIN_PREFIX)i686-w64-mingw32-

CC              := $(CROSS_COMPILE)gcc
OBJCOPY         := $(CROSS_COMPILE)objcopy
NASM            ?= $(TOOLCHAIN_PREFIX)nasm

CC32            := $(CROSS_COMPILE32)gcc
NASM32          ?= $(NASM)

TARGET          := BOOTX64.EFI
PAYLOAD         := bootx32.efi

BUILDDIR        := build

#
#  The 32 bit firmware that is hosted by the 64 bit application.  It is
#  embedded into BOOTX64.EFI, exactly the way a CSM module carries a legacy
#  BIOS: the image travels with the loader and is started inside the
#  32 bit environment the loader creates.
#
OVMF32          := OVMF32.fd
OVMF32_GEN      := $(BUILDDIR)/ovmf32_constants.h
OVMF32_OBJ      := $(BUILDDIR)/ovmf32_blob.o

# --------------------------------------------------------------------------
#  64 bit UEFI application
# --------------------------------------------------------------------------
#  -nostdlib/-nostartfiles/-nodefaultlibs : no CRT, no libc, freestanding
#  -fno-builtin                          : no calls into the C library
#  -mno-red-zone                         : the firmware may use the stack
#  -fshort-wchar                         : UEFI uses 16 bit wide characters
#  -ffunction-sections + --gc-sections    : drop unused code
CFLAGS  := -m64 -Os -g0 -std=gnu11 \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fno-tree-loop-distribute-patterns \
           -mno-red-zone -fshort-wchar \
           -ffunction-sections -fdata-sections \
           -I$(BUILDDIR) \
           -Wall -Wextra -Wno-unused-parameter

#  --subsystem,10   : EFI application
#  --entry,efi_main : the UEFI entry point
LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs \
           -Wl,--subsystem,10 \
           -Wl,--entry,efi_main \
           -Wl,--no-insert-timestamp \
           -Wl,--gc-sections

NASMFLAGS := -f win64

# --------------------------------------------------------------------------
#  IA-32 (i386) Windows PE payload
# --------------------------------------------------------------------------
CFLAGS32 := -m32 -Os -g0 -std=gnu11 \
            -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -fno-tree-loop-distribute-patterns \
            -mno-red-zone -fshort-wchar \
            -fno-pic \
            -Wall -Wextra -Wno-unused-parameter

LDFLAGS32 := -nostdlib -nostartfiles -nodefaultlibs \
             -Wl,--subsystem,10 \
             -Wl,--entry,payload_entry \
             -Wl,--no-insert-timestamp

NASMFLAGS32 := -f win32

# --------------------------------------------------------------------------
.PHONY: all payload test clean

all: $(TARGET)

payload: $(PAYLOAD)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# --- 64 bit UEFI application ---------------------------------------------
#  The constants of the hosted firmware (flash address, SEC entry point,
#  temporary RAM) are extracted from the firmware image itself.
$(OVMF32_GEN): tools/gen_ovmf32.py $(OVMF32) | $(BUILDDIR)
	python3 tools/gen_ovmf32.py $(OVMF32) $@

#  The firmware image is turned into an object file and linked into the
#  application, so that a single BOOTX64.EFI carries the complete
#  32 bit firmware.
$(OVMF32_OBJ): ovmf32_blob.S $(OVMF32) | $(BUILDDIR)
	$(CC) -m64 -c -I. -o $@ ovmf32_blob.S

$(BUILDDIR)/main.o: main.c $(OVMF32_GEN) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/transition.o: transition.asm | $(BUILDDIR)
	$(NASM) $(NASMFLAGS) -o $@ $<

$(TARGET): $(BUILDDIR)/main.o $(BUILDDIR)/transition.o $(OVMF32_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILDDIR)/main.o $(BUILDDIR)/transition.o $(OVMF32_OBJ)
	@echo "--- $@ ---"
	@$(CROSS_COMPILE)objdump -f $@ 2>/dev/null | head -4 || true

# --- IA-32 Windows PE payload --------------------------------------------
$(BUILDDIR)/payload32.o: payload32.c | $(BUILDDIR)
	$(CC32) $(CFLAGS32) -c -o $@ $<

$(BUILDDIR)/payload32_entry.o: payload32.asm | $(BUILDDIR)
	$(NASM32) $(NASMFLAGS32) -o $@ $<

$(PAYLOAD): $(BUILDDIR)/payload32.o $(BUILDDIR)/payload32_entry.o
	$(CC32) $(CFLAGS32) $(LDFLAGS32) -o $@ \
	    $(BUILDDIR)/payload32.o $(BUILDDIR)/payload32_entry.o

# --- verification ---------------------------------------------------------
test: all payload
	./test_winpe32.sh

clean:
	rm -rf $(BUILDDIR) $(TARGET) $(PAYLOAD)
