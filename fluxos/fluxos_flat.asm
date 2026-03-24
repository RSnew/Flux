; fluxos_flat.asm — FluxOS with PIT Timer, Memory Manager, RAM Filesystem
; Split-pane UI: Command area (left) | History/Info (right)
;
; Build: nasm -f elf32 fluxos_flat.asm -o fluxos_flat.o
;        i686-elf-ld -T linker.ld -o fluxos.bin fluxos_flat.o

BITS 32

; ═══════════════════════════════════════════════════
; Constants (equ)
; ═══════════════════════════════════════════════════
MBOOT_MAGIC    equ 0x1BADB002
MBOOT_FLAGS    equ 0x00000003
MBOOT_CHECKSUM equ -(MBOOT_MAGIC + MBOOT_FLAGS)

VGA_BASE       equ 0xB8000
VGA_COLS       equ 80
VGA_ROWS       equ 25
KBD_DATA       equ 0x60
KBD_STATUS     equ 0x64
SC_LSHIFT      equ 0x2A
SC_RSHIFT      equ 0x36
SC_UP          equ 0x48
SC_TAB         equ 0x0F

COL_LOGO       equ 0x0B
COL_DIR        equ 0x08
COL_VER        equ 0x0A
COL_BORDER     equ 0x03
COL_PROMPT     equ 0x0E
COL_INPUT      equ 0x0F
COL_OUTPUT     equ 0x07
COL_HIST_LBL   equ 0x06
COL_HIST       equ 0x08
COL_OK         equ 0x0A
COL_ERR        equ 0x0C
COL_BLUE       equ 0x09
COL_WHITE      equ 0x0F

HEADER_ROWS    equ 6
DIVIDER_ROW    equ 6
PANE_TOP       equ 7
PANE_BOT       equ 23
BOTTOM_ROW     equ 24
LEFT_W         equ 40
DIV_COL        equ 40
RIGHT_S        equ 41

HIST_MAX       equ 16
HIST_W         equ 64

FS_ENTRY_SZ    equ 64
FS_NAME        equ 0
FS_DPTR        equ 32
FS_SIZE        equ 36
FS_TYPE        equ 40
FS_PAR         equ 41
FS_MAXF        equ 64
FS_FREE        equ 0
FS_FILE        equ 1
FS_DIR         equ 2

HEAP_SZ        equ 65536

PIT_CH0        equ 0x40
PIT_CMD        equ 0x43
PIT_FREQ       equ 1193182
PIT_HZ         equ 100

; ═══════════════════════════════════════════════════
; Multiboot Header
; ═══════════════════════════════════════════════════
section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

; ═══════════════════════════════════════════════════
; BSS
; ═══════════════════════════════════════════════════
section .bss
align 16
stack_bottom: resb 16384
stack_top:

left_row:      resb 1
left_col:      resb 1
shift_held:    resb 1
cmd_buf:       resb 128
cmd_len:       resb 1
last_cmd:      resb 128
last_cmd_len:  resb 1

hist_buf:      resb HIST_MAX * HIST_W
hist_count:    resb 1

tick_count:    resd 1
pit_last:      resw 1

mem_total_pg:  resd 1
mem_used_pg:   resd 1
mem_bitmap:    resb 1024

align 16
fs_tab:        resb FS_MAXF * FS_ENTRY_SZ
fs_count:      resb 1
fs_cwd:        resb 1

heap:          resb HEAP_SZ
heap_ptr:      resd 1

arg1:          resb 128
arg2:          resb 256
pathbuf:       resb 128
numbuf:        resb 16

; ═══════════════════════════════════════════════════
; Data
; ═══════════════════════════════════════════════════
section .data

logo1: db " ___ _            ", 0
logo2: db "|  _| |_   ___  __", 0
logo3: db "| |_| | | | \ \/ /", 0
logo4: db "|  _| | |_| |>  < ", 0
logo5: db "|_| |_|\__,_/_/\_\", 0

dir_dot:    db ".", 0
dir_home:   db "~", 0
ver_str:    db "v1.0", 0
home_lbl:   db "home", 0
prompt:     db "> ", 0

welcome_msg:  db "Welcome to FluxOS.", 0
hint_msg:     db "Type 'help' for commands.", 0

help_lines:
h0:  db "Commands:", 0
h1:  db " help   - this help", 0
h2:  db " clear  - clear screen", 0
h3:  db " info   - system info", 0
h4:  db " pwd    - current dir", 0
h5:  db " uptime - time since boot", 0
h6:  db " mem    - memory info", 0
h7:  db " ls     - list files", 0
h8:  db " cd <d> - change dir", 0
h9:  db " mkdir <n> - make dir", 0
h10: db " touch <n> - make file", 0
h11: db " write <f> <t> - write", 0
h12: db " cat <f> - show file", 0
h13: db " rm <f>  - remove", 0
h14: db " stat <f> - file info", 0
h15: db " hello  - greeting", 0
h16: db " reboot - restart", 0

info1: db "FluxOS v1.0", 0
info2: db "Arch: x86 (32-bit)", 0
info3: db "VGA: 80x25 16-color", 0
info4: db "PIT: 100Hz timer", 0
info5: db "FS: RAMfs (64 files)", 0

hello_msg:    db "Hello from FluxOS!", 0
unknown_msg:  db "Unknown: ", 0
hist_label:   db " History ", 0

