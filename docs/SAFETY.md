# Safety

This project is experimental.

Do not install it as the primary boot loader on a production computer. The
hosted 32-bit firmware depends on the host UEFI memory map and low-memory
allocation behavior. Use QEMU first. On real hardware, use a spare disk and
an EFI Shell. Always keep recovery media.
