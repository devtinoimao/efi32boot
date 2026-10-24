# Architecture

```text
64-bit UEFI firmware
        |
        v
BOOTX64.EFI
        |
        +-- allocate below-4GiB firmware image
        +-- relocate SEC module
        +-- reserve MEMFD region
        +-- transition to compatibility mode
        v
32-bit EDK2 / OVMF runtime
        |
        v
32-bit UEFI BDS
        |
        +-- GRUB32
        +-- Windows BOOTIA32.EFI (experimental)
```

The project does not emulate the CPU. Instead, the 64-bit loader enters
compatibility mode and lets the 32-bit firmware runtime run directly.