s_uptime:     db "Uptime: ", 0
s_seconds:    db "s", 0
s_total:      db "Total: ", 0
s_used:       db "Used:  ", 0
s_free:       db "Free:  ", 0
s_pages:      db " pages (4KB)", 0
s_slash:      db "/", 0
s_dotdot:     db "..", 0
s_type:       db "Type: ", 0
s_size:       db "Size: ", 0
s_dir_s:      db "directory", 0
s_file_s:     db "file", 0
s_bytes:      db " bytes", 0

e_exist:      db "Already exists", 0
e_full:       db "Filesystem full", 0
e_nf:         db "Not found: ", 0
e_ndir:       db "Not a directory: ", 0
e_isdir:      db "Is a directory", 0
e_notempty:   db "Not empty", 0

def_readme:   db "Welcome to FluxOS filesystem", 0
def_motd:     db "FluxOS v1.0 - Flux Language Bare Metal Kernel", 0

panel_dir:    db "Dir: ", 0
panel_files:  db "Files: ", 0

; Command name strings
c_help:   db "help", 0
c_clear:  db "clear", 0
c_info:   db "info", 0
c_hello:  db "hello", 0
c_reboot: db "reboot", 0
c_pwd:    db "pwd", 0
c_uptime: db "uptime", 0
c_mem:    db "mem", 0
c_ls:     db "ls", 0
c_cd:     db "cd", 0
c_mkdir:  db "mkdir", 0
c_touch:  db "touch", 0
c_write:  db "write", 0
c_cat:    db "cat", 0
c_rm:     db "rm", 0
c_stat:   db "stat", 0

tab_cmds: db "help clear info pwd ls cd mkdir touch write cat rm stat mem uptime", 0

scancode_table:
    db 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9
    db 'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0
    db 'a','s','d','f','g','h','j','k','l',';', 39, '`', 0, '\'
    db 'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '

scancode_shift:
    db 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9
    db 'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0
    db 'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|'
    db 'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' '

; ═══════════════════════════════════════════════════
; Text — Entry Point
; ═══════════════════════════════════════════════════
section .text
global _start
_start:
    mov esp, stack_top
    ; Zero BSS-located state
    mov byte [left_row], 0
    mov byte [left_col], 0
    mov byte [shift_held], 0
    mov byte [cmd_len], 0
    mov byte [last_cmd_len], 0
    mov byte [hist_count], 0
    mov dword [tick_count], 0
    mov word [pit_last], 0

    call pit_init
    call mem_init
    call fs_init
    call draw_ui
    call update_panel

    mov esi, welcome_msg
    mov bl, COL_OK
    call lp_println
    mov esi, hint_msg
    mov bl, COL_OUTPUT
    call lp_println
    call lp_newline
    jmp cmd_loop

; ═══════════════════════════════════════════════════
; PIT Init
; ═══════════════════════════════════════════════════
pit_init:
    mov al, 0x36
    out PIT_CMD, al
    mov ax, PIT_FREQ / PIT_HZ
    out PIT_CH0, al
    mov al, ah
    out PIT_CH0, al
    ret

; Poll PIT for tick counting
pit_poll:
    push eax
    push edx
    mov al, 0x00
    out PIT_CMD, al
    in al, PIT_CH0
    mov dl, al
    in al, PIT_CH0
    mov dh, al             ; dx = counter (high:low)
    cmp dx, [pit_last]
    jae .no_tick
    inc dword [tick_count]
.no_tick:
    mov [pit_last], dx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Memory Manager
; ═══════════════════════════════════════════════════
mem_init:
    ; 32MB = 8192 4KB pages
    mov dword [mem_total_pg], 8192
    ; Clear bitmap
    mov edi, mem_bitmap
    mov ecx, 1024
    xor al, al
    rep stosb
    ; Mark first 512 pages (2MB) as used
    mov edi, mem_bitmap
    mov ecx, 64
    mov al, 0xFF
    rep stosb
    mov dword [mem_used_pg], 512
    ret

; ═══════════════════════════════════════════════════
; Filesystem Init
; ═══════════════════════════════════════════════════
fs_init:
    ; Init heap
    mov eax, heap
    mov [heap_ptr], eax

    ; Clear fs table
    mov edi, fs_tab
    mov ecx, FS_MAXF * FS_ENTRY_SZ
    xor al, al
    rep stosb
    mov byte [fs_count], 0
    mov byte [fs_cwd], 0

    ; Entry 0: root dir "/"
    mov edi, fs_tab
    mov byte [edi + FS_NAME], '/'
    mov byte [edi + FS_NAME + 1], 0
    mov byte [edi + FS_TYPE], FS_DIR
    mov byte [edi + FS_PAR], 0xFF
    mov byte [fs_count], 1

    ; /README (file, parent=0)
    push dword .s_readme
    push dword FS_FILE
    push dword 0
    call fs_add_entry       ; eax = index
    add esp, 12
    ; Write content to README
    push dword def_readme
    push eax
    call fs_set_content
    add esp, 8

    ; /etc (dir, parent=0)
    push dword .s_etc
    push dword FS_DIR
    push dword 0
    call fs_add_entry
    add esp, 12
    push eax                ; save etc index

    ; /etc/motd (file, parent=etc)
    mov eax, [esp]          ; etc index
    push dword .s_motd
    push dword FS_FILE
    push eax
    call fs_add_entry
    add esp, 12
    push dword def_motd
    push eax
    call fs_set_content
    add esp, 8
    pop eax                 ; discard etc index

    ; /home (dir, parent=0)
    push dword .s_home
    push dword FS_DIR
    push dword 0
    call fs_add_entry
    add esp, 12

    ; /tmp (dir, parent=0)
    push dword .s_tmp
    push dword FS_DIR
    push dword 0
    call fs_add_entry
    add esp, 12
    ret

