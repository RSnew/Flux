; boot.asm — Multiboot2 bootloader for FluxOS
; Assembles to 32-bit protected mode, calls kernel_main

BITS 32

; ═══════════════════════════════════════════════════════════
; Multiboot1 Header (QEMU natively supports multiboot1)
; ═══════════════════════════════════════════════════════════
MBOOT_MAGIC     equ 0x1BADB002
MBOOT_FLAGS     equ 0x00000003
MBOOT_CHECKSUM  equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

; ═══════════════════════════════════════════════════════════
; Stack
; ═══════════════════════════════════════════════════════════
section .bss
align 16
stack_bottom:
    resb 16384          ; 16 KB stack
stack_top:

; ═══════════════════════════════════════════════════════════
; Entry Point
; ═══════════════════════════════════════════════════════════
section .text
global _start
extern kernel_main

_start:
    ; Set up stack
    mov esp, stack_top

    ; Push multiboot info pointer and magic for kernel_main
    push ebx            ; multiboot info structure pointer
    push eax            ; multiboot magic number

    ; Call the kernel
    call kernel_main

    ; Halt if kernel returns
.hang:
    cli
    hlt
    jmp .hang
