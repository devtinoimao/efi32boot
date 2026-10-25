# Multi-architecture roadmap for efi32boot (64-bit firmware hosting 32-bit)

efi32boot on x86 hosts a **complete** 32-bit firmware (OVMF32) from a 64-bit
loader, then hands the machine over. Porting that *hosted-firmware* pattern
to other architectures pairs with the opposite direction, which lives in the
[efi64boot](https://github.com/devtinoimao/efi64boot) repo — see its
`docs/MULTIARCH.md` for the shared feasibility table.

What changes per architecture for THIS repo's direction (64-bit host → 32-bit
hosted firmware):

| Arch | A complete 32-bit UEFI firmware exists to host? | Verdict |
|------|--------------------------------------------------|---------|
| x86  | ✅ OVMF32 (we build it) — **done**               | ✅ shipped |
| ARM  | ✅ EDK2 `ArmPkg` AArch32 builds exist (real boards used them) | 🟡 best next port |
| PPC  | ❌ no PPC UEFI firmware exists (EDK2 has no PowerPC port) | 🔴 would require writing the hosted firmware itself |
| RISC-V | ❌ RV32 EDK2 support is minimal; RV64 is the real target | 🔴 research-only |

## ARM port sketch (the realistic one)

1. AArch64 loader (`BOOTAA64.EFI`) — capability gate (EL reported by
   `CurrentEL`; AArch32 firmware must run at EL1, so bail out of EL2+).
2. Reserve low memory for the hosted AArch32 firmware image + its page
   tables (EDK2 `Arm` build: short descriptors or LPAE — must match what
   the hosted build expects).
3. Relocate the hosted SEC/PEI image (mirroring `gen_ovmf32.py`'s constant
   extraction, but for the Arm build's FVs).
4. Trampoline: build AArch32 page tables, `ERET` with `SPSR.nRW=1` into the
   hosted 32-bit SEC entry with the handoff state in registers (the
   `transition.asm` analogue).
5. The hosted firmware then owns the machine exactly like OVMF32 does on
   x86 — QEMU `-M virt` with an AArch32 OVMF/EDK2 build is the test bench.

The x86 lessons transfer: extract-and-patch build constants, serial-first
debugging, `qemu -d int` for trampoline faults, and never call across the
width boundary without marshalling.

## CI note

CI runs from the web UI (not triggered from this machine per author
request). The existing workflow already cross-compiles with
aarch64/riscv64/ppc64le toolchains as a portability check; real ports get
their own jobs when they exist.