.s_readme: db "README", 0
.s_etc:    db "etc", 0
.s_motd:   db "motd", 0
.s_home:   db "home", 0
.s_tmp:    db "tmp", 0

; fs_add_entry(parent, type, name_ptr) -> eax=index
; Stack: [esp+4]=parent, [esp+8]=type, [esp+12]=name_ptr
fs_add_entry:
    push ebx
    push ecx
    push edi
    push esi
    movzx eax, byte [fs_count]
    cmp eax, FS_MAXF
    jge .fae_fail
    mov ebx, eax            ; ebx = new index
    imul edi, eax, FS_ENTRY_SZ
    add edi, fs_tab
    ; Copy name
    mov esi, [esp + 28]     ; name_ptr (4 saved regs + ret + parent + type + name)
    mov ecx, 31
.fae_cp:
    lodsb
    mov [edi], al
    inc edi
    test al, al
    jz .fae_cp_done
    dec ecx
    jnz .fae_cp
    mov byte [edi], 0
.fae_cp_done:
    ; Set fields at entry base
    imul edi, ebx, FS_ENTRY_SZ
    add edi, fs_tab
    mov eax, [esp + 24]     ; type
    mov [edi + FS_TYPE], al
    mov eax, [esp + 20]     ; parent
    mov [edi + FS_PAR], al
    mov dword [edi + FS_SIZE], 0
    mov dword [edi + FS_DPTR], 0
    inc byte [fs_count]
    mov eax, ebx             ; return index
    pop esi
    pop edi
    pop ecx
    pop ebx
    ret
.fae_fail:
    mov eax, -1
    pop esi
    pop edi
    pop ecx
    pop ebx
    ret

; fs_set_content(entry_index, str_ptr)
; Stack: [esp+4]=index, [esp+8]=str_ptr
fs_set_content:
    push eax
    push ebx
    push ecx
    push edi
    push esi
    ; Get string length
    mov esi, [esp + 28]     ; str_ptr
    xor ecx, ecx
.fsc_len:
    cmp byte [esi + ecx], 0
    je .fsc_got
    inc ecx
    jmp .fsc_len
.fsc_got:
    ; ecx = length (excl null)
    ; Allocate from heap
    mov eax, [heap_ptr]
    lea ebx, [eax + ecx + 1]
    mov [heap_ptr], ebx
    ; Copy string to heap
    mov edi, eax
    mov esi, [esp + 28]
    push ecx
    inc ecx                 ; include null
    rep movsb
    pop ecx
    ; Update entry
    mov ebx, [esp + 24]     ; entry index
    imul ebx, FS_ENTRY_SZ
    add ebx, fs_tab
    mov [ebx + FS_DPTR], eax
    mov [ebx + FS_SIZE], ecx
    pop esi
    pop edi
    pop ecx
    pop ebx
    pop eax
    ret

; fs_find(name_ptr, parent_idx) -> eax=index or -1
; esi=name, dl=parent
fs_find:
    push ebx
    push ecx
    push edi
    movzx ecx, byte [fs_count]
    mov ebx, 1
.ff_loop:
    cmp ebx, ecx
    jge .ff_nf
    imul eax, ebx, FS_ENTRY_SZ
    cmp byte [fs_tab + eax + FS_TYPE], FS_FREE
    je .ff_next
    cmp [fs_tab + eax + FS_PAR], dl
    jne .ff_next
    ; Compare names
    push esi
    push ecx
    lea edi, [fs_tab + eax + FS_NAME]
    call strcmp
    pop ecx
    pop esi
    je .ff_found
.ff_next:
    inc ebx
    jmp .ff_loop
.ff_found:
    mov eax, ebx
    pop edi
    pop ecx
    pop ebx
    ret
.ff_nf:
    mov eax, -1
    pop edi
    pop ecx
    pop ebx
    ret

; ═══════════════════════════════════════════════════
; Build CWD path string -> pathbuf
; ═══════════════════════════════════════════════════
build_path:
    push eax
    push ebx
    push ecx
    push esi
    push edi
    movzx eax, byte [fs_cwd]
    test eax, eax
    jnz .bp_deep
    mov byte [pathbuf], '/'
    mov byte [pathbuf + 1], 0
    jmp .bp_done
.bp_deep:
    ; Collect ancestor indices on stack
    sub esp, 32
    xor ecx, ecx
    movzx ebx, byte [fs_cwd]
.bp_up:
    mov [esp + ecx], bl
    inc ecx
    imul eax, ebx, FS_ENTRY_SZ
    movzx ebx, byte [fs_tab + eax + FS_PAR]
    cmp bl, 0xFF
    je .bp_build
    test bl, bl
    jz .bp_build
    cmp ecx, 31
    jge .bp_build
    jmp .bp_up
.bp_build:
    lea edi, [pathbuf]
    dec ecx
.bp_fwd:
    cmp ecx, -1
    jl .bp_end
    mov byte [edi], '/'
    inc edi
    movzx ebx, byte [esp + ecx]
    imul eax, ebx, FS_ENTRY_SZ
    lea esi, [fs_tab + eax + FS_NAME]
.bp_cpn:
    lodsb
    test al, al
    jz .bp_cpn_d
    mov [edi], al
    inc edi
    jmp .bp_cpn
.bp_cpn_d:
    dec ecx
    jmp .bp_fwd
