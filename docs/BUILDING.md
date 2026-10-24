# Building

See README.md. The build needs:

- `OVMF32.fd`, a 32-bit firmware image
- Python 3
- MinGW-w64 GCC
- NASM

`tools/gen_ovmf32.py` extracts constants from the firmware image. The constants
are generated into `build/ovmf32_constants.h`.
