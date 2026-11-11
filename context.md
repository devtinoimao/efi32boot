# EFI32Boot development context (Nov 09 2026)

## Project Role
- THIS repo = "efi32boot" = 64-bit UEFI host that hands off to a 32-bit OVMF32
  firmware image. The SOLE REFERENCE for the target efi64boot project.
- Direction: 64-bit host -> 32-bit inner firmware. (Mirror of efi64boot but
  flipped mode-switch direction.)
- AI-assisted/experimental. Not production-safe.

See target side at: ../efi64boot (32-bit host -> 64-bit OVMF64).

## Goal
Host a real 32-bit EDK2 OVMF32 firmware inside a 64-bit UEFI boot application
so a 32-bit OS/UEFI-payout can run; machine configuration can use it as an
outer firmware when 64-bit UEFI plus a 32-bit target is required (e.g. Apple
Intel Macs with 32-bit EFI firmware + 64-bit CPU).

## Architecture
Outer 64-bit UEFI (gBS, gST, gRT live in BS-handles)
   runs BOOTX64.EFI (64-bit App, PE32+ x86-64)
   -> UefiInit discovers Boot/Run services + ConOut
   -> GetMemoryMap -> scan map -> Print map; AllocatePool for map buffer
   -> PrepareHostedFirmware() in main.c:
        - Load Ovmf32Image (embedded in blobs) below 4 GiB
          LoadPages(AllocateAnyPages...) EfiLoaderData -> firmware base
        - memcpy Ovmf32Image -> base
        - RelocateSecModule(sec = base + SEC_FILE_OFFSET, ...): apply
          base relocations in-place (delta = sec - link_base)
        - MEMFD reserve OVMF32_MEMFD_BASE 0x40000000 .. 0x41310000
          (OVMF32_MEMFD_SIZE 0x01310000, ~19 MiB)
        - Set FV base = base + OVMF32_SEC_FV_OFFSET
        - Set Entry = base + SEC_FILE_OFFSET + SEC_ENTRY_RVA
        - Set Stack Top = OVMF32_TEMP_RAM_BASE + OVMF32_TEMP_RAM_SIZE
          (0x40020000)
   -> ExitBootServices(...) retry loop (Allocate a GetMemoryMap buffer)
   -> TransitionTo32BitAndJump(handoverPage, entry, bootArg, stackTop)
     (see transition.asm):
        - copy 32-bit GDT template to handover page
        - lgdt; cli
        - EBP = boot arg, EDX = 32-bit stack top
        - far jmp through GDT[0x08] (32-bit compat code seg) into 32-bit mode
        - 32-bit compat code:
            - sets ax/ds/es/fs/gs = 0x10 (data)
            - mov esp = EDX (32-bit stack top)
            - clear CR0 NW/CD (and eax | 1; or 0x00000001)
            - clear CR4.PSE
            - disable fixed-range MTRR at 0x2FF bit10 (and eax 0xFFFFFBFF)
            - EFER.LME off
            - Source `TransitionTo32BitAndJump` param edges (all getenv)
            - jmp into 32-bit Windows boot manager early path
        - TRACE markers 'A'..'H' emitted at each step on COM1
   -> 32-bit OVMF SEC runs (Inner, on its own AcpiTimer)

## Inner Firmware Constants (build/ovmf32_constants.h)
- OVMF32_FLASH_BASE        0xFFC00000
- OVMF32_IMAGE_SIZE        0x00400000 (4 MiB)
- OVMF32_SEC_FV_OFFSET     0x003CC000
- OVMF32_SEC_FILE_OFFSET   0x003CC094
- OVMF32_SEC_ENTRY_RVA     0x000040B0
- OVMF32_SEC_LINK_BASE     0xFFFCC094
- OVMF32_SEC_RELOC_RVA     0x00005580
- OVMF32_SEC_RELOC_SIZE    0x00000200
- OVMF32_TEMP_RAM_BASE     0x40010000
- OVMF32_TEMP_RAM_SIZE     0x00010000 (64 KiB)
- OVMF32_STACK_TOP         0x40020000
- OVMF32_INIT_STACK_VALUE 0x5AA55AA5

Ovmf32Image is 4 MiB.

## Build
    make TOOLCHAIN_PREFIX=/path/to/mingw-w64-bin/ all
    -> BOOTX64.EFI (PE32+ / x86-64, 4.0 MiB, 64-bit EFI application)
    -> bootx32.efi

## Status
- M1-M2 stable under QEMU TCP/KVM (-cpu core2duo or qemu64, 2048 MiB).
- Validated: 64 -> 32 handoff completes to SEC, SEC messages visible on
  serial, inner OVMF32 boots Windows.