.bp_end:
    mov byte [edi], 0
    add esp, 32
.bp_done:
    pop edi
    pop esi
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Draw UI frame
; ═══════════════════════════════════════════════════
draw_ui:
    ; Clear screen
    mov edi, VGA_BASE
    mov ecx, VGA_COLS * VGA_ROWS
    mov ax, 0x0020
    rep stosw
    ; Logo
    mov eax, 1
    mov edx, 2
    mov esi, logo1
    mov bl, COL_LOGO
    call draw_at
    mov eax, 2
    mov edx, 2
    mov esi, logo2
    call draw_at
    mov eax, 3
    mov edx, 2
    mov esi, logo3
    call draw_at
    mov eax, 4
    mov edx, 2
    mov esi, logo4
    call draw_at
    mov eax, 5
    mov edx, 2
    mov esi, logo5
    call draw_at
    ; Version
    mov eax, 4
    mov edx, 65
    mov esi, ver_str
    mov bl, COL_VER
    call draw_at
    ; Horizontal divider
    mov eax, DIVIDER_ROW
    xor edx, edx
.hd:
    cmp edx, VGA_COLS
    jge .hd_done
    push eax
    push edx
    cmp edx, DIV_COL
    je .hd_cross
    call vga_off
    mov byte [edi], 0xC4
    jmp .hd_set
.hd_cross:
    call vga_off
    mov byte [edi], 0xC2
.hd_set:
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .hd
.hd_done:
    ; Vertical divider
    mov eax, PANE_TOP
.vd:
    cmp eax, PANE_BOT
    jg .vd_done
    push eax
    mov edx, DIV_COL
    call vga_off
    mov byte [edi], 0xB3
    mov byte [edi+1], COL_BORDER
    pop eax
    inc eax
    jmp .vd
.vd_done:
    ; Bottom border
    mov eax, BOTTOM_ROW
    xor edx, edx
.bb:
    cmp edx, VGA_COLS
    jge .bb_done
    push eax
    push edx
    cmp edx, DIV_COL
    je .bb_cross
    call vga_off
    mov byte [edi], 0xC4
    jmp .bb_set
.bb_cross:
    call vga_off
    mov byte [edi], 0xC1
.bb_set:
    mov byte [edi+1], COL_BORDER
    pop edx
    pop eax
    inc edx
    jmp .bb
.bb_done:
    ; History label
    mov eax, DIVIDER_ROW
    mov edx, 52
    mov esi, hist_label
    mov bl, COL_HIST_LBL
    call draw_at
    ret

; ═══════════════════════════════════════════════════
; Update right-side info panel (rows 1-5)
; ═══════════════════════════════════════════════════
update_panel:
    push eax
    push edx
    push ecx
    push esi
    ; Clear rows 1-5 cols 58-79
    mov eax, 1
.up_clr:
    cmp eax, 5
    jg .up_draw
    push eax
    mov edx, 58
    call vga_off
    mov ecx, 22
    push eax
    mov ax, 0x0020
    rep stosw
    pop eax
    pop eax
    inc eax
    jmp .up_clr
.up_draw:
    call build_path
    mov eax, 1
    mov edx, 58
    mov esi, panel_dir
    mov bl, COL_DIR
    call draw_at
    mov eax, 1
    mov edx, 63
    mov esi, pathbuf
    mov bl, COL_VER
    call draw_at
    mov eax, 2
    mov edx, 58
    mov esi, dir_home
    mov bl, COL_DIR
    call draw_at
    mov eax, 2
    mov edx, 61
    mov esi, home_lbl
    mov bl, COL_OUTPUT
    call draw_at
    ; File count
    mov eax, 3
    mov edx, 58
    mov esi, panel_files
    mov bl, COL_DIR
    call draw_at
    push eax
    movzx eax, byte [fs_count]
    call itoa
    pop eax
    mov eax, 3
    mov edx, 66
    mov esi, numbuf
    mov bl, COL_OUTPUT
    call draw_at
    pop esi
    pop ecx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; VGA helpers
; ═══════════════════════════════════════════════════
; vga_off: eax=row, edx=col -> edi=VGA address
vga_off:
    push ebx
    mov ebx, eax
    imul ebx, VGA_COLS
    add ebx, edx
    shl ebx, 1
    lea edi, [VGA_BASE + ebx]
    pop ebx
    ret

; draw_at: row=eax, col=edx, esi=str, bl=color
draw_at:
    push eax
    push edx
    push ecx
    call vga_off
.da_l:
    lodsb
    test al, al
    jz .da_d
    mov [edi], al
    mov [edi+1], bl
    add edi, 2
    jmp .da_l
.da_d:
    pop ecx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Left pane output
; ═══════════════════════════════════════════════════
lp_putc:
    ; al=char, ah=color
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
    call vga_off
    pop eax
    mov [edi], al
    mov [edi+1], ah
    inc byte [left_col]
    cmp byte [left_col], LEFT_W
    jl .lpc_ok
    mov byte [left_col], 0
    inc byte [left_row]
.lpc_ok:
    call lp_cursor
    pop edx
    pop ecx
    pop ebx
    ret
.lpc_scroll:
    call lp_scroll
    jmp lp_putc

lp_newline:
    mov byte [left_col], 0
    inc byte [left_row]
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    cmp eax, PANE_BOT
    jle .ln_ok
    call lp_scroll
.ln_ok:
    call lp_cursor
    ret

lp_print:
    ; esi=str, bl=color
    push eax
