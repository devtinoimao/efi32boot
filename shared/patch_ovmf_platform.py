#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
patch_ovmf_platform.py -- make an edk2-stable OVMF tree platform-neutral.

No chipset allowlist, no per-machine build, no "VMware edition".  One firmware
image adapts to whatever host bridge and power-management device it finds.

Two changes, both generic:

1. OvmfHostBridgeDevId() (OvmfPlatforms.h)
   A handful of DIDs have *dedicated* OVMF code paths (i440FX, Q35,
   Cloud Hypervisor, microvm, bhyve).  Those pass through unchanged.  EVERY
   other DID -- VMware 440BX, another hypervisor, real silicon, a chipset
   that does not exist yet -- falls back to the conservative i440FX/PIIX4
   family path.  This is a fallback, not an allowlist: new hardware needs no
   source change and no separate build.  All downstream switches, HOBs and
   PcdOvmfHostBridgePciDevId consumers then see a DID OVMF already handles,
   so no `Unknown Host Bridge Device ID` ASSERT can fire.

2. Generic PM-device discovery (I440FxPiix4.h)
   Instead of assuming 00:01.3 and Intel DID 0x7113, scan bus 0 by PCI
   *class code*, any vendor:
       06:80 "other bridge" -> PIIX4-style PM (PMBA @0x40, PMIOSE @0x80:0)
       06:01 "ISA bridge"   -> ICH/LPC-style PM (PMBASE @0x40, ACPI_EN @0x44:7)
   Both expose the ACPI timer at PMBASE+8.  POWER_MGMT_REGISTER_PIIX4(),
   PIIX4_PMREGMISC and PIIX4_PMREGMISC_PMIOSE become runtime-resolved, so the
   same binary works at 00:01.3 (QEMU i440FX), 00:07.3 (VMware 440BX),
   00:1F.0 (ICH/Q35/LPC) and anywhere else.

VID/DID are HwInit (read-only) PCI config registers, so a bootloader cannot
fake them; the firmware itself has to be platform-neutral.  Q35 needs no
special case and is passed through.

Usage:  python3 patch_ovmf_platform.py /path/to/edk2-stable
"""
from pathlib import Path
import sys


def load(path):
    """Read as bytes, return (LF-normalized text, original newline style)."""
    raw = path.read_bytes()
    style = "\r\n" if b"\r\n" in raw else "\n"
    return raw.decode("utf-8", "surrogateescape").replace("\r\n", "\n"), style


def save(path, text, style):
    path.write_bytes(text.replace("\n", style).encode("utf-8", "surrogateescape"))


def edit(root, rel, anchor, replacement, label):
    path = root / rel
    if not path.is_file():
        raise SystemExit("missing file: %s" % path)
    text, style = load(path)
    if replacement in text:
        print("  skip    %-62s (already patched)" % rel)
        return
    if "OVMF-PLATFORM-PORTABILITY" in text:
        raise SystemExit("%s has an older/incompatible portability patch; "
                         "restore it from git first" % rel)
    if anchor not in text:
        raise SystemExit("anchor not found in %s for %s:\n%s"
                         % (rel, label, anchor[:120]))
    save(path, text.replace(anchor, replacement, 1), style)
    print("  patched %-62s %s [%s]" % (rel, label,
          "CRLF" if style == "\r\n" else "LF"))


I440_PM_BLOCK = r'''//
// OVMF-PLATFORM-PORTABILITY: generic, vendor-neutral power-management
// discovery.
//
// OVMF was written for QEMU's i440FX, where the ACPI PM function is at
// 00:01.3 with Intel DID 0x7113.  Other platforms put an equivalent device
// somewhere else (VMware 440BX: 00:07.3; ICH/LPC bridges: 00:1F.0) and may
// use a different vendor/device ID.  Rather than maintaining a chipset
// allowlist, discover the device from its PCI class code, which is defined
// by the PCI spec and is identical across vendors:
//
//   06:80 "other bridge" -> PIIX4-style PM (PMBA @0x40, PMIOSE @0x80 bit0)
//   06:01 "ISA bridge"   -> ICH-style PM   (PMBASE @0x40, ACPI_EN @0x44 bit7)
//
// Both expose the ACPI timer at PMBASE + 8.  VID/DID are HwInit (read-only)
// config registers, so a bootloader cannot fake this; the firmware itself
// has to be platform-neutral.
//
#define OVMF_PM_CLASS_PIIX4   0x0680
#define OVMF_PM_CLASS_ICH     0x0601
#define OVMF_PM_STYLE_PIIX4   1
#define OVMF_PM_STYLE_ICH     2

/**
  Find the power-management function on bus 0 and return the PCI_LIB_ADDRESS
  of `Offset` on it.  Scans all devices/functions of any vendor.  Prefers
  06:80 over 06:01 because a PIIX4 chipset has both an ISA bridge and a
  separate PM function, and the latter is the one with PMBA.
**/
STATIC
inline
UINTN
OvmfPmPciAddress (
  IN  UINT8  Offset
  )
{
  UINT8   Pass, Device, Function;
  UINT16  Want, VendorId, ClassCode;
  UINT32  ClassDword;

  for (Pass = 0; Pass < 2; Pass++) {
    Want = (Pass == 0) ? OVMF_PM_CLASS_PIIX4 : OVMF_PM_CLASS_ICH;
    for (Device = 0; Device < 32; Device++) {
      for (Function = 0; Function < 8; Function++) {
        VendorId = PciRead16 (PCI_LIB_ADDRESS (0, Device, Function, 0x00));
        if ((VendorId == 0xFFFF) || (VendorId == 0x0000)) {
          continue;                       // absent function
        }
        ClassDword = PciRead32 (PCI_LIB_ADDRESS (0, Device, Function, 0x08));
        ClassCode  = (UINT16)(ClassDword >> 16);
        if (ClassCode == Want) {
          return PCI_LIB_ADDRESS (0, Device, Function, Offset);
        }
      }
    }
  }

  return PCI_LIB_ADDRESS (0, 1, 3, Offset);   // conservative legacy default
}

/** Return OVMF_PM_STYLE_PIIX4 or OVMF_PM_STYLE_ICH for the discovered PM. **/
STATIC
inline
UINT8
OvmfPmStyle (
  VOID
  )
{
  UINT8   Device, Function;
  UINT16  VendorId;
  UINT32  ClassDword;

  for (Device = 0; Device < 32; Device++) {
    for (Function = 0; Function < 8; Function++) {
      VendorId = PciRead16 (PCI_LIB_ADDRESS (0, Device, Function, 0x00));
      if ((VendorId == 0xFFFF) || (VendorId == 0x0000)) {
        continue;
      }
      ClassDword = PciRead32 (PCI_LIB_ADDRESS (0, Device, Function, 0x08));
      if (((UINT16)(ClassDword >> 16)) == OVMF_PM_CLASS_PIIX4) {
        return OVMF_PM_STYLE_PIIX4;
      }
    }
  }

  return OVMF_PM_STYLE_ICH;
}

#define POWER_MGMT_REGISTER_PIIX4(Offset)  OvmfPmPciAddress (Offset)

#define PIIX4_PMBA       0x40
#define PIIX4_PMBA_MASK  (BIT15 | BIT14 | BIT13 | BIT12 | BIT11 |         \
                                  BIT10 | BIT9  | BIT8  | BIT7  | BIT6)

/* Style-aware ACPI-enable register and bit.  Keeping the historical macro
 * names means no caller anywhere in OvmfPkg needs to change. */
#define PIIX4_PMREGMISC \
  ((OvmfPmStyle () == OVMF_PM_STYLE_PIIX4) ? 0x80 : 0x44)
#define PIIX4_PMREGMISC_PMIOSE \
  ((OvmfPmStyle () == OVMF_PM_STYLE_PIIX4) ? BIT0 : BIT7)
'''

PLATFORMS_BLOCK = r'''#define OVMF_HOSTBRIDGE_DID \
  PCI_LIB_ADDRESS (0, 0, 0, PCI_DEVICE_ID_OFFSET)

//
// OVMF-PLATFORM-PORTABILITY: one firmware for every host bridge.
//
// These DIDs have *dedicated* OVMF code paths and pass through unchanged.
// EVERY other DID -- VMware 440BX, another hypervisor, real silicon, a
// chipset that has not been invented yet -- falls back to the conservative
// i440FX/PIIX4-family path.  This is a fallback, not an allowlist: adding
// support for a new machine requires no source edit, no patch and no
// separate build.  The PM registers are resolved from the real device by
// PCI class code (see I440FxPiix4.h), so the fallback is functionally
// correct rather than merely "does not ASSERT".
//
// VID/DID are read-only in config space, so this cannot be done by a
// bootloader at runtime; it belongs in the firmware.
//
STATIC
inline
UINT16
OvmfHostBridgeDevId (
  VOID
  )
{
  UINT16  DeviceId;

  DeviceId = PciRead16 (OVMF_HOSTBRIDGE_DID);
  switch (DeviceId) {
    case INTEL_82441_DEVICE_ID:      // QEMU i440FX / generic fallback value
    case INTEL_Q35_MCH_DEVICE_ID:    // QEMU Q35 / ICH9
    case CLOUDHV_DEVICE_ID:
    case MICROVM_PSEUDO_DEVICE_ID:
    case 0xffff:                     // microvm
    case 0x7432:                     // bhyve (AMD host bridge)
    case 0x1275:                     // bhyve (Intel host bridge)
      return DeviceId;
    default:
      return INTEL_82441_DEVICE_ID;
  }
}
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_ovmf_platform.py /path/to/edk2-stable")
    root = Path(sys.argv[1]).resolve()
    if not (root / "OvmfPkg").is_dir():
        raise SystemExit("not an edk2 tree: %s" % root)

    print("Patching %s (universal portability layer)" % root)

    # 1. Generic PM discovery by PCI class, replacing the hardcoded 00:01.3.
    pristine_pm = '''//
// B/D/F/Type: 0/1/3/PCI
//
#define POWER_MGMT_REGISTER_PIIX4(Offset)  PCI_LIB_ADDRESS (0, 1, 3, (Offset))

#define PIIX4_PMBA       0x40
#define PIIX4_PMBA_MASK  (BIT15 | BIT14 | BIT13 | BIT12 | BIT11 |         \\
                                  BIT10 | BIT9  | BIT8  | BIT7  | BIT6)

#define PIIX4_PMREGMISC         0x80
#define PIIX4_PMREGMISC_PMIOSE  BIT0
'''
    edit(root, "OvmfPkg/Include/IndustryStandard/I440FxPiix4.h",
         pristine_pm, I440_PM_BLOCK, "generic-pm-discovery")

    # 2. Generic DID fallback, replacing the fixed DID read at every site.
    pristine_did = '''#define OVMF_HOSTBRIDGE_DID \\
  PCI_LIB_ADDRESS (0, 0, 0, PCI_DEVICE_ID_OFFSET)
'''
    edit(root, "OvmfPkg/Include/OvmfPlatforms.h",
         pristine_did, PLATFORMS_BLOCK, "generic-did-fallback")

    # 3. Route every direct DID read through the resolver.  Everything
    #    downstream (HOBs, PCDs, switches) then inherits the generic
    #    behaviour with no per-platform case labels anywhere.
    read_sites = [
        "OvmfPkg/Bhyve/PlatformPei/Platform.c",
        "OvmfPkg/Library/ResetSystemLib/BaseResetShutdown.c",
        "OvmfPkg/Library/AcpiTimerLib/BaseAcpiTimerLib.c",
        "OvmfPkg/Library/AcpiTimerLib/BaseRomAcpiTimerLib.c",
        "OvmfPkg/Library/PeilessStartupLib/Hob.c",
        "OvmfPkg/Library/PeilessStartupLib/PeilessStartup.c",
        "OvmfPkg/SmmAccess/SmmAccessPei.c",
        "OvmfPkg/PlatformPei/Platform.c",
        "OvmfPkg/XenPlatformPei/Platform.c",
    ]
    for rel in read_sites:
        p = root / rel
        if not p.is_file():
            print("  skip    %-62s (not present in this tree)" % rel)
            continue
        text, style = load(p)
        if "OvmfHostBridgeDevId ()" in text:
            print("  skip    %-62s (already routed)" % rel)
            continue
        n = text.count("PciRead16 (OVMF_HOSTBRIDGE_DID)")
        if n == 0:
            print("  skip    %-62s (no direct DID read)" % rel)
            continue
        save(p, text.replace("PciRead16 (OVMF_HOSTBRIDGE_DID)",
                             "OvmfHostBridgeDevId ()"), style)
        print("  patched %-62s %d DID read(s) resolved" % (rel, n))

    print("Done. Rebuild OVMF IA32 and X64, then regenerate the embedded .fd blobs.")


if __name__ == "__main__":
    main()
