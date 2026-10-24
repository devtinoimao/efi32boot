; ==========================================================================
;  payload32.asm -- entry stub of the IA-32 (i386) Windows PE test payload
;
;  The 64 bit bootloader jumps to this code with a far jump after the CPU has
;  been switched to 32 bit protected mode with paging disabled:
;
;      EBX = address of the handover descriptor (BOOT32_HANDOVER)
;      ECX = entry point of this image
;      EDX = image size
;      ESP = 32 bit stack, 16 byte aligned
;
;  The stub unpacks the handover descriptor onto the stack and calls the C
;  part of the payload with the plain cdecl convention.
; ==========================================================================

bits 32

global payload_entry

;  On the i386 Windows ABI plain C symbols carry a leading underscore.
extern _payload_main

section .text

payload_entry:
    ; --- set up a stack frame for the C part ----------------------------
    push    ebp
    mov     ebp, esp

    ; --- push the handover arguments (cdecl: right to left) --------------
    push    dword [ebx + 4]          ; image base
    push    dword [ebx + 8]          ; entry point
    push    dword [ebx + 12]         ; image size
    push    dword ebx                ; handover descriptor

    call    _payload_main
    add     esp, 16

    pop     ebp

    ; --- leave QEMU with a well defined exit status ----------------------
    ; the isa-debug-exit device turns the written value into the process
    ; exit code (value << 1) | 1
    mov     dx, 0xf4
    mov     al, 0x21
    out     dx, al

.hang:
    cli
    hlt
    jmp     .hang
