; kernel.asm — FluxOS bare-metal kernel (32-bit x86)
; Implements VGA text output, keyboard input, and a simple command shell
;
; This is the hand-written assembly equivalent of kernel.flux
; (Flux codegen does not yet handle bare-metal / no-OS targets)

BITS 32

; ═══════════════════════════════════════════════════════════
; Constants
; ═══════════════════════════════════════════════════════════
VGA_BASE    equ 0xB8000
VGA_COLS    equ 80
VGA_ROWS    equ 25
VGA_SIZE    equ VGA_COLS * VGA_ROWS * 2

; VGA color attributes
WHITE_ON_BLACK equ 0x0F
GREEN_ON_BLACK equ 0x0A
CYAN_ON_BLACK  equ 0x0B
YELLOW_ON_BLACK equ 0x0E

; Keyboard I/O ports
KBD_DATA    equ 0x60
KBD_STATUS  equ 0x64

; Scancode constants
SC_ENTER    equ 0x1C
SC_BACKSPACE equ 0x0E
SC_LSHIFT   equ 0x2A
SC_RSHIFT   equ 0x36

; ═══════════════════════════════════════════════════════════
; Data Section
; ═══════════════════════════════════════════════════════════
section .data

banner_line1: db "FluxOS v0.1 - Flux Language Bare Metal Kernel", 0
banner_line2: db "================================================", 0
banner_line3: db "Type 'help' for available commands.", 0
prompt:       db "fluxos> ", 0
help_msg1:    db "Available commands:", 0
help_msg2:    db "  help     - Show this help message", 0
help_msg3:    db "  clear    - Clear the screen", 0
help_msg4:    db "  info     - Show system information", 0
help_msg5:    db "  hello    - Print a greeting", 0
help_msg6:    db "  reboot   - Reboot the system", 0
info_msg1:    db "FluxOS v0.1 bare-metal microkernel", 0
info_msg2:    db "Architecture: x86 (32-bit protected mode)", 0
info_msg3:    db "VGA Text Mode: 80x25, 16 colors", 0
info_msg4:    db "Built with: NASM + Flux concepts", 0
hello_msg:    db "Hello from FluxOS! The Flux language says hi.", 0
unknown_msg:  db "Unknown command: ", 0
cmd_help:     db "help", 0
cmd_clear:    db "clear", 0
cmd_info:     db "info", 0
cmd_hello:    db "hello", 0
cmd_reboot:   db "reboot", 0

; Scancode to ASCII lookup table (unshifted)
; Index = scancode, Value = ASCII character (0 = no mapping)
scancode_table:
    db 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9  ; 0x00-0x0F
    db 'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0        ; 0x10-0x1D
    db 'a','s','d','f','g','h','j','k','l',';', 39, '`', 0, '\'     ; 0x1E-0x2B
    db 'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '       ; 0x2C-0x39

; Shifted scancode table
scancode_shift_table:
    db 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9  ; 0x00-0x0F
    db 'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0        ; 0x10-0x1D
    db 'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|'       ; 0x1E-0x2B
    db 'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' '       ; 0x2C-0x39

; ═══════════════════════════════════════════════════════════
; BSS Section
; ═══════════════════════════════════════════════════════════
section .bss
cursor_row: resb 1          ; current cursor row (0-24)
cursor_col: resb 1          ; current cursor column (0-79)
shift_held: resb 1          ; shift key state
cmd_buffer: resb 128        ; command input buffer
cmd_len:    resb 1          ; current command length

; ═══════════════════════════════════════════════════════════
; Text Section — Kernel Entry
; ═══════════════════════════════════════════════════════════
section .text
global kernel_main

kernel_main:
    ; Clear screen
    call clear_screen

    ; Display boot banner
    mov esi, banner_line1
    mov bl, CYAN_ON_BLACK
    call print_line

    mov esi, banner_line2
    mov bl, CYAN_ON_BLACK
    call print_line

    ; Blank line
    call newline

    mov esi, banner_line3
    mov bl, GREEN_ON_BLACK
    call print_line

    ; Blank line
    call newline

    ; Enter command loop
    jmp command_loop

; ═══════════════════════════════════════════════════════════
; Command Loop
; ═══════════════════════════════════════════════════════════
command_loop:
    ; Print prompt
    mov esi, prompt
    mov bl, YELLOW_ON_BLACK
    call print_string

    ; Clear command buffer
    mov byte [cmd_len], 0
    mov edi, cmd_buffer
    mov ecx, 128
    xor al, al
    rep stosb

    ; Read input until Enter
.read_loop:
    call read_key           ; returns ASCII in al (0 = no key / special)
    test al, al
    jz .read_loop           ; no key, keep polling

    cmp al, 13              ; Enter?
    je .execute

    cmp al, 8               ; Backspace?
    je .backspace

    ; Regular character — add to buffer if room
    movzx ecx, byte [cmd_len]
    cmp ecx, 126
    jge .read_loop          ; buffer full

    mov [cmd_buffer + ecx], al
    inc byte [cmd_len]

    ; Echo character
    mov ah, WHITE_ON_BLACK
    call put_char

    jmp .read_loop

.backspace:
    movzx ecx, byte [cmd_len]
    test ecx, ecx
    jz .read_loop           ; nothing to delete

    dec byte [cmd_len]

    ; Erase on screen: move cursor back, write space, move back
    movzx eax, byte [cursor_col]
    test eax, eax
    jz .read_loop
    dec byte [cursor_col]
    ; Write space at cursor position
    movzx eax, byte [cursor_row]
    imul eax, VGA_COLS
    movzx ecx, byte [cursor_col]
    add eax, ecx
    shl eax, 1
    add eax, VGA_BASE
    mov byte [eax], ' '
    mov byte [eax+1], WHITE_ON_BLACK
    call update_cursor
    jmp .read_loop

.execute:
    ; Null-terminate command
    movzx ecx, byte [cmd_len]
    mov byte [cmd_buffer + ecx], 0

    ; Newline after input
    call newline

    ; Empty command?
    cmp byte [cmd_len], 0
    je command_loop

    ; Compare with known commands
    mov esi, cmd_buffer
    mov edi, cmd_help
    call strcmp
    je .do_help

    mov esi, cmd_buffer
    mov edi, cmd_clear
    call strcmp
    je .do_clear

    mov esi, cmd_buffer
    mov edi, cmd_info
    call strcmp
    je .do_info

    mov esi, cmd_buffer
    mov edi, cmd_hello
    call strcmp
    je .do_hello

    mov esi, cmd_buffer
    mov edi, cmd_reboot
    call strcmp
    je .do_reboot

    ; Unknown command
    mov esi, unknown_msg
    mov bl, WHITE_ON_BLACK
    call print_string
    mov esi, cmd_buffer
    mov bl, WHITE_ON_BLACK
    call print_line
    jmp command_loop

.do_help:
    mov esi, help_msg1
    mov bl, GREEN_ON_BLACK
    call print_line
    mov esi, help_msg2
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, help_msg3
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, help_msg4
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, help_msg5
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, help_msg6
    mov bl, WHITE_ON_BLACK
    call print_line
    jmp command_loop

.do_clear:
    call clear_screen
    jmp command_loop

.do_info:
    mov esi, info_msg1
    mov bl, CYAN_ON_BLACK
    call print_line
    mov esi, info_msg2
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, info_msg3
    mov bl, WHITE_ON_BLACK
    call print_line
    mov esi, info_msg4
    mov bl, WHITE_ON_BLACK
    call print_line
    jmp command_loop

.do_hello:
    mov esi, hello_msg
    mov bl, GREEN_ON_BLACK
    call print_line
    jmp command_loop

.do_reboot:
    ; Triple fault reboot: load a null IDT and trigger interrupt
    lidt [.null_idt]
    int 0
.null_idt:
    dw 0
    dd 0

; ═══════════════════════════════════════════════════════════
; VGA Functions
; ═══════════════════════════════════════════════════════════

; clear_screen — fill VGA buffer with spaces
clear_screen:
    mov edi, VGA_BASE
    mov ecx, VGA_COLS * VGA_ROWS
    mov ax, 0x0F20          ; space + white-on-black
    rep stosw
    mov byte [cursor_row], 0
    mov byte [cursor_col], 0
    call update_cursor
    ret

; put_char — write character al with attribute ah at cursor, advance cursor
; Input: al = character, ah = color attribute
put_char:
    push ebx
    push ecx
    push edx

    ; Calculate VGA offset
    movzx ebx, byte [cursor_row]
    imul ebx, VGA_COLS
    movzx ecx, byte [cursor_col]
    add ebx, ecx
    shl ebx, 1
    add ebx, VGA_BASE

    ; Write character + attribute
    mov [ebx], al
    mov [ebx+1], ah

    ; Advance cursor
    inc byte [cursor_col]
    cmp byte [cursor_col], VGA_COLS
    jl .put_done
    mov byte [cursor_col], 0
    inc byte [cursor_row]
    cmp byte [cursor_row], VGA_ROWS
    jl .put_done
    call scroll_screen

.put_done:
    call update_cursor
    pop edx
    pop ecx
    pop ebx
    ret

; newline — move to start of next line
newline:
    mov byte [cursor_col], 0
    inc byte [cursor_row]
    cmp byte [cursor_row], VGA_ROWS
    jl .nl_done
    call scroll_screen
.nl_done:
    call update_cursor
    ret

; print_string — print null-terminated string at esi with color bl
; Does NOT add newline
print_string:
    push eax
.ps_loop:
    lodsb
    test al, al
    jz .ps_done
    mov ah, bl
    call put_char
    jmp .ps_loop
.ps_done:
    pop eax
    ret

; print_line — print null-terminated string at esi with color bl, then newline
print_line:
    call print_string
    call newline
    ret

; scroll_screen — scroll all lines up by 1, clear bottom line
scroll_screen:
    push esi
    push edi
    push ecx

    ; Copy rows 1..24 → rows 0..23
    mov esi, VGA_BASE + VGA_COLS * 2    ; source = row 1
    mov edi, VGA_BASE                    ; dest   = row 0
    mov ecx, VGA_COLS * (VGA_ROWS - 1)  ; word count
    rep movsw

    ; Clear last row
    mov ecx, VGA_COLS
    mov ax, 0x0F20
    rep stosw

    mov byte [cursor_row], VGA_ROWS - 1

    pop ecx
    pop edi
    pop esi
    ret

; update_cursor — update hardware cursor position via VGA I/O ports
update_cursor:
    push eax
    push ebx
    push edx

    ; Calculate linear position
    movzx eax, byte [cursor_row]
    imul eax, VGA_COLS
    movzx ebx, byte [cursor_col]
    add eax, ebx

    ; Send low byte to VGA port 0x3D4/0x3D5
    mov dx, 0x3D4
    push eax
    mov al, 0x0F
    out dx, al
    pop eax
    push eax
    mov dx, 0x3D5
    out dx, al

    ; Send high byte
    pop eax
    shr eax, 8
    push eax
    mov dx, 0x3D4
    mov al, 0x0E
    out dx, al
    pop eax
    mov dx, 0x3D5
    out dx, al

    pop edx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════════════
; Keyboard Input
; ═══════════════════════════════════════════════════════════

; read_key — poll keyboard, return ASCII in al (0 = nothing)
read_key:
    push edx

    ; Check if key is available
    in al, KBD_STATUS
    test al, 1
    jz .no_key

    ; Read scancode
    in al, KBD_DATA

    ; Check for key release (bit 7 set)
    test al, 0x80
    jnz .key_release

    ; Check shift press
    cmp al, SC_LSHIFT
    je .shift_on
    cmp al, SC_RSHIFT
    je .shift_on

    ; Convert scancode to ASCII
    cmp al, 0x39           ; max scancode in our table
    ja .no_key

    movzx eax, al
    cmp byte [shift_held], 0
    jne .use_shift
    mov al, [scancode_table + eax]
    jmp .key_done
.use_shift:
    mov al, [scancode_shift_table + eax]
    jmp .key_done

.shift_on:
    mov byte [shift_held], 1
    xor al, al
    jmp .key_done

.key_release:
    ; Check shift release
    and al, 0x7F
    cmp al, SC_LSHIFT
    je .shift_off
    cmp al, SC_RSHIFT
    je .shift_off
    xor al, al
    jmp .key_done

.shift_off:
    mov byte [shift_held], 0
    xor al, al

.key_done:
    pop edx
    ret

.no_key:
    xor al, al
    pop edx
    ret

; ═══════════════════════════════════════════════════════════
; String Comparison
; ═══════════════════════════════════════════════════════════

; strcmp — compare strings at esi and edi
; Sets ZF if equal
strcmp:
    push eax
    push ebx
.cmp_loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .cmp_ne
    test al, al
    jz .cmp_eq          ; both null terminators
    inc esi
    inc edi
    jmp .cmp_loop
.cmp_eq:
    ; Set ZF
    xor eax, eax
    pop ebx
    pop eax
    ret
.cmp_ne:
    ; Clear ZF
    or eax, 1
    pop ebx
    pop eax
    ret
