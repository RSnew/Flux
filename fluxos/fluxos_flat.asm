; fluxos_flat.asm — FluxOS with split-pane UI
; Layout:
;   Top bar: Flux logo (left) | ./ dir  ~ home  v1.0 (right)
;   ├─────────────────────┬──────────────────────┤
;   │ Command area (left) │ History (right)       │
;   └─────────────────────┴──────────────────────┘
;
; Build: nasm -f elf32 fluxos_flat.asm -o fluxos_flat.o
;        i686-elf-ld -T linker.ld -o fluxos.bin fluxos_flat.o

BITS 32

; ═══════════════════════════════════════════════════════════
; Multiboot1 Header
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
; BSS
; ═══════════════════════════════════════════════════════════
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; Cursor for left pane (command area)
left_row:    resb 1     ; current row in left pane (relative, 0-based from pane top)
left_col:    resb 1     ; current col in left pane

shift_held:  resb 1
cmd_buffer:  resb 128
cmd_len:     resb 1
cwd_buf:     resb 64    ; current directory display

; History buffer: 16 entries x 64 chars
HIST_MAX     equ 16
HIST_WIDTH   equ 64
hist_buf:    resb HIST_MAX * HIST_WIDTH
hist_count:  resb 1

; ═══════════════════════════════════════════════════════════
; Data
; ═══════════════════════════════════════════════════════════
section .data

; Logo lines (Flux stylized)
logo1: db " ___ _            ", 0
logo2: db "|  _| |_   ___  __", 0
logo3: db "| |_| | | | \ \/ /", 0
logo4: db "|  _| | |_| |>  < ", 0
logo5: db "|_| |_|\__,_/_/\_\", 0

; Right-side info
dir_dot:    db ".", 0
dir_home:   db "~", 0
version:    db "v1.0", 0
cur_dir:    db "/fluxos", 0
home_label: db "home", 0

; Separator chars
hline_char: db 0xC4         ; ─ (code page 437)
vline_char: db 0xB3         ; │
tl_char:    db 0xDA         ; ┌
tr_char:    db 0xBF         ; ┐
bl_char:    db 0xC0         ; └
br_char:    db 0xD9         ; ┘
t_down:     db 0xC2         ; ┬
t_up:       db 0xC1         ; ┴
t_right:    db 0xC3         ; ├
t_left:     db 0xB4         ; ┤

; Prompt
prompt:       db "> ", 0

; Messages
welcome_msg:  db "Welcome to FluxOS.", 0
hint_msg:     db "Type 'help' for commands.", 0
help_msg1:    db "Commands:", 0
help_msg2:    db " help  - this help", 0
help_msg3:    db " clear - clear screen", 0
help_msg4:    db " info  - system info", 0
help_msg5:    db " pwd   - show directory", 0
help_msg6:    db " hello - greeting", 0
help_msg7:    db " reboot", 0
info_msg1:    db "FluxOS v1.0", 0
info_msg2:    db "Arch: x86 (32-bit)", 0
info_msg3:    db "VGA: 80x25 16-color", 0
info_msg4:    db "Flux Language Kernel", 0
hello_msg:    db "Hello from FluxOS!", 0
unknown_msg:  db "Unknown: ", 0
pwd_msg:      db "/fluxos", 0

cmd_help:     db "help", 0
cmd_clear:    db "clear", 0
cmd_info:     db "info", 0
cmd_hello:    db "hello", 0
cmd_reboot:   db "reboot", 0
cmd_pwd:      db "pwd", 0

hist_label:   db " History ", 0

; Scancode tables
scancode_table:
    db 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9
    db 'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0
    db 'a','s','d','f','g','h','j','k','l',';', 39, '`', 0, '\'
    db 'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '

scancode_shift_table:
    db 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9
    db 'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0
    db 'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|'
    db 'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' '

; ═══════════════════════════════════════════════════════════
; Constants
; ═══════════════════════════════════════════════════════════
VGA_BASE        equ 0xB8000
VGA_COLS        equ 80
VGA_ROWS        equ 25
KBD_DATA        equ 0x60
KBD_STATUS      equ 0x64
SC_LSHIFT       equ 0x2A
SC_RSHIFT       equ 0x36