.lpp_l:
    lodsb
    test al, al
    jz .lpp_d
    mov ah, bl
    call lp_putc
    jmp .lpp_l
.lpp_d:
    pop eax
    ret

lp_println:
    call lp_print
    call lp_newline
    ret

lp_scroll:
    push esi
    push edi
    push ecx
    push eax
    mov eax, PANE_TOP
.ls_r:
    cmp eax, PANE_BOT
    jge .ls_clr
    push eax
    mov edx, 0
    call vga_off
    mov esi, edi
    pop eax
    push eax
    inc eax
    mov edx, 0
    call vga_off
    xchg esi, edi
    mov ecx, LEFT_W
    rep movsw
    pop eax
    inc eax
    jmp .ls_r
.ls_clr:
    mov edx, 0
    call vga_off
    mov ecx, LEFT_W
    mov ax, 0x0020
    rep stosw
    dec byte [left_row]
    pop eax
    pop ecx
    pop edi
    pop esi
    ret

lp_clear:
    push eax
    push edx
    push ecx
    mov eax, PANE_TOP
.lc_r:
    cmp eax, PANE_BOT
    jg .lc_d
    push eax
    mov edx, 0
    call vga_off
    mov ecx, LEFT_W
    mov ax, 0x0020
    rep stosw
    pop eax
    inc eax
    jmp .lc_r
.lc_d:
    mov byte [left_row], 0
    mov byte [left_col], 0
    call lp_cursor
    pop ecx
    pop edx
    pop eax
    ret

lp_cursor:
    push eax
    push ebx
    push edx
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    imul eax, VGA_COLS
    movzx ebx, byte [left_col]
    add eax, ebx
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

; ═══════════════════════════════════════════════════
; History
; ═══════════════════════════════════════════════════
add_hist:
    ; esi = command string
    push eax
    push ecx
    push edi
    movzx eax, byte [hist_count]
    cmp eax, HIST_MAX
    jl .ah_ns
    push esi
    mov edi, hist_buf
    lea esi, [hist_buf + HIST_W]
    mov ecx, (HIST_MAX - 1) * HIST_W
    rep movsb
    pop esi
    mov eax, HIST_MAX - 1
    jmp .ah_cp
.ah_ns:
    inc byte [hist_count]
.ah_cp:
    imul eax, HIST_W
    lea edi, [hist_buf + eax]
    mov ecx, HIST_W - 1
.ah_c:
    lodsb
    test al, al
    jz .ah_pad
    stosb
    dec ecx
    jnz .ah_c
.ah_pad:
    xor al, al
    rep stosb
    pop edi
    pop ecx
    pop eax
    call draw_hist
    ret

draw_hist:
    push eax
    push edx
    push ecx
    push esi
    mov eax, PANE_TOP
.dh_clr:
    cmp eax, PANE_BOT
    jg .dh_draw
    push eax
    mov edx, RIGHT_S
    call vga_off
    mov ecx, VGA_COLS - RIGHT_S - 1
    mov ax, 0x0020
    rep stosw
    pop eax
    inc eax
    jmp .dh_clr
.dh_draw:
    movzx ecx, byte [hist_count]
    test ecx, ecx
    jz .dh_done
    mov eax, PANE_TOP
    dec ecx
.dh_lp:
    cmp ecx, 0
    jl .dh_done
    cmp eax, PANE_BOT
    jg .dh_done
    push eax
    push ecx
    imul ecx, HIST_W
    lea esi, [hist_buf + ecx]
    mov edx, RIGHT_S
    mov bl, COL_HIST
    call draw_at
    pop ecx
    pop eax
    inc eax
    dec ecx
    jmp .dh_lp
.dh_done:
    pop esi
    pop ecx
    pop edx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Keyboard (polling)
; ═══════════════════════════════════════════════════
read_key:
    push edx
    in al, KBD_STATUS
    test al, 1
    jz .rk_none
    in al, KBD_DATA
    ; Release?
    test al, 0x80
    jnz .rk_rel
    ; Shift?
    cmp al, SC_LSHIFT
    je .rk_son
    cmp al, SC_RSHIFT
    je .rk_son
    ; Up arrow?
    cmp al, SC_UP
    je .rk_up
    ; Tab?
    cmp al, SC_TAB
    je .rk_tab
    ; Normal
    cmp al, 0x39
    ja .rk_none
    movzx eax, al
    cmp byte [shift_held], 0
    jne .rk_shift
    mov al, [scancode_table + eax]
    jmp .rk_done
.rk_shift:
    mov al, [scancode_shift + eax]
    jmp .rk_done
.rk_son:
    mov byte [shift_held], 1
    xor al, al
    jmp .rk_done
.rk_rel:
    and al, 0x7F
    cmp al, SC_LSHIFT
    je .rk_soff
    cmp al, SC_RSHIFT
    je .rk_soff
    xor al, al
    jmp .rk_done
.rk_soff:
    mov byte [shift_held], 0
    xor al, al
    jmp .rk_done
.rk_up:
    mov al, 0xFF
    jmp .rk_done
.rk_tab:
    mov al, 9
.rk_done:
    pop edx
    ret
.rk_none:
    xor al, al
    pop edx
    ret

; ═══════════════════════════════════════════════════
; String compare: esi vs edi, sets ZF
; ═══════════════════════════════════════════════════
strcmp:
    push eax
    push ebx
.sc_l:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .sc_ne
    test al, al
    jz .sc_eq
    inc esi
    inc edi
    jmp .sc_l
.sc_eq:
    xor eax, eax    ; ZF=1
    pop ebx
    pop eax
    ret