- VMware 0x7190 host bridge / real i5-4570 CSM untested.

## Recent work (snap)

> **SUPERSEDED 2026-11-10.** Items 1, 2 and the `patches/` line in "Known
> limitations" below describe the old, non-working VMware approach. The DID
> runtime override was removed and the patch file was replaced. See the
> 2026-11-10 section at the end of this file. Kept as history.

1. VMware host-bridge detection — logs "Host bridge PCI ID = 0xXXXXXXXX"
   via outl/inl (PCI config cycles) before handoff:
   QEMU = 0x12378086 (QEMU i440FX), VMware 0x8086:0x7190 (Intel 440BX), real
   unknown. Patch file `patches/acpitimer-vmware-0x7190.patch` maps 0x7190 to
   the PIIX4 path so the hosted OVMF32 AcpiRTCPtrLib passes.
2. VMware host-bridge runtime override — efi32boot overrides the host-bridge PCI
  ID to 0x1237 just before ExitBootServices if it detected VMware (temp).
3. COM1 reinit (115200 8N1) + Mark('1')..Mark('2') + "HOSTED 32-BIT
  FIRMWARE STARTING" print right before hand-off (diagnostics).
4. (ACPi) Work mode markers: "Ss" before EBS.
5. docs/README.md: supported-environments section.

## Known limitations
- VMware's host bridge (0x7190) would trip the inner OVMF32's
  BaseRomAcpiTimerLib assert (Unknown Host Bridge Device ID). Mitigated via
  runtime override AND a patch in `patches/acpitimer-vmware-0x7190.patch`.
  Both need VMware-side validation.
- CSM enabled means pure BIOS boot (no UEFI) -> BOOTX64.EFI never runs.
- memory-map driven allocation assumes EBS succeeds; if EBS fails after the
  first attempt we retry once.
- AciTimerLib issue: the embedded OVMF32 firmware itself expects QEMU's
  ACPI HW (see README "Building for VMware / real hardware").
- edk2 source tree does not build out-of-host-box (BaseTools missing); the
  patch is the best manual fix available.

## Files
- main.c (85 KB): UFI console, ConOut/FontOut serializer, Mark logging,
  PrepareHostedFirmware, ExitBootServices, debug logging, host-bridge id read
- transition.asm (236 lines): 64->32 transition + TRACE markers A..H
- build scripts
- patches/acpitimer-vmware-0x7190.patch

## Repository / Releases
- https://github.com/devtinoimao/efi32boot (HEAD: ae39a97)
- tag v1.0 at f1b64dd
- tag v0.2.1 at 4e02c70

## CI / Testing Verification
- QA-only validation with QEMU `-cpu core2duo` or `qemu64`, `-accel tcg/kvm`.
- Serial output captured via `-serial file:...` and PuTTY-style logs.
- Inner 32-bit firmware boots Windows with mode switch and ACPI timer init.

## Open items
1. Validate VMware host bridge workaround end-to-end (manual VM install +
   re-bake EFI firmware).
2. Patch `gen_ovmf32.py` to detect host bridge QEMU specifics (none needed
   for now).
3. Multi-arch roadmap (only x86 built so far).
4. Security hardening before any outside-HW deployment.

## References (sole reference for efi64boot project)
- efi32boot serves as the SOLE reference for efi64boot, mirroring it but in
  the reverse mode-switch direction. Its patterns:
  - `PrepareHostedFirmware()` — full-alloc of firmware image below 4G and
    MEMFD reservation at `OVMF32_MEMFD_BASE..SIZE`
  - `RelocateSecModule` — rebasing SEC FV in-place
  - `PatchSecTempRam` — temp-RAM base+size+stack top for the SEC stub
  - `AllocateBelow4G` (4G-capped allocator with max/free fallback)
  - `DeleteEspFile` (write0 + delete)
  - `ExitBootServices` retry loop
  - `Mark`/`Print` single-char COM1 debug bisects (and all A-Z trace letters)
- transition.asm A--H trace is the canonical bisect used in ALL efi64boot
  problems (General Protection fault → caught by its own per-vector IDT).

## 2026-11-10 -- VMware / Q35 fix (rebuilt firmware)

> **SUPERSEDED 2026-11-11** by the universal portability layer below.
> Kept as history: this section describes the earlier VMware-specific fix.