; Colors (VGA attribute byte: bg[7:4] | fg[3:0])
; Background: 0=black
; Foreground: 0=black 1=blue 2=green 3=cyan 4=red 5=magenta 6=brown 7=lgray
;             8=dgray 9=lblue A=lgreen B=lcyan C=lred D=lmagenta E=yellow F=white
COL_LOGO        equ 0x0B    ; light cyan on black
COL_DIR         equ 0x08    ; dark gray on black
COL_VER         equ 0x0A    ; light green on black
COL_BORDER      equ 0x03    ; cyan on black
COL_PROMPT      equ 0x0E    ; yellow on black
COL_INPUT       equ 0x0F    ; white on black
COL_OUTPUT      equ 0x07    ; light gray on black
COL_HIST_LABEL  equ 0x06    ; brown on black
COL_HIST        equ 0x08    ; dark gray on black
COL_WELCOME     equ 0x0A    ; light green on black
COL_ERROR       equ 0x0C    ; light red on black

; Layout constants
HEADER_ROWS     equ 6       ; rows 0-5: logo + info
DIVIDER_ROW     equ 6       ; horizontal divider
PANE_TOP        equ 7       ; first content row
PANE_BOT        equ 23      ; last content row
BOTTOM_ROW      equ 24      ; bottom border
LEFT_WIDTH      equ 40      ; left pane columns (0-39)
DIVIDER_COL     equ 40      ; vertical divider column
RIGHT_START     equ 41      ; right pane start col

; ═══════════════════════════════════════════════════════════
; Entry Point
; ═══════════════════════════════════════════════════════════
section .text
global _start
_start:
    mov esp, stack_top
    mov byte [hist_count], 0
    call draw_ui

    ; Welcome messages in left pane
    mov byte [left_row], 0
    mov byte [left_col], 0
    mov esi, welcome_msg
    mov bl, COL_WELCOME
    call left_print_line
    mov esi, hint_msg
    mov bl, COL_OUTPUT
    call left_print_line
    call left_newline

    jmp command_loop

; ═══════════════════════════════════════════════════════════
; Draw UI Frame
; ═══════════════════════════════════════════════════════════
draw_ui:
    ; Clear entire screen (black)
    mov edi, VGA_BASE
    mov ecx, VGA_COLS * VGA_ROWS
    mov ax, 0x0020          ; black bg, space
    rep stosw

    ; ── Logo (rows 1-5, left side) ──
    mov eax, 1
    mov edx, 2
    mov esi, logo1
    mov bl, COL_LOGO
    call draw_at
    mov eax, 2
    mov edx, 2
    mov esi, logo2
    mov bl, COL_LOGO
    call draw_at
    mov eax, 3
    mov edx, 2
    mov esi, logo3
    mov bl, COL_LOGO
    call draw_at
    mov eax, 4
    mov edx, 2
    mov esi, logo4
    mov bl, COL_LOGO
    call draw_at
    mov eax, 5
    mov edx, 2
    mov esi, logo5
    mov bl, COL_LOGO
    call draw_at

    ; ── Right side info (rows 1-4) ──
    ; ./ current_dir
    mov eax, 1
    mov edx, 58
    mov esi, dir_dot
    mov bl, COL_DIR
    call draw_at
    mov eax, 1
    mov edx, 61
    mov esi, cur_dir
    mov bl, COL_VER
    call draw_at

    ; ~ home
    mov eax, 2
    mov edx, 58
    mov esi, dir_home
    mov bl, COL_DIR
    call draw_at
    mov eax, 2
    mov edx, 61
    mov esi, home_label
    mov bl, COL_OUTPUT
    call draw_at

    ; v1.0
    mov eax, 4
    mov edx, 65
    mov esi, version
    mov bl, COL_VER
    call draw_at

    ; ── Horizontal divider (row 6) ──
    mov eax, DIVIDER_ROW
    xor edx, edx            ; col 0
