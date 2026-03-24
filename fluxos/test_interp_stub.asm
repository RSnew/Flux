; Stub file to test assembly of flux_interp.asm and multitask.asm
BITS 32

; Constants from main kernel
FS_ENTRY_SZ    equ 64
FS_NAME        equ 0
FS_DPTR        equ 32
FS_SIZE        equ 36
FS_TYPE        equ 40
FS_PAR         equ 41
FS_FILE        equ 1
FS_DIR         equ 2
COL_OUTPUT     equ 0x07
COL_ERR        equ 0x0C
COL_OK         equ 0x0A
COL_HIST_LBL   equ 0x06
COL_WHITE      equ 0x0F
COL_VER        equ 0x0A
COL_PROMPT     equ 0x0E
COL_BLUE       equ 0x09

section .text
global _start

; Stub functions
fs_find:        xor eax, eax
                dec eax
                ret
strcmp:          xor eax, eax
                ret
lp_print:       ret
lp_println:     ret
lp_newline:     ret
lp_putc:        ret
itoa:           ret

_start:
    jmp _start

; Include modules in .text section (they have their own section directives)
%include "flux_interp.asm"
%include "multitask.asm"

; Stub BSS (after includes so module sections are properly separated)
section .bss
fs_tab:        resb 64 * 64
fs_count:      resb 1
fs_cwd:        resb 1
heap_ptr:      resd 1
numbuf:        resb 16
arg1:          resb 128
arg2:          resb 256