The old VMware fix was dead code three times over: it compared the raw 0xCFC
dword (VID in low word, DID in high word) against byte-swapped 0x80867190;
it wrote to port 0x0CFA instead of 0xCFE; and VID/DID are read-only PCI
config registers (verified with QEMU QMP: `o /w 0xcfe 0x1237` changes nothing).
The 2026-11-01 VMware log proves the real failure:
`AcpiTimerLibConstructor: Unknown Host Bridge Device ID: 0x7190` +
`ASSERT [SecMain] BaseRomAcpiTimerLib.c(65)` after all ABCDEFGH markers.

Fixes now in this repo:
- main.c includes ../shared/platform_quirks.h. Detection happens once in
  efi_main and logs platform, host bridge VID:DID and PM device B/D/F.
  The malformed nested detection block (with a literal backslash-r-n bug)
  and the DID-spoof block before ExitBootServices are gone.
- pq_enable_pm_io() enables ACPI PM I/O on the *probed* device and is a
  no-op when the host firmware already enabled it.
- patches/ovmf-platform-portability.patch (canonical: shared/patches/) replaces
  the malformed acpitimer-vmware-0x7190.patch. It normalizes 0x7190 -> 0x1237
  at every DID read and makes POWER_MGMT_REGISTER_PIIX4() discover the PM
  device (VMware 00:07.3, not QEMU 00:01.3). Q35 needs no source change.
- OVMF32.fd rebuilt from patched edk2-stable with the podman
  edk2-builder:bookworm image; constants regenerated (SEC_ENTRY_RVA 0x4130,
  SEC_RELOC_RVA 0x5600); BOOTX64.EFI relinked.
- Reproduce: `bash ../shared/rebuild_ovmf.sh`.

Validated under QEMU 10.2.2 TCG: -M pc -m 2048 and -M q35 -m 2048 both
reach inner SEC/PEI/DXE/BDS and EFI Internal Shell, no ASSERT, no
"Unknown Host Bridge".

NOT validated: an actual VMware boot (no VMware instance available). The
normalization and PM B/D/F discovery are source- and compile-verified only.

## 2026-11-11 -- universal, no per-machine build

The 2026-11-10 VMware alias is gone. There is now ONE firmware and ONE
loader for every host bridge; nobody applies a VMware patch or builds a
VMware-specific image.

- `../shared/patch_ovmf_platform.py` (canonical patch
  `../shared/patches/ovmf-platform-portability.patch`, copy in `patches/`):
  only DIDs with dedicated OVMF code pass through; EVERY other DID takes a
  generic `default:` fallback. No chipset allowlist. The ACPI PM device is
  found by PCI class code (06:80 / 06:01), any vendor, so PM registers,
  the enable register and its bit are resolved at runtime.
- main.c: DID is a log label only; PM device logged as `class %04x style %u`;
  `generic_fallback` just prints an informational line. Removed all
  "rebuild with the alias patch" messages.
- `OVMF32.fd` rebuilt (sha256 7ff77d21...), constants SEC_ENTRY_RVA 0x4300 /
  SEC_RELOC_RVA 0x5840, `BOOTX64.EFI` relinked.
- Reproduce everything: `bash ../shared/rebuild_ovmf.sh`.

Validated QEMU 10.2.2 TCG: `-M pc -m 2048` and `-M q35 -m 2048` both reach
inner BDS/Shell with no ASSERT. PM found as `00:01.3 class 0680` on pc and
`00:1f.0 class 0601` on q35. Real VMware not tested here.

## 2026-11-11 (evening) -- OS-neutral, VMware validated, packaged

- Scope correction: only the main C/asm WinPE code was removed. All test
  artifacts, ESP images and the demo GIF/MP4 were restored from Trash.
- This loader no longer names, loads or deletes any OS-specific file; it
  always hosts the inner firmware, whose BDS boots any EFI OS. efi64boot
  also dropped the DVD-first BootOrder override.
- REAL VMware Workstation 26.0.1 pass (serial log:
  efi32boot/vmware_universal_test.log): host bridge 8086:7190, ACPI PM at
  00:07.3 class 0680, generic fallback, ABCDEFGH -> SEC/PEI/DXE/BDS,
  zero ASSERTs and no Unknown Host Bridge. Original 0x7190 assert is gone.
- EDK2 fork https://github.com/devtinoimao/edk2 branch `hosted-firmware`
  (599a612f19) is now the `edk2` submodule in both repos. shared/ is
  vendored into each repo; Makefiles use -Ishared -I../shared.
- README/guides humanised, MIT LICENSE + NOTICE + SPDX tags added, all
  /tmp/opencode paths removed, ISO-based QEMU test commands documented
  (user supplies the ISO), proper compile instructions added.
- QEMU pc/q35 (and 12 GiB for efi64boot) re-validated after the cleanup.