.sc_ne:
    or eax, 1       ; ZF=0
    pop ebx
    pop eax
    ret

; Prefix compare: cmd_buf starts with [edi] followed by space or null
strcmp_pfx:
    push eax
    push ebx
    push ecx
    mov ecx, cmd_buf
.sp_l:
    mov al, [ecx]
    mov bl, [edi]
    test bl, bl
    jz .sp_end
    cmp al, bl
    jne .sp_ne
    inc ecx
    inc edi
    jmp .sp_l
.sp_end:
    cmp al, ' '
    je .sp_eq
    cmp al, 0
    je .sp_eq
.sp_ne:
    or eax, 1
    pop ecx
    pop ebx
    pop eax
    ret
.sp_eq:
    xor eax, eax
    pop ecx
    pop ebx
    pop eax
    ret

; itoa: eax -> numbuf (decimal string)
itoa:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    lea edi, [numbuf + 15]
    mov byte [edi], 0
    dec edi
    mov ebx, 10
    test eax, eax
    jnz .it_l
    mov byte [edi], '0'
    jmp .it_done
.it_l:
    test eax, eax
    jz .it_done
    xor edx, edx
    div ebx
    add dl, '0'
    mov [edi], dl
    dec edi
    jmp .it_l
.it_done:
    inc edi
    ; Shift to start of numbuf
    mov esi, edi
    mov edi, numbuf
    cmp esi, edi
    je .it_ret
.it_cp:
    lodsb
    stosb
    test al, al
    jnz .it_cp
.it_ret:
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Parse arguments from cmd_buf -> arg1, arg2
; ═══════════════════════════════════════════════════
parse_args:
    push eax
    push ecx
    push edi
    push esi
    mov byte [arg1], 0
    mov byte [arg2], 0
    mov esi, cmd_buf
    ; Skip command word
.pa_sk:
    lodsb
    test al, al
    jz .pa_done
    cmp al, ' '
    je .pa_sp1
    jmp .pa_sk
.pa_sp1:
    ; Skip spaces
    cmp byte [esi], ' '
    jne .pa_a1
    inc esi
    jmp .pa_sp1
.pa_a1:
    cmp byte [esi], 0
    je .pa_done
    ; Copy arg1
    mov edi, arg1
    mov ecx, 126
.pa_c1:
    lodsb
    test al, al
    jz .pa_c1d
    cmp al, ' '
    je .pa_c1d
    stosb
    dec ecx
    jnz .pa_c1
.pa_c1d:
    mov byte [edi], 0
    ; If stopped at space, get arg2 (rest of line)
    cmp al, ' '
    jne .pa_done
.pa_sp2:
    cmp byte [esi], ' '
    jne .pa_a2
    inc esi
    jmp .pa_sp2
.pa_a2:
    cmp byte [esi], 0
    je .pa_done
    mov edi, arg2
    mov ecx, 254
.pa_c2:
    lodsb
    test al, al
    jz .pa_c2d
    stosb
    dec ecx
    jnz .pa_c2
.pa_c2d:
    mov byte [edi], 0
.pa_done:
    pop esi
    pop edi
    pop ecx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Command Loop
; ═══════════════════════════════════════════════════
cmd_loop:
    mov esi, prompt
    mov bl, COL_PROMPT
    call lp_print

    mov byte [cmd_len], 0
    mov edi, cmd_buf
    mov ecx, 128
    xor al, al
    rep stosb

.rl:
    call pit_poll
    call read_key
    test al, al
    jz .rl
    cmp al, 0xFF
    je .recall
    cmp al, 9
    je .tab
    cmp al, 13
    je .exec
    cmp al, 8
    je .bs
    ; Normal char
    movzx ecx, byte [cmd_len]
    cmp ecx, 120
    jge .rl
    mov [cmd_buf + ecx], al
    inc byte [cmd_len]
    mov ah, COL_INPUT
    call lp_putc
    jmp .rl

.bs:
    movzx ecx, byte [cmd_len]
    test ecx, ecx
    jz .rl
    dec byte [cmd_len]
    cmp byte [left_col], 0
    je .rl
    dec byte [left_col]
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    movzx edx, byte [left_col]
    call vga_off
    mov byte [edi], ' '
    mov byte [edi+1], COL_INPUT
    call lp_cursor
    jmp .rl

.recall:
    cmp byte [last_cmd_len], 0
    je .rl
    movzx ecx, byte [cmd_len]
.rc_er:
    test ecx, ecx
    jz .rc_fill
    cmp byte [left_col], 0
    je .rc_fill
    dec byte [left_col]
    push ecx
    movzx eax, byte [left_row]
    add eax, PANE_TOP
    movzx edx, byte [left_col]
    call vga_off
    mov byte [edi], ' '
    mov byte [edi+1], COL_INPUT
    pop ecx
    dec ecx
    jmp .rc_er
.rc_fill:
    movzx ecx, byte [last_cmd_len]
    mov [cmd_len], cl
    xor ebx, ebx
.rc_cp:
    cmp ebx, ecx
    jge .rc_done
    mov al, [last_cmd + ebx]
    mov [cmd_buf + ebx], al
    mov ah, COL_INPUT
    call lp_putc
    inc ebx
    jmp .rc_cp
.rc_done:
    jmp .rl

.tab:
    call lp_newline
    mov esi, tab_cmds
    mov bl, COL_OUTPUT
    call lp_println
    mov esi, prompt
    mov bl, COL_PROMPT
    call lp_print
    xor ecx, ecx
.tab_rd:
    cmp cl, [cmd_len]
    jge .rl
    mov al, [cmd_buf + ecx]
    mov ah, COL_INPUT
    call lp_putc
    inc ecx
    jmp .tab_rd

.exec:
    movzx ecx, byte [cmd_len]
    mov byte [cmd_buf + ecx], 0
    call lp_newline
    cmp byte [cmd_len], 0
    je cmd_loop

    ; Save last command
    mov esi, cmd_buf
    mov edi, last_cmd
    movzx ecx, byte [cmd_len]
    mov [last_cmd_len], cl
    push ecx
    rep movsb
    mov byte [edi], 0
    pop ecx

    mov esi, cmd_buf
    call add_hist
    call parse_args

    ; Match exact commands
    mov esi, cmd_buf
    mov edi, c_help
    call strcmp
    je .cmd_help
    mov esi, cmd_buf
    mov edi, c_clear
    call strcmp
    je .cmd_clear
    mov esi, cmd_buf
    mov edi, c_info
    call strcmp
    je .cmd_info
    mov esi, cmd_buf
    mov edi, c_hello
    call strcmp
    je .cmd_hello
    mov esi, cmd_buf
    mov edi, c_reboot
    call strcmp
    je .cmd_reboot
    mov esi, cmd_buf
    mov edi, c_pwd
    call strcmp
    je .cmd_pwd
    mov esi, cmd_buf
    mov edi, c_uptime
    call strcmp
    je .cmd_uptime
    mov esi, cmd_buf
    mov edi, c_mem
    call strcmp
    je .cmd_mem
    mov esi, cmd_buf
    mov edi, c_ls
    call strcmp
    je .cmd_ls

    ; Prefix commands
    mov edi, c_cd
    call strcmp_pfx
    je .cmd_cd
    mov edi, c_mkdir
    call strcmp_pfx
    je .cmd_mkdir
    mov edi, c_touch
    call strcmp_pfx
    je .cmd_touch
    mov edi, c_cat
    call strcmp_pfx
    je .cmd_cat
    mov edi, c_rm
    call strcmp_pfx
    je .cmd_rm
    mov edi, c_stat
    call strcmp_pfx
    je .cmd_stat
    mov edi, c_write
    call strcmp_pfx
    je .cmd_write

    ; Unknown
    mov esi, unknown_msg
    mov bl, COL_ERR
    call lp_print
    mov esi, cmd_buf
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop

; ── help ──
.cmd_help:
    mov esi, h0
    mov bl, COL_OK
    call lp_println
    mov bl, COL_OUTPUT
    mov esi, h1
    call lp_println
    mov esi, h2
    call lp_println
    mov esi, h3
    call lp_println
    mov esi, h4
    call lp_println
    mov esi, h5
    call lp_println
    mov esi, h6
    call lp_println
    mov esi, h7
    call lp_println
    mov esi, h8
    call lp_println
    mov esi, h9
    call lp_println
    mov esi, h10
    call lp_println
    mov esi, h11
    call lp_println
    mov esi, h12
    call lp_println
    mov esi, h13
    call lp_println
    mov esi, h14
    call lp_println
    mov esi, h15
    call lp_println
    mov esi, h16
    call lp_println
    jmp cmd_loop

.cmd_clear:
    call lp_clear
    jmp cmd_loop

.cmd_info:
    mov esi, info1
    mov bl, COL_LOGO
    call lp_println
    mov bl, COL_OUTPUT
    mov esi, info2
    call lp_println
    mov esi, info3
    call lp_println
    mov esi, info4
    call lp_println
    mov esi, info5
    call lp_println
    jmp cmd_loop

.cmd_hello:
    mov esi, hello_msg
    mov bl, COL_OK
    call lp_println
    jmp cmd_loop

.cmd_pwd:
    call build_path
    mov esi, pathbuf
    mov bl, COL_VER
    call lp_println
    jmp cmd_loop

.cmd_reboot:
    lidt [.null_idt]
    int 0
.null_idt:
    dw 0
    dd 0

.cmd_uptime:
    mov esi, s_uptime
    mov bl, COL_OUTPUT
    call lp_print
    mov eax, [tick_count]
    xor edx, edx
    mov ecx, PIT_HZ
    div ecx
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_seconds
    mov bl, COL_OUTPUT
    call lp_println
    jmp cmd_loop

.cmd_mem:
    mov esi, s_total
    mov bl, COL_OUTPUT
    call lp_print
    mov eax, [mem_total_pg]
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_pages
    mov bl, COL_OUTPUT
    call lp_println

    mov esi, s_used
    mov bl, COL_OUTPUT
    call lp_print
    mov eax, [mem_used_pg]
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_pages
    mov bl, COL_OUTPUT
    call lp_println

    mov esi, s_free
    mov bl, COL_OUTPUT
    call lp_print
    mov eax, [mem_total_pg]
    sub eax, [mem_used_pg]
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_pages
    mov bl, COL_OUTPUT
    call lp_println
    jmp cmd_loop

; ── ls ──
.cmd_ls:
    movzx edx, byte [fs_cwd]  ; parent to match
    movzx ecx, byte [fs_count]
    mov ebx, 0
.ls_lp:
    cmp ebx, ecx
    jge .ls_done
    imul eax, ebx, FS_ENTRY_SZ
    cmp byte [fs_tab + eax + FS_TYPE], FS_FREE
    je .ls_nx
    cmp [fs_tab + eax + FS_PAR], dl
    jne .ls_nx
    push ecx
    push edx
    push ebx
    cmp byte [fs_tab + eax + FS_TYPE], FS_DIR
    je .ls_d
    ; File
    lea esi, [fs_tab + eax + FS_NAME]
    mov bl, COL_WHITE
    call lp_println
    jmp .ls_c
.ls_d:
    lea esi, [fs_tab + eax + FS_NAME]
    mov bl, COL_BLUE
    call lp_print
    mov esi, s_slash
    call lp_println
.ls_c:
    pop ebx
    pop edx
    pop ecx
.ls_nx:
    inc ebx
    jmp .ls_lp
.ls_done:
    jmp cmd_loop

; ── cd ──
.cmd_cd:
    cmp byte [arg1], 0
    je cmd_loop
    ; cd /
    cmp byte [arg1], '/'
    jne .cd_nr
    cmp byte [arg1 + 1], 0
    jne .cd_nr
    mov byte [fs_cwd], 0
    call update_panel
    jmp cmd_loop
.cd_nr:
    ; cd ..
    mov esi, arg1
    mov edi, s_dotdot
    call strcmp
    jne .cd_named
    movzx eax, byte [fs_cwd]
    test eax, eax
    jz cmd_loop
    imul eax, FS_ENTRY_SZ
    movzx eax, byte [fs_tab + eax + FS_PAR]
    cmp al, 0xFF
    jne .cd_par
    xor al, al
.cd_par:
    mov [fs_cwd], al
    call update_panel
    jmp cmd_loop
.cd_named:
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .cd_nf
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_DIR
    jne .cd_nd
    mov [fs_cwd], al
    call update_panel
    jmp cmd_loop
.cd_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_loop
.cd_nd:
    mov esi, e_ndir
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_loop

; ── mkdir ──
.cmd_mkdir:
    cmp byte [arg1], 0
    je cmd_loop
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    jne .mk_ex
    movzx eax, byte [fs_cwd]
    push dword arg1
    push dword FS_DIR
    push eax
    call fs_add_entry
    add esp, 12
    call update_panel
    jmp cmd_loop
.mk_ex:
    mov esi, e_exist
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop

; ── touch ──
.cmd_touch:
    cmp byte [arg1], 0
    je cmd_loop
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    jne cmd_loop           ; exists, OK
    movzx eax, byte [fs_cwd]
    push dword arg1
    push dword FS_FILE
    push eax
    call fs_add_entry
    add esp, 12
    call update_panel
    jmp cmd_loop

; ── write ──
.cmd_write:
    cmp byte [arg1], 0
    je cmd_loop
    ; Find or create file
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .wr_create
    ; Check it's a file
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_FILE
    jne .wr_isdir
    jmp .wr_do
.wr_create:
    movzx eax, byte [fs_cwd]
    push dword arg1
    push dword FS_FILE
    push eax
    call fs_add_entry
    add esp, 12
    cmp eax, -1
    je .wr_full
.wr_do:
    ; Write arg2 content
    cmp byte [arg2], 0
    je cmd_loop
    push dword arg2
    push eax
    call fs_set_content
    add esp, 8
    call update_panel
    jmp cmd_loop
.wr_isdir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop
.wr_full:
    mov esi, e_full
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop

; ── cat ──
.cmd_cat:
    cmp byte [arg1], 0
    je cmd_loop
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .cat_nf
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_FILE
    jne .cat_dir
    mov esi, [fs_tab + ebx + FS_DPTR]
    test esi, esi
    jz cmd_loop
    mov bl, COL_OUTPUT
    call lp_println
    jmp cmd_loop
.cat_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_loop
.cat_dir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop

; ── rm ──
.cmd_rm:
    cmp byte [arg1], 0
    je cmd_loop
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .rm_nf
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_DIR
    jne .rm_do
    ; Check directory is empty
    push eax
    push ebx
    movzx edx, al
    movzx ecx, byte [fs_count]
    xor esi, esi
.rm_chk:
    cmp esi, ecx
    jge .rm_empty
    imul edi, esi, FS_ENTRY_SZ
    cmp byte [fs_tab + edi + FS_TYPE], FS_FREE
    je .rm_chk_n
    cmp [fs_tab + edi + FS_PAR], dl
    je .rm_ne
.rm_chk_n:
    inc esi
    jmp .rm_chk
.rm_ne:
    pop ebx
    pop eax
    mov esi, e_notempty
    mov bl, COL_ERR
    call lp_println
    jmp cmd_loop
.rm_empty:
    pop ebx
    pop eax
.rm_do:
    mov byte [fs_tab + ebx + FS_TYPE], FS_FREE
    call update_panel
    jmp cmd_loop
.rm_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_loop

; ── stat ──
.cmd_stat:
    cmp byte [arg1], 0
    je cmd_loop
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .st_nf
    imul ebx, eax, FS_ENTRY_SZ
    ; Type
    mov esi, s_type
    mov bl, COL_OUTPUT
    call lp_print
    cmp byte [fs_tab + ebx + FS_TYPE], FS_DIR
    je .st_d
    mov esi, s_file_s
    jmp .st_td
.st_d:
    mov esi, s_dir_s
.st_td:
    mov bl, COL_VER
    call lp_println
    ; Size
    mov esi, s_size
    mov bl, COL_OUTPUT
    call lp_print
    mov eax, [fs_tab + ebx + FS_SIZE]
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_bytes
    mov bl, COL_OUTPUT
    call lp_println
    jmp cmd_loop
.st_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_loop
