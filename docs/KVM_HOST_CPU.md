# KVM with `-cpu host`

This project initially only worked under TCG with `-cpu qemu64`. Running with
`-accel kvm -cpu host` crashed the hosted 32-bit firmware with a `#GP` during
PEI. This document describes the root cause, the required EDK2 patch, and the
verified configuration.

Status: **verified 2026-10-24** on an Intel Core i7-10850H — Windows Setup
(Tiny10 x86) boots to the language-selection dialog with zero firmware
exceptions. AI-assisted experimental project; not production-safe.

## Root cause

Crash signature (inner OVMF32 debug serial):

```
!!!! IA32 Exception Type - 0D(#GP - General Protection)  CPU Apic ID - 00000001 !!!!
EIP  - 400339FD (PlatformPei.dll+0x4BD = wrmsr in AsmWriteMsr64)
EAX  - 00100005, ECX - 0000003A
```

`ECX = 0x3A` is `MSR_IA32_FEATURE_CONTROL`. The chain:

1. With `-cpu host`, QEMU exposes the host's VMX state and publishes the
   fw_cfg file `etc/msr_feature_control` (value e.g. `0x100005`).
2. The outer 64-bit OVMF (`OVMF_CODE.fd`) writes this value to MSR `0x3A` on
   all CPUs. Bit 0 is the lock bit — the MSR is now permanently locked.
3. The hosted 32-bit OVMF32 reads the same fw_cfg file and its
   `OvmfPkg/PlatformPei/FeatureControl.c` (`WriteFeatureControl`) writes the
   value blindly via `WRMSR` → `#GP` against the locked MSR.
4. Under `-cpu qemu64` the fw_cfg file does not exist, the HOB value stays 0
   and no MSR is written, which is why only `-cpu host` crashed.

A control test with `-cpu host,-vmx` still crashed the same way
(`EAX = 0x100001`): KVM locks the MSR from host state regardless. The fix has
to be in the OVMF32 source; QEMU flags cannot avoid it.

Note the DXE-side path (`UefiCpuPkg/Library/CpuCommonFeaturesLib`) already
uses test-then-write and is safe.

## Required EDK2 patch

In the OVMF32 source tree (`OvmfPkg/OvmfPkgIa32.dsc` build), patch
`OvmfPkg/PlatformPei/FeatureControl.c`, `WriteFeatureControl()`:

```c
  } else {
    MSR_IA32_FEATURE_CONTROL_REGISTER  Current;

    Current.Uint64 = AsmReadMsr64 (MSR_IA32_FEATURE_CONTROL);
    if (Current.Bits.Lock) {
      //
      // The MSR is locked (for example, by an outer firmware when this OVMF
      // build is hosted by another loader, or by KVM with -cpu host). Any
      // WRMSR would fault with #GP; the locked value already governs VMX.
      //
      return;
    }

    if (Current.Uint64 == PlatformInfoHob->FeatureControlValue) {
      return;
    }

    AsmWriteMsr64 (
      MSR_IA32_FEATURE_CONTROL,
      PlatformInfoHob->FeatureControlValue
      );
  }
```

Rebuild OVMF32 (container example, Debian bookworm + GCC 12):

```bash
podman run --rm -v /path/to/edk2:/edk2:Z -w /edk2 edk2-builder:bookworm bash -c \
  "export WORKSPACE=/edk2 && export PYTHONPATH=/edk2/BaseTools/Source/Python && \
   source edksetup.sh BaseTools >/dev/null && export CONF_PATH=/edk2/Conf && \
   build -p OvmfPkg/OvmfPkgIa32.dsc -a IA32 -b DEBUG -t GCC -D DEBUG_ON_SERIAL_PORT"
cp Build/OvmfIa32/DEBUG_GCC/FV/OVMF.fd /path/to/efi32boot/OVMF32.fd
```

Then rebuild the loader (`make clean && make all payload`) and refresh the ESP
with the new `BOOTX64.EFI`.

## ESP helper: `startup.nsh`

The hosted firmware's UEFI Shell drops to a prompt instead of auto-booting
the CD. Placing `tools/esp_startup.nsh` at the ESP root makes the Shell launch
the CD's 32-bit Windows Boot Manager automatically:

```
@echo -off
if exist FS1:\efi\boot\bootia32.efi then
FS1:\efi\boot\bootia32.efi
else
if exist FS0:\efi\boot\bootia32.efi then
FS0:\efi\boot\bootia32.efi
endif
endif
```

Copy it as `\startup.nsh` into the ESP FAT filesystem.

## Verified KVM `-cpu host` command

```bash
qemu-system-x86_64 \
  -accel kvm \
  -cpu host \
  -m 2048 \
  -smp 2 \
  -display gtk \
  -serial file:kvm_host_run.log \
  -boot order=cd \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars/OVMF_VARS.fd \
  -drive format=raw,file=esp_tiny_host.img \
  -cdrom "/path/to/Tiny10 B2.iso"
```

- The ESP must contain only `BOOTX64.EFI` (plus `startup.nsh`); no
  `BOOTIA32.EFI` in the ESP itself.
- Windows cdboot shows "Press any key" for ~10 s — press Enter in the GTK
  window, or for headless automation inject keys via a QEMU monitor
  (`sendkey kp_enter`); `tools/kvm_host_drive.py` is a one-command test
  driver that does the whole flow and captures screendumps.
