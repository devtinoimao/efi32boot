# Please read this first

This is experimental firmware. It changes CPU mode underneath a running UEFI
environment and reserves fixed physical address ranges. When it works it is
rather elegant; when it doesn't, the machine simply stops with no display.

So:

- **Don't put it on a computer you need.** Not as the default boot entry, not
  on your only disk, not before a deadline.
- **Try it in QEMU first.** `./test_qemu.sh` takes under a minute and proves
  the hand-off without touching real hardware.
- **If you do try real hardware**, use a spare disk, keep an EFI Shell and
  recovery media within reach, and turn CSM off — with CSM on you may never
  reach UEFI at all.
- **Capture the serial log.** The loader prints the host-bridge ID, the ACPI
  PM device, the memory map and the relocation delta before it jumps. Without
  that log there is very little anyone can do to help.
- **No Secure Boot guarantees.** None.

The hosted 32-bit firmware depends on the host's memory map and low-memory
allocation behaviour, and those vary between machines. That is the main
reason real-hardware support is still listed as unvalidated.

The demo media in `media/` shows a successful QEMU run. It is evidence that
the chain can work, not a promise that it will work everywhere.
