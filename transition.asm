; ==========================================================================
;  transition.asm -- 64 bit long mode  ->  32 bit protected mode transition
; ==========================================================================

bits 64

; --- serial trace helper (COM1) --------------------------------------------
%macro TRACE 1
    mov     al, %1
    mov     dx, 0x3f8
    out     dx, al
%endmacro

global TransitionTo32BitAndJump
global TransitionCompatTrampoline
global TransitionCompatTrampolineEnd
global TransitionStackTop32
global TransitionGdtSlot32
global TransitionCompatTrampoline32
global TransitionHandover32
global TransitionBootArg32

; --------------------------------------------------------------------------
section .text

; --------------------------------------------------------------------------
;  void EFIAPI TransitionTo32BitAndJump(UINT64 HandoverPage,
;                                       UINT64 EntryPoint32,
;                                       UINT64 BootArg32,
;                                       UINT64 StackTop32)
;
;  RCX = handover page base (GDT at +0, trampoline at +0x100)
;  RDX = 32 bit physical entry point
;  R8  = boot argument (EBP of the 32 bit entry)
;  R9  = 32 bit stack top
;
;  All addresses come from registers: no .data globals are read, so the
;  transition does not depend on the EFI image's writable data surviving
;  ExitBootServices.
; --------------------------------------------------------------------------

TransitionTo32BitAndJump:
    cli
    cld
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    ; keep the arguments in callee-saved registers
    mov     r12, rcx                ; r12 = handover page base
    mov     r13, rdx                ; r13 = entry point
    mov     r14, r8                 ; r14 = boot arg (EBP)
    mov     r15, r9                 ; r15 = stack top (EDX)

    ; --- copy the 32 bit GDT template into the handover page --------------
    mov     rdi, r12
    lea     rsi, [rel gdt32_template]
    mov     ecx, gdt32_template_end - gdt32_template
    rep     movsb

    ; --- build the 10 byte GDTR right behind the copied table -------------
    mov     rax, r12
    mov     word [r12 + 32], gdt32_template_end - gdt32_template - 1
    mov     [r12 + 34], rax
    TRACE   'A'
    lgdt    [r12 + 32]
    TRACE   'B'

    ; --- register hand-off for the compatibility trampoline ---------------
    mov     ebp, r14d               ; EBP = boot arg
    mov     edx, r15d               ; EDX = 32 bit stack top
    mov     ecx, r13d               ; ECX = 32 bit entry point

    ; --- far jump into the 32 bit code segment ----------------------------
    ; jmp far [rsp] is the m16:64 form: 8 byte offset + 2 byte selector.
    ; This is the only form that actually changes CS (and therefore takes
    ; the CPU out of long mode) in 64 bit mode.  A near jmp or a retf
    ; leaves CS unchanged and faults instead.
    TRACE   'C'
    sub     rsp, 16
    lea     rax, [r12 + 0x100]     ; trampoline = handover page + 0x100
    mov     [rsp], rax
    mov     word [rsp + 8], 0x08
    jmp     far [rsp]

; --------------------------------------------------------------------------
;  Compatibility mode trampoline (copied into the handover page at +0x100).
; --------------------------------------------------------------------------
bits 32
TransitionCompatEntry:
TransitionCompatTrampoline:
    TRACE   'D'
    mov     esi, ecx            ; save the 32 bit entry point (ECX is clobbered below)
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    TRACE   'E'
    mov     esp, edx

    TRACE   'F'
    ; Clear CR0 NW/CD so caching is on (the outer firmware may have
    ; left them in a different state than the inner SEC expects).
    mov     eax, cr0
    and     eax, 0x7FFFFFF9
    or      eax, 0x00000001
    mov     cr0, eax
    TRACE   'G'

    mov     eax, cr4
    and     eax, 0xFFFFFFEF
    mov     cr4, eax

    ; Disable fixed-range MTRRs (BIT10 of IA32_MTRR_DEF_TYPE, MSR 0x2FF).
    ; The outer firmware enables them for the first 1 MiB; the inner
    ; PlatformPei asserts that they are off.
    mov     ecx, 0x2FF
    rdmsr
    and     eax, 0xFFFFFBFF
    wrmsr

    mov     ecx, 0xC0000080
    rdmsr
    and     eax, 0xFFFFFEFF
    wrmsr

    TRACE   'H'
    push    word 0x08
    push    esi                 ; EIP = saved 32 bit entry point
    retf

TransitionCompatTrampolineEnd:
bits 64

; --------------------------------------------------------------------------
section .data

; --- hand-off variables (written by main.c; kept for ABI compatibility) -----
TransitionStackTop32:        dq 0
TransitionGdtSlot32:         dq 0
TransitionCompatTrampoline32:dq 0
TransitionHandover32:        dq 0
TransitionBootArg32:          dq 0

; --- 32 bit GDT template -------------------------------------------------
gdt32_template:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt32_template_end:
