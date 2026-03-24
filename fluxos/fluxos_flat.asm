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

; History entry: 64 cmd + 256 output + 1 expanded + 1 output_lines = 322
HIST_ENTRY_SIZE equ 322
HIST_CMD_OFF   equ 0
HIST_OUT_OFF   equ 64
HIST_EXP_OFF   equ 320
HIST_OLINES_OFF equ 321

SC_CTRL        equ 0x1D
SC_ALT         equ 0x38
SC_LEFT        equ 0x4B
SC_RIGHT       equ 0x4D
SC_DOWN        equ 0x50
SC_ENTER       equ 0x1C

COL_HIST_SEL   equ 0x1F    ; white on dark blue
COL_HIST_ARROW equ 0x0E    ; yellow
COL_FADE1      equ 0x08    ; dark gray
COL_FADE2      equ 0x07    ; gray
COL_FADE3      equ 0x0F    ; bright white

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

; Interactive history panel data
hist_entries:  resb HIST_MAX * HIST_ENTRY_SIZE
hist_entry_cnt: resb 1
hist_selected: resb 1      ; currently selected index in right panel
focus_panel:   resb 1      ; 0=left, 1=right
ctrl_held:     resb 1
alt_held:      resb 1
pre_cmd_row:   resb 1      ; left_row before command execution

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

arrow_right: db "> ", 0     ; collapsed arrow (ASCII >)
arrow_down:  db "v ", 0     ; expanded arrow (ASCII v)

; Ease-out delay table (in PIT ticks, ~10ms each)
ease_out_5: db 2, 3, 4, 6, 9
ease_in_5:  db 9, 6, 4, 3, 2

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
    mov byte [hist_entry_cnt], 0
    mov byte [hist_selected], 0
    mov byte [focus_panel], 0
    mov byte [ctrl_held], 0
    mov byte [alt_held], 0
    mov byte [pre_cmd_row], 0
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
    push esi              ; save esi for hist_entries copy

    ; --- Original hist_buf logic ---
    movzx eax, byte [hist_count]
    cmp eax, HIST_MAX
    jl .ah_ns
    push esi
    mov edi, hist_buf
    lea esi, [hist_buf + HIST_W]
    mov ecx, (HIST_MAX - 1) * HIST_W
    rep movsb
    pop esi
    ; Also shift hist_entries
    push esi
    mov edi, hist_entries
    lea esi, [hist_entries + HIST_ENTRY_SIZE]
    mov ecx, (HIST_MAX - 1) * HIST_ENTRY_SIZE
    rep movsb
    pop esi
    mov eax, HIST_MAX - 1
    jmp .ah_cp
.ah_ns:
    inc byte [hist_count]
.ah_cp:
    push eax              ; save index for hist_entries
    imul eax, HIST_W
    lea edi, [hist_buf + eax]
    pop eax
    push eax
    push edi
    ; Restore original esi from stack
    ; Stack: [esp]=edi, [esp+4]=eax, [esp+8]=saved_esi, [esp+12]=saved_edi_orig, ...
    mov esi, [esp + 8]    ; original esi (command string)
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
    pop edi               ; discard saved edi
    pop eax               ; index back

    ; --- hist_entries: copy cmd, clear output, collapsed ---
    push eax
    imul eax, HIST_ENTRY_SIZE
    lea edi, [hist_entries + eax]
    ; Clear entire entry first
    push edi
    mov ecx, HIST_ENTRY_SIZE
    xor al, al
    rep stosb
    pop edi
    ; Copy command string
    ; Stack: [esp]=eax, [esp+4]=saved_esi, [esp+8]=saved_edi, ...
    mov esi, [esp + 4]    ; original esi (cmd string)
    mov ecx, 63
.ah_ecmd:
    lodsb
    test al, al
    jz .ah_ecmd_done
    mov [edi], al
    inc edi
    dec ecx
    jnz .ah_ecmd
.ah_ecmd_done:
    mov byte [edi], 0
    pop eax

    ; Update hist_entry_cnt to match hist_count
    mov cl, [hist_count]
    mov [hist_entry_cnt], cl

    ; Set selected to newest entry
    dec cl
    mov [hist_selected], cl

    pop esi               ; restore original esi
    pop edi
    pop ecx
    pop eax
    call draw_hist_panel
    ret

draw_hist:
    ; Legacy wrapper
    call draw_hist_panel
    ret

; ═══════════════════════════════════════════════════
; Draw history panel with arrows, selection, expand
; ═══════════════════════════════════════════════════
draw_hist_panel:
    push eax
    push ebx
    push edx
    push ecx
    push esi
    push edi

    ; Clear right pane area
    mov eax, PANE_TOP
.dhp_clr:
    cmp eax, PANE_BOT
    jg .dhp_draw
    push eax
    mov edx, RIGHT_S
    call vga_off
    mov ecx, VGA_COLS - RIGHT_S - 1
    mov ax, 0x0020
    rep stosw
    pop eax
    inc eax
    jmp .dhp_clr

.dhp_draw:
    movzx ecx, byte [hist_entry_cnt]
    test ecx, ecx
    jz .dhp_done

    ; Draw from newest (top) to oldest
    ; ecx = count, we draw index (ecx-1) down to 0
    mov eax, PANE_TOP       ; current screen row
    dec ecx                 ; start index = count-1

.dhp_loop:
    cmp ecx, 0
    jl .dhp_done
    cmp eax, PANE_BOT
    jg .dhp_done

    push eax
    push ecx

    ; Determine color based on selection and focus
    mov bl, COL_HIST         ; default color
    cmp byte [focus_panel], 1
    jne .dhp_no_sel
    cmp cl, [hist_selected]
    jne .dhp_no_sel
    mov bl, COL_HIST_SEL     ; selected: white on blue
.dhp_no_sel:

    ; Draw arrow
    push ebx
    imul edx, ecx, HIST_ENTRY_SIZE
    cmp byte [hist_entries + edx + HIST_EXP_OFF], 0
    jne .dhp_expanded_arrow
    ; Collapsed: >
    mov edx, RIGHT_S
    mov esi, arrow_right
    mov bl, COL_HIST_ARROW
    call draw_at
    jmp .dhp_arrow_done
.dhp_expanded_arrow:
    mov edx, RIGHT_S
    mov esi, arrow_down
    mov bl, COL_HIST_ARROW
    call draw_at
.dhp_arrow_done:
    pop ebx

    ; Draw command text (after arrow, col RIGHT_S+2)
    pop ecx
    push ecx
    imul edx, ecx, HIST_ENTRY_SIZE
    lea esi, [hist_entries + edx + HIST_CMD_OFF]
    mov edx, RIGHT_S + 2
    call draw_at

    pop ecx
    pop eax

    ; If expanded, draw output lines below
    push eax
    push ecx
    imul edx, ecx, HIST_ENTRY_SIZE
    cmp byte [hist_entries + edx + HIST_EXP_OFF], 0
    je .dhp_no_expand
    movzx ebx, byte [hist_entries + edx + HIST_OLINES_OFF]
    test ebx, ebx
    jz .dhp_no_expand

    ; Draw each output line
    lea esi, [hist_entries + edx + HIST_OUT_OFF]
    xor edi, edi            ; line counter
.dhp_oloop:
    cmp edi, ebx
    jge .dhp_no_expand
    inc eax                 ; next screen row
    cmp eax, PANE_BOT
    jg .dhp_no_expand
    push eax
    push ebx
    push edi
    push esi
    mov edx, RIGHT_S + 2
    mov bl, COL_OUTPUT
    call draw_at
    ; esi advanced past the string already by draw_at
    pop esi
    pop edi
    pop ebx
    pop eax
    ; Advance esi to next line (find null, skip it)
    push ecx
    xor ecx, ecx
.dhp_skip:
    cmp byte [esi], 0
    je .dhp_skip_done
    inc esi
    inc ecx
    cmp ecx, 38
    jge .dhp_skip_done
    jmp .dhp_skip
.dhp_skip_done:
    inc esi                 ; skip the null
    pop ecx
    inc edi
    jmp .dhp_oloop

.dhp_no_expand:
    pop ecx
    pop eax
    inc eax                 ; next row
    dec ecx                 ; next entry index
    jmp .dhp_loop

.dhp_done:
    pop edi
    pop esi
    pop ecx
    pop edx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Wait N PIT ticks (eax = number of ticks to wait)
; ═══════════════════════════════════════════════════
wait_ticks:
    push ecx
    push eax
    mov ecx, [tick_count]
    add ecx, eax
.wt_loop:
    call pit_poll
    cmp [tick_count], ecx
    jl .wt_loop
    pop eax
    pop ecx
    ret

; ═══════════════════════════════════════════════════
; Capture output: store output lines for hist entry
; Call after command execution.
; Uses pre_cmd_row (saved before exec) and current left_row.
; eax = hist entry index
; ═══════════════════════════════════════════════════
capture_output:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    ; Calculate number of output lines
    movzx ebx, byte [left_row]
    movzx ecx, byte [pre_cmd_row]
    sub ebx, ecx               ; ebx = number of output lines
    cmp ebx, 0
    jle .co_none
    cmp ebx, 8                 ; max 8 lines of output stored
    jle .co_ok
    mov ebx, 8
.co_ok:
    ; Get entry pointer
    movzx eax, byte [hist_entry_cnt]
    dec eax                     ; newest entry
    imul eax, HIST_ENTRY_SIZE
    lea edi, [hist_entries + eax + HIST_OUT_OFF]
    mov [hist_entries + eax + HIST_OLINES_OFF], bl

    ; Copy lines from VGA memory
    ; Source: row (pre_cmd_row + PANE_TOP) through (pre_cmd_row + PANE_TOP + ebx - 1)
    movzx ecx, byte [pre_cmd_row]
    add ecx, PANE_TOP          ; start screen row
    xor edx, edx               ; line counter
.co_line:
    cmp edx, ebx
    jge .co_done
    push ebx
    push edx
    push ecx
    ; Read from VGA: row ecx, col 0, up to LEFT_W chars
    push eax
    mov eax, ecx
    push edx
    mov edx, 0
    call vga_off               ; edi_vga = edi (but we need edi for output)
    pop edx
    pop eax
    ; edi now points to VGA row -- save our output dest
    mov esi, edi               ; esi = VGA source
    ; Restore edi to output buffer
    pop ecx
    pop edx
    pop ebx
    push ebx
    push edx
    push ecx
    ; Calculate output buffer position
    push eax
    movzx eax, byte [hist_entry_cnt]
    dec eax
    imul eax, HIST_ENTRY_SIZE
    lea edi, [hist_entries + eax + HIST_OUT_OFF]
    pop eax
    ; Advance edi to current line offset (each line up to 32 chars + null)
    push edx
    push eax
    mov eax, edx
    imul eax, 33               ; 32 chars + null per line
    add edi, eax
    pop eax
    pop edx
    ; Copy chars from VGA (every other byte is char, skip attribute)
    push ecx
    mov ecx, 32                ; max chars per output line
.co_char:
    mov al, [esi]
    cmp al, ' '
    jl .co_space
    jmp .co_store
.co_space:
    mov al, ' '
.co_store:
    mov [edi], al
    add esi, 2                 ; skip VGA attribute byte
    inc edi
    dec ecx
    jnz .co_char
    ; Trim trailing spaces and null-terminate
    dec edi
.co_trim:
    cmp byte [edi], ' '
    jne .co_trimmed
    cmp edi, hist_entries      ; safety bound
    jle .co_trimmed
    dec edi
    jmp .co_trim
.co_trimmed:
    inc edi
    mov byte [edi], 0
    pop ecx

    pop ecx
    pop edx
    pop ebx
    inc edx
    inc ecx
    jmp .co_line

.co_none:
    ; No output lines
    movzx eax, byte [hist_entry_cnt]
    dec eax
    imul eax, HIST_ENTRY_SIZE
    mov byte [hist_entries + eax + HIST_OLINES_OFF], 0
.co_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Expand animation for history entry
; ecx = hist entry index
; ═══════════════════════════════════════════════════
expand_animation:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    ; Mark as expanded
    imul edx, ecx, HIST_ENTRY_SIZE
    mov byte [hist_entries + edx + HIST_EXP_OFF], 1

    ; Get number of output lines
    movzx ebx, byte [hist_entries + edx + HIST_OLINES_OFF]
    test ebx, ebx
    jz .ea_done

    ; Redraw with animation: draw each output line with fade
    ; First redraw to show arrow change
    call draw_hist_panel

    ; Now animate: for each output line, fade in
    ; Find screen row of this entry's first output line
    ; We need to figure out which row the entry is on
    ; For simplicity, just redraw with increasing brightness
    ; Step through fade colors: dark -> medium -> bright
    xor edi, edi             ; output line index
.ea_line:
    cmp edi, ebx
    jge .ea_final

    ; Wait with ease-out delay
    push eax
    cmp edi, 4
    jg .ea_def_delay
    movzx eax, byte [ease_out_5 + edi]
    jmp .ea_do_wait
.ea_def_delay:
    mov eax, 5
.ea_do_wait:
    call wait_ticks
    pop eax

    ; Redraw panel (the line is already there, just visible)
    call draw_hist_panel

    inc edi
    jmp .ea_line

.ea_final:
    call draw_hist_panel
.ea_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Collapse animation for history entry
; ecx = hist entry index
; ═══════════════════════════════════════════════════
collapse_animation:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    imul edx, ecx, HIST_ENTRY_SIZE
    movzx ebx, byte [hist_entries + edx + HIST_OLINES_OFF]
    test ebx, ebx
    jz .ca_mark

    ; Animate fade out: briefly flash darker colors
    ; Quick reverse: redraw a couple times with delays
    mov edi, ebx
    dec edi
.ca_line:
    cmp edi, 0
    jl .ca_mark

    push eax
    cmp edi, 4
    jg .ca_def_delay
    movzx eax, byte [ease_in_5 + edi]
    jmp .ca_do_wait
.ca_def_delay:
    mov eax, 5
.ca_do_wait:
    call wait_ticks
    pop eax

    dec edi
    jmp .ca_line

.ca_mark:
    ; Mark as collapsed
    imul edx, ecx, HIST_ENTRY_SIZE
    mov byte [hist_entries + edx + HIST_EXP_OFF], 0

    call draw_hist_panel

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Right panel focus loop
; ═══════════════════════════════════════════════════
right_panel_loop:
    push eax

    ; Set focus to right panel
    mov byte [focus_panel], 1
    ; Set selected to newest entry if count > 0
    movzx eax, byte [hist_entry_cnt]
    test eax, eax
    jz .rpl_exit
    dec eax
    mov [hist_selected], al
    call draw_hist_panel

.rpl_input:
    call pit_poll
    call read_key
    test al, al
    jz .rpl_input

    cmp al, 0xFD             ; Ctrl+Alt+Left -> back to left
    je .rpl_exit

    cmp al, 0xFC             ; Up arrow
    je .rpl_up

    cmp al, 0xFB             ; Down arrow
    je .rpl_down

    cmp al, 0xFA             ; Enter -> toggle expand
    je .rpl_enter

    jmp .rpl_input

.rpl_up:
    movzx eax, byte [hist_selected]
    movzx ecx, byte [hist_entry_cnt]
    dec ecx
    cmp eax, ecx
    jge .rpl_input            ; already at top (newest)
    inc byte [hist_selected]
    call draw_hist_panel
    jmp .rpl_input

.rpl_down:
    cmp byte [hist_selected], 0
    je .rpl_input             ; already at bottom (oldest)
    dec byte [hist_selected]
    call draw_hist_panel
    jmp .rpl_input

.rpl_enter:
    movzx ecx, byte [hist_selected]
    cmp ecx, 0
    jl .rpl_input
    movzx eax, byte [hist_entry_cnt]
    cmp ecx, eax
    jge .rpl_input

    ; Toggle expand/collapse
    imul edx, ecx, HIST_ENTRY_SIZE
    cmp byte [hist_entries + edx + HIST_EXP_OFF], 0
    jne .rpl_collapse
    ; Expand
    call expand_animation
    jmp .rpl_input
.rpl_collapse:
    call collapse_animation
    jmp .rpl_input

.rpl_exit:
    mov byte [focus_panel], 0
    call draw_hist_panel
    ; Restore cursor to left pane
    call lp_cursor
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Keyboard (polling)
; ═══════════════════════════════════════════════════
; Returns: al = ASCII char, or special codes:
;   0xFF = up arrow (recall), 9 = tab, 0xFE = Ctrl+Alt+Right,
;   0xFD = Ctrl+Alt+Left, 0xFC = arrow up (hist nav),
;   0xFB = arrow down (hist nav), 0xFA = Enter (hist)
;   0 = nothing
read_key:
    push edx
    in al, KBD_STATUS
    test al, 1
    jz .rk_none
    in al, KBD_DATA
    ; Release?
    test al, 0x80
    jnz .rk_rel
    ; Ctrl press?
    cmp al, SC_CTRL
    je .rk_ctrl_on
    ; Alt press?
    cmp al, SC_ALT
    je .rk_alt_on
    ; Shift?
    cmp al, SC_LSHIFT
    je .rk_son
    cmp al, SC_RSHIFT
    je .rk_son

    ; Check Ctrl+Alt+Arrow combos
    cmp byte [ctrl_held], 0
    je .rk_no_ctrlalt
    cmp byte [alt_held], 0
    je .rk_no_ctrlalt
    ; Ctrl+Alt held -- check arrows
    cmp al, SC_RIGHT
    je .rk_ca_right
    cmp al, SC_LEFT
    je .rk_ca_left
    jmp .rk_no_ctrlalt
