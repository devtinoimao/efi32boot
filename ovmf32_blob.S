/*
 * ovmf32_blob.S -- embeds the 32 bit EDK2 firmware into the 64 bit
 *                   application.
 *
 * The 32 bit firmware is hosted the same way a CSM module carries a legacy
 * BIOS: the image travels inside the loader and is started inside the 32 bit
 * environment that the loader prepares.  The image is linked for a fixed
 * flash address, so the loader relocates its SEC core when it copies the
 * firmware below the 4 GiB ceiling.
 */

    .section .rodata
    .balign 16
    .globl  Ovmf32Image
Ovmf32Image:
    .incbin "OVMF32.fd"
    .globl  Ovmf32ImageEnd
Ovmf32ImageEnd:

