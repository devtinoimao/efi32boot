# efi32boot

A research UEFI loader for 64-bit x86_64 firmware that transitions the CPU into
32-bit protected mode and hosts a 32-bit OVMF firmware runtime.

This project was developed and tested inside QEMU. It is not production-safe.

## Goal

Provide a 64-bit UEFI application that can boot a 32-bit UEFI payload on a
64-bit UEFI-only machine. The hosted 32-bit firmware runtime is OVMF/EDK2
IA32, embedded into the boot application.

Proven so far:

- 64-bit UEFI to 32-bit protected-mode transition
- hosted OVMF32 runtime startup
- 32-bit GRUB reaches an interactive prompt
- 32-bit Windows media begin loading on QEMU with a rebuilt protected-policy
  firmware build

## Files

- `main.c` — UEFI bootstrap, memory allocation, compatibility-mode hand-off
- `transition.asm` — GDT, CR0/paging teardown, compatibility-mode jump
- `main.c` relocation logic for the embedded 32-bit firmware
- `tools/gen_ovmf32.py` — extracts EDK2/OVMF constants from a built firmware
  image
- `OVMF32*.fd` — **not included**; these must be supplied by the user
- `Makefile` — freestanding UEFI application build

## Build

Install the rootless toolchain first:

```bash
./setup_toolchain.sh /tmp/opencode/tc
export PATH=/tmp/opencode/tc/usr/bin:$PATH
```

Obtain a 32-bit DEBUG OVMF image separately, for example with:

```bash
build -p OvmfPkg/OvmfPkgIa32.dsc -a IA32 -b DEBUG -t GCC
```

Then extract constants:

```bash
python3 tools/gen_ovmf32.py /path/to/OVMF.fd build/ovmf32_constants.h
```

Build:

```bash
make TOOLCHAIN_PREFIX=/tmp/opencode/tc/usr/bin/ all
```

## QEMU test

Create an MBR-partitioned ESP containing:

- `/EFI/BOOT/BOOTX64.EFI` — the built loader
- optionally `BOOTIA32.EFI` or GRUB
- attach the desired ISO via `-cdrom`

Example disk-only hosted firmware test:

```bash
qemu-system-x86_64 -cpu qemu64 -m 2048 -smp 2 -nographic \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars/OVMF_VARS.fd \
  -drive format=raw,file=esp.img \
  -cdrom tiny10.iso
```

KVM with the real host CPU model is supported and verified
(`-accel kvm -cpu host` boots Windows Setup); it requires the OVMF32
FeatureControl MSR fix described in `docs/KVM_HOST_CPU.md`.

## Related projects and prior art

This project does not claim that the idea started here. Related prior concepts and
research include:

- EDK2 / Tianocore, especially `OvmfPkg` and the UEFI firmware architecture
  (source of the hosted 32-bit firmware environment)
- GRUB, `grub_cpu=i386` / `grub_platform=efi`
- Intel MultiArchUefiPkg, which runs non-native UEFI binaries with a CPU emulator
- CSMWrap, a standalone SeaBIOS/CSM wrapper project
- SeaBIOS in the CSM path
- Tianocore UEFI boot specification documentation
- QEMU-OVMF virtual hardware as the primary test environment

The implementation here is a compatibility-mode host attempt rather than a
software emulator or a legacy CSM.

## Demo

A QEMU demonstration of the hosted 32-bit firmware chain is included as:

- `media/efi32boot_qemu_demo.mp4`

The video shows the 64-bit UEFI loader, hosted OVMF32 startup, and 32-bit
Windows media booting in QEMU. It is provided as research evidence, not as a
production safety guarantee.


## AI-assisted development

This project is a heavily AI-assisted research prototype.

And the best part: AI made this — and the human side spent hours constraining
it, testing it, debugging real firmware failures, and proving what actually works
in QEMU.

Nope, it is not a pure “vibe code” dump. The tested behavior, recovery steps,
and limitations are documented.


## Safety warning

This project is experimental.

- Do not use it on a primary or production computer.
- Do not install it as the primary bootloader on real hardware.
- The II32 runtime and memory layout depend on the host firmware and memory
  map.
- A malformed deployment can prevent boot and may require recovery.
- No Secure Boot safety guarantees are currently provided.

## License

MIT License. See `LICENSE`.

## Prior note

This repository does not include Windows ISOs, firmware binaries purchased or
downloaded by the user, or generated disk images. Obtain these separately in
compliance with their respective licenses.