.hdiv_loop:
    cmp edx, VGA_COLS
    jge .hdiv_done
    cmp edx, DIVIDER_COL
    je .hdiv_cross
    push eax
    push edx
    call vga_offset
    mov byte [edi], 0xC4    ; ─
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .hdiv_loop
.hdiv_cross:
    push eax
    push edx
    call vga_offset
    mov byte [edi], 0xC2    ; ┬
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .hdiv_loop
.hdiv_done:

    ; ── Vertical divider (rows 7-23) ──
    mov eax, PANE_TOP
.vdiv_loop:
    cmp eax, PANE_BOT
    jg .vdiv_done
    push eax
    mov edx, DIVIDER_COL
    call vga_offset
    mov byte [edi], 0xB3    ; │
    mov byte [edi+1], COL_BORDER
    pop eax
    inc eax
    jmp .vdiv_loop
.vdiv_done:

    ; ── Bottom border (row 24) ──
    mov eax, BOTTOM_ROW
    xor edx, edx
.bbar_loop:
    cmp edx, VGA_COLS
    jge .bbar_done
    cmp edx, DIVIDER_COL
    je .bbar_cross
    push eax
    push edx
    call vga_offset
    mov byte [edi], 0xC4    ; ─
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .bbar_loop
.bbar_cross:
    push eax
    push edx
    call vga_offset
    mov byte [edi], 0xC1    ; ┴
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .bbar_loop
.bbar_done:

    ; ── History label ──
    mov eax, DIVIDER_ROW
    mov edx, 52
    mov esi, hist_label
    mov bl, COL_HIST_LABEL
    call draw_at

    ret

; ═══════════════════════════════════════════════════════════
; Helper: VGA offset → edi = VGA_BASE + (row*80+col)*2
; eax=row, edx=col
; ═══════════════════════════════════════════════════════════
vga_offset:
    push ebx
    mov ebx, eax
    imul ebx, VGA_COLS
    add ebx, edx
    shl ebx, 1
    lea edi, [VGA_BASE + ebx]
    pop ebx
    ret

; ═══════════════════════════════════════════════════════════
; Draw string at row=eax, col=edx, color=bl
; ═══════════════════════════════════════════════════════════
draw_at:
    push eax
    push edx
    push ecx
    call vga_offset
.da_loop:
    lodsb
    test al, al
    jz .da_done
    mov [edi], al
    mov [edi+1], bl
    add edi, 2
    jmp .da_loop
.da_done:
    pop ecx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════════════
; Left Pane: print string, newline, clear, put_char
; Left pane: rows PANE_TOP..PANE_BOT, cols 0..(LEFT_WIDTH-1)
; ═══════════════════════════════════════════════════════════
left_put_char:
    ; al=char, ah=color attr
    push ebx
    push ecx
    push edx
    movzx ebx, byte [left_row]
    add ebx, PANE_TOP
    cmp ebx, PANE_BOT
    jg .lpc_scroll
    movzx ecx, byte [left_col]
    push eax
    mov eax, ebx
    mov edx, ecx
    call vga_offset
    pop eax
    mov [edi], al
    mov [edi+1], ah
    inc byte [left_col]
    cmp byte [left_col], LEFT_WIDTH
    jl .lpc_done
    mov byte [left_col], 0
    inc byte [left_row]
.lpc_done:
    call left_update_cursor
    pop edx
    pop ecx
    pop ebx
    ret
.lpc_scroll:
    call left_scroll
    jmp left_put_char

left_newline:
    mov byte [left_col], 0
    inc byte [left_row]
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    cmp eax, PANE_BOT
    jle .ln_ok
    call left_scroll
.ln_ok:
    call left_update_cursor
    ret

left_print_string:
    ; esi=string, bl=color
    push eax
.lps_loop:
    lodsb
    test al, al
    jz .lps_done
    mov ah, bl
    call left_put_char
    jmp .lps_loop
.lps_done:
    pop eax
    ret

left_print_line:
    call left_print_string
    call left_newline
    ret

left_scroll:
    ; Scroll left pane up by 1 row
    push esi
    push edi
    push ecx
    push eax
    mov eax, PANE_TOP
