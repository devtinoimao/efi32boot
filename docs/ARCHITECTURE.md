# Architecture

Think of it as lending the machine a different firmware for a few milliseconds.

```text
64-bit host firmware
        |
        v
BOOTX64.EFI  (a normal 64-bit EFI application)
        |
        +-- read the host memory map
        +-- copy the embedded OVMF32 image below 4 GiB
        +-- rebase its SEC core, reserve the MEMFD region
        +-- leave long mode -> compatibility mode -> 32-bit protected mode
        v
32-bit EDK2 / OVMF runtime
        |
        v
32-bit UEFI BDS
        |
        +-- any 32-bit EFI OS: Linux, BSD, Windows, GRUB, a shell, ...
```

No CPU emulation is involved. We do not interpret someone else's instructions
— we put the real processor into the mode the 32-bit firmware expects and let
it run natively.

## The one rule that keeps this simple

The loader knows nothing about operating systems. It never looks for a named
OS file, never changes `BootOrder`, and never deletes a file that belongs to
the OS. Everything after the hand-off is the inner firmware's job. That is
why one binary works for every 32-bit EFI OS, and why the QEMU smoke test can
pass with an ESP that contains nothing but the loader.

## Platform handling

The host-bridge VID/DID is read only to print a friendly name in the log. All
real decisions come from probing bus 0 for an ACPI power-management device by
PCI class code (`06:80` PIIX4-style, `06:01` ICH/LPC-style), any vendor.

The matching EDK2 change lives in the `edk2` submodule: host-bridge DIDs
without dedicated OVMF code take a conservative generic fallback, and the PM
register block is resolved at runtime. No per-machine build, no allowlist.

## The hand-off page

A single 4 KiB page below 4 GiB carries the 32-bit GDT, a small handover
descriptor and a copy of the compatibility trampoline. Once paging is off the
CPU can only fetch 32-bit linear addresses, so the code that performs the
final jump has to live there. `transition.asm` does that work; `main.c`
prepares the page.

## Where things can go wrong

- The host firmware's memory map decides where the firmware image and MEMFD
  region can go. A surprising map is the usual cause of an early hang.
- `OVMF32.fd` is linked for a fixed flash address. Its SEC core is rebased at
  load time using the relocation table extracted by `tools/gen_ovmf32.py`.
  If you rebuild the firmware, regenerate the constants header — the SEC
  offsets move.
- Real hardware has not been validated end-to-end. See `SAFETY.md`.
