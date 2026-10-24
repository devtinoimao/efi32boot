#!/usr/bin/env python3
"""Generate the build time constants of the embedded 32 bit firmware.

The 32 bit EDK2 firmware (OVMF32) is linked for a fixed flash address, so its
modules are *not* position independent.  This script inspects the firmware
image, finds the SEC core module and extracts

  * the flash base address the image has to be copied to,
  * the file offset and the entry point RVA of the SEC core,
  * the base address the SEC module is linked for (needed to relocate it when
    the image is moved to a low memory address),
  * the temporary RAM base/size and the stack top that the IA32 SEC entry
    stub (OvmfPkg/Sec/Ia32/SecEntry.nasm) expects,
  * the location of the base relocation directory of the SEC module.

The result is written as a C header.
"""
import struct
import sys

FV_SIG = 0x4856465F


def find_fv_containing(data, off):
    """Return the file offset of the firmware volume containing *off*."""
    start = 0
    while start + 0x48 <= len(data):
        if struct.unpack_from("<I", data, start + 0x28)[0] == FV_SIG:
            fvlen = struct.unpack_from("<Q", data, start + 0x20)[0]
            if start <= off < start + fvlen:
                return start
            start += (fvlen + 7) & ~7
        else:
            start += 8
    return 0


def find_fv(data):
    off = 0
    while off + 0x48 <= len(data):
        if struct.unpack_from("<I", data, off + 0x28)[0] == FV_SIG:
            return off
        off += 8
    return None


def find_pe32(data):
    """Return (file_offset, pe_bytes) of the first uncompressed PE32 module."""
    n = len(data)
    for i in range(0x40, n - 0x400):
        if data[i + 3] != 0x10:              # EFI_SECTION_PE32
            continue
        size = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)
        if size < 0x400 or size > 0x200000:
            continue
        b = i + 4
        if data[b:b + 2] != b"MZ":
            continue
        e = struct.unpack_from("<I", data, b + 0x3C)[0]
        if e > 0x400 or b + e + 0x100 > n:
            continue
        if data[b + e:b + e + 4] != b"PE\x00\x00":
            continue
        mach = struct.unpack_from("<H", data, b + e + 4)[0]
        nsec = struct.unpack_from("<H", data, b + e + 6)[0]
        optsz = struct.unpack_from("<H", data, b + e + 20)[0]
        magic = struct.unpack_from("<H", data, b + e + 24)[0]
        if mach != 0x14C or magic != 0x10B or nsec == 0 or nsec > 8:
            continue
        if optsz < 0xE0:
            continue
        return b, data[b:b + size]
    return None, None


def pe_info(pe):
    e = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, e + 6)[0]
    optsz = struct.unpack_from("<H", pe, e + 20)[0]
    entry = struct.unpack_from("<I", pe, e + 0x28)[0]
    base = struct.unpack_from("<I", pe, e + 0x34)[0]
    sect = e + 24 + optsz
    # data directory 5 (IMAGE_DIRECTORY_ENTRY_BASERELOC); the directories
    # start at offset 0x60 of the PE32 optional header
    reloc_rva = struct.unpack_from("<I", pe, e + 24 + 0x60 + 5 * 8)[0]
    reloc_size = struct.unpack_from("<I", pe, e + 24 + 0x60 + 5 * 8 + 4)[0]
    return entry, base, reloc_rva, reloc_size


def find_sec_entry_constants(pe, entry_rva):
    """Scan the SEC entry stub for the PCD immediates.

    OvmfPkg/Sec/Ia32/SecEntry.nasm looks like
        mov  eax, PcdInitValueInTempStack
        mov  edi, PcdOvmfSecPeiTempRamBase
        mov  ecx, PcdOvmfSecPeiTempRamSize / 4
        ...
        mov  eax, PcdOvmfSecPeiTempRamBase + PcdOvmfSecPeiTempRamSize
        mov  esp, eax
    """
    code = pe[entry_rva:entry_rva + 0x60]
    init_value = temp_base = temp_size_dwords = stack_top = None
    i = 0
    while i < len(code) - 5:
        if code[i] == 0xB8:                       # mov eax, imm32
            val = struct.unpack_from("<I", code, i + 1)[0]
            if init_value is None and val not in (0,):
                init_value = val
            i += 5
            continue
        if code[i] == 0xBF:                       # mov edi, imm32
            temp_base = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
            continue
        if code[i] == 0xB9:                       # mov ecx, imm32
            temp_size_dwords = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
            continue
        if code[i] == 0x89 and code[i + 1] == 0xC4:   # mov esp, eax
            stack_top = init_value if False else None
            i += 2
            continue
        i += 1

    # second pass: the value that ends up in esp
    i = 0
    while i < len(code) - 5:
        if code[i] == 0xB8:
            val = struct.unpack_from("<I", code, i + 1)[0]
            if val != init_value and val != temp_base:
                stack_top = val
            i += 5
            continue
        i += 1

    return init_value, temp_base, temp_size_dwords, stack_top


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    data = open(src, "rb").read()

    fv_off = find_fv(data)
    if fv_off is None:
        raise SystemExit("no firmware volume in %s" % src)

    pe_off, pe = find_pe32(data)
    if pe is None:
        raise SystemExit("no PE32 module in %s" % src)

    entry_rva, pe_base, reloc_rva, reloc_size = pe_info(pe)
    init_value, temp_base, temp_dwords, stack_top = find_sec_entry_constants(pe, entry_rva)

    # The FV that contains the SEC core module.  The SEC entry code scans
    # this volume (passed in EBP) for the SEC/PEI core FFS files, so the
    # handoff must point at the SECFV, not at the start of the image.
    sec_fv_off = find_fv_containing(data, pe_off)

    # The module is linked for the flash address of the image.  The flash base
    # is the difference between the link address of the module and its offset
    # inside the file.
    flash_base = pe_base - pe_off
    temp_size = temp_dwords * 4 if temp_dwords else 0

    print("firmware volume      : 0x%06x" % fv_off)
    print("SEC FV file offset   : 0x%06x" % sec_fv_off)
    print("SEC module file off  : 0x%06x" % pe_off)
    print("SEC entry RVA        : 0x%05x" % entry_rva)
    print("SEC link base        : 0x%08x" % pe_base)
    print("flash base           : 0x%08x" % flash_base)
    print("temp ram base/size   : 0x%08x / 0x%x" % (temp_base or 0, temp_size))
    print("stack top            : 0x%08x" % (stack_top or 0))
    print("reloc dir rva/size   : 0x%05x / 0x%x" % (reloc_rva, reloc_size))
    print("image size           : 0x%x" % len(data))

    with open(dst, "w") as f:
        f.write("/* Generated by tools/gen_ovmf32.py - do not edit. */\n")
        f.write("#ifndef OVMF32_CONSTANTS_H\n#define OVMF32_CONSTANTS_H\n\n")
        f.write("#define OVMF32_FLASH_BASE        0x%08XU\n" % flash_base)
        f.write("#define OVMF32_IMAGE_SIZE        0x%08XU\n" % len(data))
        f.write("#define OVMF32_SEC_FV_OFFSET     0x%08XU\n" % sec_fv_off)
        f.write("#define OVMF32_SEC_FILE_OFFSET   0x%08XU\n" % pe_off)
        f.write("#define OVMF32_SEC_ENTRY_RVA     0x%08XU\n" % entry_rva)
        f.write("#define OVMF32_SEC_LINK_BASE     0x%08XU\n" % pe_base)
        f.write("#define OVMF32_SEC_RELOC_RVA     0x%08XU\n" % reloc_rva)
        f.write("#define OVMF32_SEC_RELOC_SIZE    0x%08XU\n" % reloc_size)
        f.write("#define OVMF32_TEMP_RAM_BASE     0x%08XU\n" % (temp_base or 0))
        f.write("#define OVMF32_TEMP_RAM_SIZE     0x%08XU\n" % temp_size)
        f.write("#define OVMF32_STACK_TOP         0x%08XU\n" % (stack_top or 0))
        f.write("#define OVMF32_INIT_STACK_VALUE  0x%08XU\n" % (init_value or 0))
        f.write("\n#endif\n")


if __name__ == "__main__":
    main()