.ls_row:
    cmp eax, PANE_BOT
    jge .ls_clear_last
    ; Copy row eax+1 → row eax (cols 0..LEFT_WIDTH-1)
    push eax
    mov edx, 0
    call vga_offset         ; edi = dest (row eax, col 0)
    mov esi, edi
    pop eax
    push eax
    inc eax
    mov edx, 0
    call vga_offset         ; edi = source (row eax+1, col 0)
    xchg esi, edi           ; esi=source, edi=dest
    mov ecx, LEFT_WIDTH
    rep movsw
    pop eax
    inc eax
    jmp .ls_row
.ls_clear_last:
    ; Clear last row
    mov edx, 0
    call vga_offset
    mov ecx, LEFT_WIDTH
    mov ax, 0x0020
    rep stosw
    dec byte [left_row]
    pop eax
    pop ecx
    pop edi
    pop esi
    ret

left_clear:
    ; Clear left pane content area
    push eax
    push edx
    push ecx
    mov eax, PANE_TOP
.lc_row:
    cmp eax, PANE_BOT
    jg .lc_done
    push eax
    mov edx, 0
    call vga_offset
    mov ecx, LEFT_WIDTH
    mov ax, 0x0020
    rep stosw
    pop eax
    inc eax
    jmp .lc_row
.lc_done:
    mov byte [left_row], 0
    mov byte [left_col], 0
    call left_update_cursor
    pop ecx
    pop edx
    pop eax
    ret

left_update_cursor:
    push eax
    push ebx
    push edx
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    imul eax, VGA_COLS
    movzx ebx, byte [left_col]
    add eax, ebx
    ; Set hardware cursor
    mov dx, 0x3D4
    push eax
    mov al, 0x0F
    out dx, al
    pop eax
    push eax
    mov dx, 0x3D5
    out dx, al
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
; Right Pane: History display
; ═══════════════════════════════════════════════════════════
add_history:
    ; esi = command string to add
    push eax
    push ecx
    push edi
    movzx eax, byte [hist_count]
    cmp eax, HIST_MAX
    jl .ah_no_shift
    ; Shift history up (drop oldest)
    push esi
    mov edi, hist_buf
    lea esi, [hist_buf + HIST_WIDTH]
    mov ecx, (HIST_MAX - 1) * HIST_WIDTH
    rep movsb
    pop esi
    mov eax, HIST_MAX - 1
    jmp .ah_copy
.ah_no_shift:
    inc byte [hist_count]
.ah_copy:
    ; Copy command to hist_buf[eax * HIST_WIDTH]
    imul eax, HIST_WIDTH
    lea edi, [hist_buf + eax]
    mov ecx, HIST_WIDTH - 1
.ah_cpy:
    lodsb
    test al, al
    jz .ah_pad
    stosb
    dec ecx
    jnz .ah_cpy
.ah_pad:
    xor al, al
    rep stosb
    pop edi
    pop ecx
    pop eax
    call draw_history
    ret

draw_history:
    ; Draw history entries in right pane (newest at top = reverse order)
    push eax
    push edx
    push ecx
    push esi

    ; Clear right pane first
    mov eax, PANE_TOP
.dh_clear:
    cmp eax, PANE_BOT
    jg .dh_draw
    push eax
    mov edx, RIGHT_START
    call vga_offset
    mov ecx, VGA_COLS - RIGHT_START - 1
    mov ax, 0x0020
    rep stosw
    pop eax
    inc eax
    jmp .dh_clear

.dh_draw:
    movzx ecx, byte [hist_count]
    test ecx, ecx
    jz .dh_done

    ; Draw from newest (index=count-1) to oldest, top to bottom
    mov eax, PANE_TOP       ; current display row
    dec ecx                 ; index = count - 1
.dh_loop:
    cmp ecx, 0
    jl .dh_done
    cmp eax, PANE_BOT
    jg .dh_done
    push eax
    push ecx
    ; Compute history entry address
    imul ecx, HIST_WIDTH
    lea esi, [hist_buf + ecx]
    mov edx, RIGHT_START
    mov bl, COL_HIST
    call draw_at
    pop ecx
    pop eax
    inc eax
    dec ecx
    jmp .dh_loop

.dh_done:
    pop esi
    pop ecx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════════════