.rk_ca_right:
    mov al, 0xFE
    jmp .rk_done
.rk_ca_left:
    mov al, 0xFD
    jmp .rk_done

.rk_no_ctrlalt:
    ; If right panel focused, handle nav keys
    cmp byte [focus_panel], 1
    jne .rk_normal
    ; Right panel mode: Up/Down/Enter
    cmp al, SC_UP
    je .rk_hist_up
    cmp al, SC_DOWN
    je .rk_hist_down
    cmp al, SC_ENTER
    je .rk_hist_enter
    ; Ignore other keys when right panel focused
    xor al, al
    jmp .rk_done
.rk_hist_up:
    mov al, 0xFC
    jmp .rk_done
.rk_hist_down:
    mov al, 0xFB
    jmp .rk_done
.rk_hist_enter:
    mov al, 0xFA
    jmp .rk_done

.rk_normal:
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
.rk_ctrl_on:
    mov byte [ctrl_held], 1
    xor al, al
    jmp .rk_done
.rk_alt_on:
    mov byte [alt_held], 1
    xor al, al
    jmp .rk_done
.rk_rel:
    and al, 0x7F
    cmp al, SC_LSHIFT
    je .rk_soff
    cmp al, SC_RSHIFT
    je .rk_soff
    cmp al, SC_CTRL
    je .rk_ctrl_off
    cmp al, SC_ALT
    je .rk_alt_off
    xor al, al
    jmp .rk_done
.rk_soff:
    mov byte [shift_held], 0
    xor al, al
    jmp .rk_done
.rk_ctrl_off:
    mov byte [ctrl_held], 0
    xor al, al
    jmp .rk_done
.rk_alt_off:
    mov byte [alt_held], 0
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
cmd_done:
    ; Capture output from the last command execution
    call capture_output
    call draw_hist_panel

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
    cmp al, 0xFE             ; Ctrl+Alt+Right -> right panel
    je .focus_right
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

.focus_right:
    call right_panel_loop
    jmp .rl

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

    ; Save current left_row before command execution for output capture
    mov al, [left_row]
    mov [pre_cmd_row], al

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
    jmp cmd_done

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
    jmp cmd_done

.cmd_clear:
    call lp_clear
    jmp cmd_done

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
    jmp cmd_done

.cmd_hello:
    mov esi, hello_msg
    mov bl, COL_OK
    call lp_println
    jmp cmd_done

.cmd_pwd:
    call build_path
    mov esi, pathbuf
    mov bl, COL_VER
    call lp_println
    jmp cmd_done

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
    jmp cmd_done

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
    jmp cmd_done

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
    jmp cmd_done

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
    jmp cmd_done
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
    jmp cmd_done
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
    jmp cmd_done
.cd_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_done
.cd_nd:
    mov esi, e_ndir
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_done

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
    jmp cmd_done
.mk_ex:
    mov esi, e_exist
    mov bl, COL_ERR
    call lp_println
    jmp cmd_done

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
    jmp cmd_done

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
    jmp cmd_done
.wr_isdir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    jmp cmd_done
.wr_full:
    mov esi, e_full
    mov bl, COL_ERR
    call lp_println
    jmp cmd_done

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
    jmp cmd_done
.cat_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_done
.cat_dir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    jmp cmd_done

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
    jmp cmd_done
.rm_empty:
    pop ebx
    pop eax
.rm_do:
    mov byte [fs_tab + ebx + FS_TYPE], FS_FREE
    call update_panel
    jmp cmd_done
.rm_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_done

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
    jmp cmd_done
.st_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    jmp cmd_done
