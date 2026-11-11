# Building efi32boot

A short, honest walkthrough. If you only want the loader, you need about two
minutes. Rebuilding the embedded firmware takes longer and needs podman.

## What you need

- Python 3
- `make`
- MinGW-w64 cross compilers (`x86_64-w64-mingw32-gcc` and
  `i686-w64-mingw32-gcc`)
- NASM
- `mtools` (only for the QEMU tests)

The repo has `setup_toolchain.sh`, which drops a private toolchain into
`./toolchain` so you don't have to install anything system-wide:

```bash
./setup_toolchain.sh ./toolchain
export PATH="$PWD/toolchain/usr/bin:$PATH"
```

If you already have those tools installed, skip it and just make sure they are
on `PATH`.

## 1. Just build the loader

```bash
git clone --recurse-submodules https://github.com/devtinoimao/efi32boot.git
cd efi32boot
make all
```

That produces `BOOTX64.EFI`. `OVMF32.fd` is checked in, and
`tools/gen_ovmf32.py` regenerates `build/ovmf32_constants.h` from it
automatically. You do **not** need EDK2 for this step.

Two paths are searched for the shared header: `shared/` inside this repo and
`../shared` in the combined workspace. Both work.

## 2. Rebuild the embedded firmware (optional)

The `edk2` submodule points at the `hosted-firmware` branch of
[devtinoimao/edk2](https://github.com/devtinoimao/edk2/tree/hosted-firmware).
It contains the platform-neutral OVMF changes plus the local IA32 build
settings. If you didn't clone recursively:

```bash
git submodule update --init --depth 1 edk2
```

Firmware builds run inside `localhost/edk2-builder:bookworm` (GCC 12, NASM,
iasl, EDK2 BaseTools). From the combined workspace:

```bash
bash shared/rebuild_ovmf.sh
```

The script applies the (idempotent) portability patch, builds IA32 OVMF,
copies `OVMF.fd` to `OVMF32.fd`, regenerates the constants header and relinks
the loader. In a standalone clone it finds the submodule at `./edk2` and only
builds the firmware this repo needs.

`shared/patch_ovmf_platform.py` is the source of truth for the EDK2 change;
`shared/patches/ovmf-platform-portability.patch` is a plain `git apply`-able
diff if you prefer to patch by hand.

One gotcha: generated `Conf/BuildEnv.sh` and `Build/*/GNUmakefile` can keep
absolute paths from a previous workspace. The rebuild script repairs
`BuildEnv.sh` and clears the per-arch `Build/` directory before each build.

## 3. Check that it works

```bash
./test_qemu.sh -M pc      # or -M q35
```

It builds a fresh ESP with nothing but the loader on it, boots QEMU, and
checks that the `ABCDEFGH` hand-off trace, the inner SEC entry and DXE all
appear — with no asserts. No OS files required.

To boot a real OS, attach an ISO you supply yourself. See the README for the
full QEMU command.

## If something breaks

Read `build/serial.log` (or wherever you pointed `-serial`). The loader logs
the host-bridge ID, the discovered ACPI PM device, the memory map and the
firmware relocation delta before handing off. That log is the bug report.

`docs/ARCHITECTURE.md` explains the moving parts, `docs/SAFETY.md` explains
what not to do on real hardware.