; Command Loop
; ═══════════════════════════════════════════════════════════
command_loop:
    ; Print prompt
    mov esi, prompt
    mov bl, COL_PROMPT
    call left_print_string

    ; Clear buffer
    mov byte [cmd_len], 0
    mov edi, cmd_buffer
    mov ecx, 128
    xor al, al
    rep stosb

.read_loop:
    call read_key
    test al, al
    jz .read_loop
    cmp al, 13
    je .execute
    cmp al, 8
    je .backspace
    movzx ecx, byte [cmd_len]
    cmp ecx, 36              ; max chars in left pane minus prompt
    jge .read_loop
    mov [cmd_buffer + ecx], al
    inc byte [cmd_len]
    mov ah, COL_INPUT
    call left_put_char
    jmp .read_loop

.backspace:
    movzx ecx, byte [cmd_len]
    test ecx, ecx
    jz .read_loop
    dec byte [cmd_len]
    ; Erase char on screen
    movzx eax, byte [left_col]
    test eax, eax
    jz .read_loop
    dec byte [left_col]
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    movzx edx, byte [left_col]
    call vga_offset
    mov byte [edi], ' '
    mov byte [edi+1], COL_INPUT
    call left_update_cursor
    jmp .read_loop

.execute:
    movzx ecx, byte [cmd_len]
    mov byte [cmd_buffer + ecx], 0
    call left_newline
    cmp byte [cmd_len], 0
    je command_loop

    ; Add to history
    mov esi, cmd_buffer
    call add_history

    ; Match commands
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
    mov esi, cmd_buffer
    mov edi, cmd_pwd
    call strcmp
    je .do_pwd

    ; Unknown command
    mov esi, unknown_msg
    mov bl, COL_ERROR
    call left_print_string
    mov esi, cmd_buffer
    mov bl, COL_ERROR
    call left_print_line
    jmp command_loop

.do_help:
    mov esi, help_msg1
    mov bl, COL_WELCOME
    call left_print_line
    mov esi, help_msg2
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, help_msg3
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, help_msg4
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, help_msg5
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, help_msg6
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, help_msg7
    mov bl, COL_OUTPUT
    call left_print_line
    jmp command_loop

.do_clear:
    call left_clear
    jmp command_loop

.do_info:
    mov esi, info_msg1
    mov bl, COL_LOGO
    call left_print_line
    mov esi, info_msg2
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, info_msg3
    mov bl, COL_OUTPUT
    call left_print_line
    mov esi, info_msg4
    mov bl, COL_OUTPUT
    call left_print_line
    jmp command_loop

.do_hello:
    mov esi, hello_msg
    mov bl, COL_WELCOME
    call left_print_line
    jmp command_loop

.do_pwd:
    mov esi, pwd_msg
    mov bl, COL_VER
    call left_print_line
    ; Flash the dir indicator in top right
    ; (briefly change color of ./ line)
    push eax
    push edx
    mov eax, 1
    mov edx, 58
    mov esi, dir_dot
    mov bl, COL_VER         ; flash green
    call draw_at
    mov eax, 1
    mov edx, 61
    mov esi, cur_dir
    mov bl, 0x0F            ; flash white
    call draw_at
    pop edx
    pop eax
    jmp command_loop

.do_reboot:
    lidt [.null_idt]
    int 0
.null_idt:
    dw 0
    dd 0

; ═══════════════════════════════════════════════════════════
; Keyboard Input
; ═══════════════════════════════════════════════════════════
read_key:
    push edx
    in al, KBD_STATUS
    test al, 1
    jz .no_key
    in al, KBD_DATA
    test al, 0x80
    jnz .key_release
    cmp al, SC_LSHIFT
    je .shift_on
    cmp al, SC_RSHIFT
    je .shift_on
    cmp al, 0x39
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
strcmp:
    push eax
    push ebx
.cmp_loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .cmp_ne
    test al, al
    jz .cmp_eq
    inc esi
    inc edi
    jmp .cmp_loop
.cmp_eq:
    xor eax, eax
    pop ebx
    pop eax
    ret
.cmp_ne:
    or eax, 1
    pop ebx
    pop eax
    ret
