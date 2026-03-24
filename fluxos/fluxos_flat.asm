; fluxos_flat.asm — FluxOS with PIT Timer, Memory Manager, RAM Filesystem
; Split-pane UI: Command area (left) | History/Info (right)
; Features: User system, shell pipes, ATA disk persistence, network (simulated)
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

; ATA PIO ports
ATA_DATA       equ 0x1F0
ATA_ERR        equ 0x1F1
ATA_COUNT      equ 0x1F2
ATA_LBALO      equ 0x1F3
ATA_LBAMID     equ 0x1F4
ATA_LBAHI      equ 0x1F5
ATA_DRIVE      equ 0x1F6
ATA_CMD        equ 0x1F7
ATA_STATUS     equ 0x1F7

; Disk constants
DISK_MAGIC     equ 0x464C5558   ; "FLUX"
DISK_START_SEC equ 100          ; Start writing at sector 100

; User system constants
MAX_USERS      equ 4
USER_NAME_SZ   equ 16
USER_PASS_SZ   equ 16
USER_HOME_SZ   equ 32
USER_ENTRY_SZ  equ 65  ; 16+16+32+1

; Pipe buffer size
PIPE_BUF_SZ    equ 4096

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

; User system BSS
user_table:    resb MAX_USERS * USER_ENTRY_SZ
user_count:    resb 1
current_uid:   resb 1
login_buf:     resb 32     ; for login input
login_len:     resb 1

; Pipe buffer
pipe_buf:      resb PIPE_BUF_SZ
pipe_len:      resd 1
pipe_active:   resb 1      ; 1 if output should go to pipe_buf
pipe_input:    resb 1      ; 1 if command reads from pipe_buf

; ATA disk
ata_present:   resb 1
disk_sector_buf: resb 512

; Original command buffer for pipe parsing
orig_cmd_buf:  resb 128
orig_cmd_len:  resb 1

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
ver_str:    db "v2.0", 0
home_lbl:   db "home", 0
prompt_sfx: db "> ", 0

welcome_msg:  db "Welcome to FluxOS.", 0
hint_msg:     db "Type 'help' for commands.", 0

help_lines:
h0:  db "Commands:", 0
h1:  db " help    - this help", 0
h2:  db " clear   - clear screen", 0
h3:  db " info    - system info", 0
h4:  db " pwd     - current dir", 0
h5:  db " uptime  - time since boot", 0
h6:  db " mem     - memory info", 0
h7:  db " ls      - list files", 0
h8:  db " cd <d>  - change dir", 0
h9:  db " mkdir <n> - make dir", 0
h10: db " touch <n> - make file", 0
h11: db " write <f> <t> - write", 0
h12: db " cat <f> - show file", 0
h13: db " rm <f>  - remove", 0
h14: db " stat <f> - file info", 0
h15: db " hello   - greeting", 0
h16: db " reboot  - restart", 0
h17: db " whoami  - current user", 0
h18: db " login   - switch user", 0
h19: db " adduser <n> - add user", 0
h20: db " save    - save fs to disk", 0
h21: db " load    - load fs from disk", 0
h22: db " disk    - disk status", 0
h23: db " ping <ip> - ping host", 0
h24: db " ifconfig - network info", 0
h25: db " grep <p> - filter lines", 0
h26: db " wc      - count lines/words", 0
h27: db " Pipes: cmd1 | cmd2", 0

info1: db "FluxOS v2.0", 0
info2: db "Arch: x86 (32-bit)", 0
info3: db "VGA: 80x25 16-color", 0
info4: db "PIT: 100Hz timer", 0
info5: db "FS: RAMfs (64 files)", 0
info6: db "ATA: PIO disk support", 0

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
def_motd:     db "FluxOS v2.0 - Flux Language Bare Metal Kernel", 0

panel_dir:    db "Dir: ", 0
panel_files:  db "Files: ", 0
panel_user:   db "User: ", 0

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
c_whoami: db "whoami", 0
c_login:  db "login", 0
c_adduser: db "adduser", 0
c_save:   db "save", 0
c_load:   db "load", 0
c_disk:   db "disk", 0
c_ping:   db "ping", 0
c_ifconfig: db "ifconfig", 0
c_grep:   db "grep", 0
c_wc:     db "wc", 0

tab_cmds: db "help clear info pwd ls cd mkdir touch write cat rm stat", 0
tab_cmds2: db "mem uptime whoami login save load disk ping ifconfig", 0

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

; Login screen strings
login_banner1: db "  FluxOS v2.0 Login", 0
login_banner2: db "  ─────────────────", 0
login_user_p:  db "  Username: ", 0
login_pass_p:  db "  Password: ", 0
login_fail:    db "  Login failed.", 0
login_ok:      db "  Login successful.", 0
login_guest:   db "guest", 0

; User system strings
s_root:        db "root", 0
s_rootpass:    db "root", 0
s_guest:       db "guest", 0
s_rootdir:     db "/root", 0
s_guestdir:    db "/home/guest", 0
s_at_flux:     db "@fluxos:", 0
s_perm_denied: db "Permission denied", 0
s_user_added:  db "User added: ", 0
s_root_only:   db "Only root can do that", 0
s_user_exists: db "User already exists", 0
s_users_full:  db "User table full", 0

; ATA/disk strings
s_disk_detect: db "ATA: ", 0
s_disk_found:  db "Primary drive detected", 0
s_disk_none:   db "No drive detected", 0
s_disk_saved:  db "Filesystem saved to disk", 0
s_disk_loaded: db "Filesystem loaded from disk", 0
s_disk_nodata: db "No saved data on disk", 0
s_disk_err:    db "Disk I/O error", 0
s_disk_status: db "Disk: ", 0
s_disk_avail:  db "Available (ATA PIO)", 0
s_disk_unavail: db "Not available", 0
s_sectors:     db " sectors used", 0

; Network strings
s_net_mac:     db "MAC: 52:54:00:12:34:56", 0
s_net_ip:      db "IP:  10.0.2.15", 0
s_net_mask:    db "Mask: 255.255.255.0", 0
s_net_gw:      db "GW:  10.0.2.2", 0
s_net_iface:   db "eth0 (simulated)", 0
s_ping_prefix: db "PING ", 0
s_ping_reply:  db " bytes from ", 0
s_ping_64:     db "64", 0
s_ping_seq:    db ": icmp_seq=", 0
s_ping_ttl:    db " ttl=64 time=", 0
s_ping_ms:     db "ms", 0
s_ping_stats:  db "--- ping statistics ---", 0
s_ping_summ:   db "4 packets sent, 4 received", 0
s_no_arg:      db "Missing argument", 0

; Pipe strings
s_pipe_sep:    db " | ", 0

; wc output
s_lines:       db " lines ", 0
s_words:       db " words ", 0
s_chars:       db " chars", 0

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
    mov byte [pipe_active], 0
    mov byte [pipe_input], 0
    mov dword [pipe_len], 0

    call pit_init
    call mem_init
    call user_init
    call fs_init
    call ata_detect

    ; Try auto-load from disk
    cmp byte [ata_present], 1
    jne .no_autoload
    call disk_load_fs
.no_autoload:

    ; Show login screen
    call login_screen
    jmp .post_login

.post_login:
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
; User System Init
; ═══════════════════════════════════════════════════
user_init:
    push eax
    push ecx
    push edi
    push esi
    ; Clear user table
    mov edi, user_table
    mov ecx, MAX_USERS * USER_ENTRY_SZ
    xor al, al
    rep stosb
    mov byte [user_count], 0
    mov byte [current_uid], 0

    ; Add root user (uid 0)
    mov edi, user_table
    mov esi, s_root
    call .copy_str16        ; name
    add edi, USER_NAME_SZ
    mov esi, s_rootpass
    call .copy_str16        ; password
    add edi, USER_PASS_SZ
    mov esi, .s_root_home
    call .copy_str32        ; home
    add edi, USER_HOME_SZ
    mov byte [edi], 0       ; uid = 0
    inc byte [user_count]

    ; Add guest user (uid 1)
    mov edi, user_table + USER_ENTRY_SZ
    mov esi, s_guest
    call .copy_str16
    add edi, USER_NAME_SZ
    ; empty password for guest
    mov byte [edi], 0
    add edi, USER_PASS_SZ
    mov esi, .s_guest_home
    call .copy_str32
    add edi, USER_HOME_SZ
    mov byte [edi], 1       ; uid = 1
    inc byte [user_count]

    pop esi
    pop edi
    pop ecx
    pop eax
    ret

.copy_str16:
    push ecx
    mov ecx, 15
.cs16:
    lodsb
    mov [edi], al
    test al, al
    jz .cs16d
    inc edi
    dec ecx
    jnz .cs16
    mov byte [edi], 0
.cs16d:
    pop ecx
    ret

.copy_str32:
    push ecx
    mov ecx, 31
.cs32:
    lodsb
    mov [edi], al
    test al, al
    jz .cs32d
    inc edi
    dec ecx
    jnz .cs32
    mov byte [edi], 0
.cs32d:
    pop ecx
    ret

.s_root_home:  db "/root", 0
.s_guest_home: db "/home/guest", 0

; ═══════════════════════════════════════════════════
; Login Screen
; ═══════════════════════════════════════════════════
login_screen:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    ; Clear screen
    mov edi, VGA_BASE
    mov ecx, VGA_COLS * VGA_ROWS
    mov ax, 0x0020
    rep stosw

    ; Draw logo centered
    mov eax, 5
    mov edx, 30
    mov esi, logo1
    mov bl, COL_LOGO
    call draw_at
    mov eax, 6
    mov edx, 30
    mov esi, logo2
    call draw_at
    mov eax, 7
    mov edx, 30
    mov esi, logo3
    call draw_at
    mov eax, 8
    mov edx, 30
    mov esi, logo4
    call draw_at
    mov eax, 9
    mov edx, 30
    mov esi, logo5
    call draw_at

    ; Banner
    mov eax, 11
    mov edx, 28
    mov esi, login_banner1
    mov bl, COL_VER
    call draw_at
    mov eax, 12
    mov edx, 28
    mov esi, login_banner2
    mov bl, COL_BORDER
    call draw_at

.login_retry:
    ; Clear username/password lines
    mov eax, 14
    mov edx, 28
    call vga_off
    mov ecx, 40
    mov ax, 0x0020
    rep stosw
    mov eax, 15
    mov edx, 28
    call vga_off
    mov ecx, 40
    mov ax, 0x0020
    rep stosw
    mov eax, 17
    mov edx, 28
    call vga_off
    mov ecx, 40
    mov ax, 0x0020
    rep stosw

    ; Username prompt
    mov eax, 14
    mov edx, 28
    mov esi, login_user_p
    mov bl, COL_PROMPT
    call draw_at

    ; Read username
    mov byte [login_len], 0
    mov edi, login_buf
    mov ecx, 30
    xor al, al
    rep stosb

    ; Cursor at row 14, col 40
    mov eax, 14
    mov edx, 40
.lu_loop:
    call pit_poll
    call read_key
    test al, al
    jz .lu_loop
    cmp al, 13
    je .lu_done
    cmp al, 8
    je .lu_bs
    cmp al, 27
    je .lu_loop
    movzx ecx, byte [login_len]
    cmp ecx, 15
    jge .lu_loop
    mov [login_buf + ecx], al
    inc byte [login_len]
    ; Display char
    push eax
    push edx
    mov eax, 14
    movzx edx, byte [login_len]
    add edx, 39
    call vga_off
    pop edx
    pop eax
    mov [edi], al
    mov byte [edi+1], COL_INPUT
    jmp .lu_loop
.lu_bs:
    cmp byte [login_len], 0
    je .lu_loop
    dec byte [login_len]
    push eax
    push edx
    mov eax, 14
    movzx edx, byte [login_len]
    add edx, 40
    call vga_off
    mov byte [edi], ' '
    mov byte [edi+1], COL_INPUT
    pop edx
    pop eax
    jmp .lu_loop
.lu_done:
    movzx ecx, byte [login_len]
    mov byte [login_buf + ecx], 0

    ; Check for guest (no password needed)
    mov esi, login_buf
    mov edi, s_guest
    call strcmp
    je .login_as_guest

    ; Password prompt
    mov eax, 15
    mov edx, 28
    mov esi, login_pass_p
    mov bl, COL_PROMPT
    call draw_at

    ; Read password into arg1 (reuse buffer)
    mov byte [arg1], 0
    xor ecx, ecx  ; length counter
.lp_loop:
    call pit_poll
    call read_key
    test al, al
    jz .lp_loop
    cmp al, 13
    je .lp_done
    cmp al, 8
    je .lp_bs
    cmp ecx, 15
    jge .lp_loop
    mov [arg1 + ecx], al
    inc ecx
    ; Show asterisk
    push eax
    push edx
    push ecx
    mov eax, 15
    lea edx, [ecx + 39]
    call vga_off
    mov byte [edi], '*'
    mov byte [edi+1], COL_INPUT
    pop ecx
    pop edx
    pop eax
    jmp .lp_loop
.lp_bs:
    test ecx, ecx
    jz .lp_loop
    dec ecx
    push eax
    push edx
    push ecx
    mov eax, 15
    lea edx, [ecx + 40]
    call vga_off
    mov byte [edi], ' '
    mov byte [edi+1], COL_INPUT
    pop ecx
    pop edx
    pop eax
    jmp .lp_loop
.lp_done:
    mov byte [arg1 + ecx], 0

    ; Verify credentials
    call user_authenticate
    cmp eax, -1
    je .login_failed

    mov [current_uid], al
    jmp .login_success

.login_as_guest:
    mov byte [current_uid], 1
    jmp .login_success

.login_failed:
    mov eax, 17
    mov edx, 28
    mov esi, login_fail
    mov bl, COL_ERR
    call draw_at
    ; Wait a second
    mov eax, 100
    call wait_ticks
    jmp .login_retry

.login_success:
    mov eax, 17
    mov edx, 28
    mov esi, login_ok
    mov bl, COL_OK
    call draw_at
    mov eax, 50
    call wait_ticks

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; user_authenticate: login_buf=username, arg1=password -> eax=uid or -1
user_authenticate:
    push ebx
    push ecx
    push edx
    push esi
    push edi
    movzx ecx, byte [user_count]
    xor ebx, ebx
.ua_loop:
    cmp ebx, ecx
    jge .ua_fail
    ; Compare username
    imul edx, ebx, USER_ENTRY_SZ
    lea esi, [user_table + edx]
    mov edi, login_buf
    push ecx
    push ebx
    call strcmp
    pop ebx
    pop ecx
    jne .ua_next
    ; Compare password
    imul edx, ebx, USER_ENTRY_SZ
    lea esi, [user_table + edx + USER_NAME_SZ]
    mov edi, arg1
    push ecx
    push ebx
    call strcmp
    pop ebx
    pop ecx
    jne .ua_next
    ; Match
    mov eax, ebx
    jmp .ua_ret
.ua_next:
    inc ebx
    jmp .ua_loop
.ua_fail:
    mov eax, -1
.ua_ret:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
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
    push eax                ; save home index

    ; /tmp (dir, parent=0)
    push dword .s_tmp
    push dword FS_DIR
    push dword 0
    call fs_add_entry
    add esp, 12

    ; /root (dir, parent=0)
    push dword .s_rootdir
    push dword FS_DIR
    push dword 0
    call fs_add_entry
    add esp, 12

    ; /home/guest (dir, parent=home)
    pop eax                 ; home index
    push dword .s_guestdir
    push dword FS_DIR
    push eax
    call fs_add_entry
    add esp, 12

    ret

.s_readme: db "README", 0
.s_etc:    db "etc", 0
.s_motd:   db "motd", 0
.s_home:   db "home", 0
.s_tmp:    db "tmp", 0
.s_rootdir: db "root", 0
.s_guestdir: db "guest", 0

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
    ; File count
    mov eax, 2
    mov edx, 58
    mov esi, panel_files
    mov bl, COL_DIR
    call draw_at
    push eax
    movzx eax, byte [fs_count]
    call itoa
    pop eax
    mov eax, 2
    mov edx, 66
    mov esi, numbuf
    mov bl, COL_OUTPUT
    call draw_at
    ; Current user
    mov eax, 3
    mov edx, 58
    mov esi, panel_user
    mov bl, COL_DIR
    call draw_at
    ; Get username
    push eax
    movzx eax, byte [current_uid]
    imul eax, USER_ENTRY_SZ
    lea esi, [user_table + eax]
    pop eax
    mov eax, 3
    mov edx, 64
    mov bl, COL_VER
    call draw_at
    ; Disk status
    mov eax, 4
    mov edx, 58
    mov esi, s_disk_status
    mov bl, COL_DIR
    call draw_at
    mov eax, 4
    mov edx, 64
    cmp byte [ata_present], 1
    jne .up_nodisk
    mov esi, .s_yes
    mov bl, COL_OK
    jmp .up_ddone
.up_nodisk:
    mov esi, .s_no
    mov bl, COL_ERR
.up_ddone:
    call draw_at
    pop esi
    pop ecx
    pop edx
    pop eax
    ret
.s_yes: db "Yes", 0
.s_no:  db "No", 0

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
; Left pane output (with pipe support)
; ═══════════════════════════════════════════════════
lp_putc:
    ; al=char, ah=color
    ; If pipe_active, write to pipe_buf instead
    cmp byte [pipe_active], 1
    je .lpc_pipe
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
.lpc_pipe:
    ; Write char to pipe buffer
    push ebx
    mov ebx, [pipe_len]
    cmp ebx, PIPE_BUF_SZ - 1
    jge .lpc_pipe_full
    mov [pipe_buf + ebx], al
    inc dword [pipe_len]
.lpc_pipe_full:
    pop ebx
    ret

lp_newline:
    cmp byte [pipe_active], 1
    je .ln_pipe
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
.ln_pipe:
    ; Write newline char to pipe buffer
    push ebx
    mov ebx, [pipe_len]
    cmp ebx, PIPE_BUF_SZ - 1
    jge .ln_pipe_full
    mov byte [pipe_buf + ebx], 10  ; LF
    inc dword [pipe_len]
.ln_pipe_full:
    pop ebx
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
    call vga_off
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

    call draw_hist_panel

    xor edi, edi             ; output line index
.ea_line:
    cmp edi, ebx
    jge .ea_final

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
    jge .rpl_input
    inc byte [hist_selected]
    call draw_hist_panel
    jmp .rpl_input

.rpl_down:
    cmp byte [hist_selected], 0
    je .rpl_input
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
    call expand_animation
    jmp .rpl_input
.rpl_collapse:
    call collapse_animation
    jmp .rpl_input

.rpl_exit:
    mov byte [focus_panel], 0
    call draw_hist_panel
    call lp_cursor
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
    cmp al, SC_UP
    je .rk_hist_up
    cmp al, SC_DOWN
    je .rk_hist_down
    cmp al, SC_ENTER
    je .rk_hist_enter
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
    cmp al, SC_UP
    je .rk_up
    cmp al, SC_TAB
    je .rk_tab
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
; Pipe support: check for | in command, split and execute
; ═══════════════════════════════════════════════════
; check_pipe: scan cmd_buf for '|'. If found, split into
; left command (cmd_buf) and right command (orig_cmd_buf),
; execute left with pipe_active=1, then execute right with pipe_input=1
; Returns: al=1 if pipe was found and handled, al=0 otherwise
check_pipe:
    push ebx
    push ecx
    push edx
    push esi
    push edi
    ; Scan for '|'
    mov esi, cmd_buf
    xor ecx, ecx
.cp_scan:
    mov al, [esi + ecx]
    test al, al
    jz .cp_nopipe
    cmp al, '|'
    je .cp_found
    inc ecx
    cmp ecx, 127
    jge .cp_nopipe
    jmp .cp_scan

.cp_found:
    ; Found pipe at offset ecx
    ; Null-terminate left command (trim trailing spaces)
    mov byte [esi + ecx], 0
    ; Trim trailing spaces from left side
    dec ecx
.cp_triml:
    cmp ecx, 0
    jl .cp_triml_done
    cmp byte [esi + ecx], ' '
    jne .cp_triml_done
    mov byte [esi + ecx], 0
    dec ecx
    jmp .cp_triml
.cp_triml_done:
    inc ecx
    mov [orig_cmd_len], cl

    ; Copy right command to orig_cmd_buf (skip | and leading spaces)
    mov esi, cmd_buf
    add esi, ecx
    ; esi now points to the null where | was; find start of right cmd
    ; Actually we nulled the |, so we need original position
    ; Let me re-find: the | was at the position we stored
    ; We need to go past the null to find the right side
    inc esi     ; skip the null (was |)
.cp_skip_sp:
    cmp byte [esi], ' '
    jne .cp_copy_right
    inc esi
    jmp .cp_skip_sp
.cp_copy_right:
    mov edi, orig_cmd_buf
    mov ecx, 127
.cp_cpr:
    lodsb
    stosb
    test al, al
    jz .cp_cpr_done
    dec ecx
    jnz .cp_cpr
    mov byte [edi], 0
.cp_cpr_done:

    ; Execute left command with output to pipe buffer
    mov dword [pipe_len], 0
    mov byte [pipe_active], 1
    mov byte [pipe_input], 0

    ; We need to recalculate cmd_len for the left part
    mov esi, cmd_buf
    xor ecx, ecx
.cp_leftlen:
    cmp byte [esi + ecx], 0
    je .cp_leftlen_done
    inc ecx
    jmp .cp_leftlen
.cp_leftlen_done:
    mov [cmd_len], cl

    call parse_args
    call exec_command

    mov byte [pipe_active], 0
    ; Null-terminate pipe buffer
    mov ecx, [pipe_len]
    mov byte [pipe_buf + ecx], 0

    ; Now set up right command
    mov esi, orig_cmd_buf
    mov edi, cmd_buf
    mov ecx, 128
.cp_copy_back:
    lodsb
    stosb
    test al, al
    jz .cp_copy_back_done
    dec ecx
    jnz .cp_copy_back
.cp_copy_back_done:
    ; Recalculate cmd_len
    mov esi, cmd_buf
    xor ecx, ecx
.cp_rightlen:
    cmp byte [esi + ecx], 0
    je .cp_rightlen_done
    inc ecx
    jmp .cp_rightlen
.cp_rightlen_done:
    mov [cmd_len], cl

    ; Execute right command with pipe as input
    mov byte [pipe_input], 1
    call parse_args
    call exec_command
    mov byte [pipe_input], 0

    mov al, 1
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

.cp_nopipe:
    mov al, 0
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

; ═══════════════════════════════════════════════════
; ATA Disk Driver (PIO mode)
; ═══════════════════════════════════════════════════
ata_detect:
    push eax
    push edx
    mov byte [ata_present], 0
    ; Select drive 0
    mov dx, ATA_DRIVE
    mov al, 0xA0
    out dx, al
    ; Small delay
    mov dx, ATA_STATUS
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    ; Check status
    in al, dx
    cmp al, 0xFF
    je .ad_none
    test al, al
    jz .ad_none
    mov byte [ata_present], 1
.ad_none:
    pop edx
    pop eax
    ret

; ata_read_sector: eax=LBA sector number, edi=dest buffer (512 bytes)
ata_read_sector:
    push eax
    push ebx
    push ecx
    push edx
    mov ebx, eax        ; save LBA

    ; Wait for drive ready
    mov dx, ATA_STATUS
.ars_wait1:
    in al, dx
    test al, 0x80       ; BSY
    jnz .ars_wait1

    ; Set sector count = 1
    mov dx, ATA_COUNT
    mov al, 1
    out dx, al

    ; LBA low
    mov dx, ATA_LBALO
    mov al, bl
    out dx, al

    ; LBA mid
    mov dx, ATA_LBAMID
    mov eax, ebx
    shr eax, 8
    out dx, al

    ; LBA high
    mov dx, ATA_LBAHI
    mov eax, ebx
    shr eax, 16
    out dx, al

    ; Drive/head with LBA bit
    mov dx, ATA_DRIVE
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0         ; LBA mode, drive 0
    out dx, al

    ; Send read command
    mov dx, ATA_CMD
    mov al, 0x20
    out dx, al

    ; Wait for data ready
    mov dx, ATA_STATUS
.ars_wait2:
    in al, dx
    test al, 0x80       ; BSY
    jnz .ars_wait2
    test al, 0x08       ; DRQ
    jz .ars_wait2

    ; Read 256 words (512 bytes)
    mov dx, ATA_DATA
    mov ecx, 256
    rep insw

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ata_write_sector: eax=LBA sector number, esi=source buffer (512 bytes)
ata_write_sector:
    push eax
    push ebx
    push ecx
    push edx
    mov ebx, eax        ; save LBA

    ; Wait for drive ready
    mov dx, ATA_STATUS
.aws_wait1:
    in al, dx
    test al, 0x80
    jnz .aws_wait1

    ; Sector count = 1
    mov dx, ATA_COUNT
    mov al, 1
    out dx, al

    ; LBA
    mov dx, ATA_LBALO
    mov al, bl
    out dx, al
    mov dx, ATA_LBAMID
    mov eax, ebx
    shr eax, 8
    out dx, al
    mov dx, ATA_LBAHI
    mov eax, ebx
    shr eax, 16
    out dx, al
    mov dx, ATA_DRIVE
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    out dx, al

    ; Write command
    mov dx, ATA_CMD
    mov al, 0x30
    out dx, al

    ; Wait for ready
    mov dx, ATA_STATUS
.aws_wait2:
    in al, dx
    test al, 0x80
    jnz .aws_wait2
    test al, 0x08
    jz .aws_wait2

    ; Write 256 words
    mov dx, ATA_DATA
    mov ecx, 256
    rep outsw

    ; Flush cache
    mov dx, ATA_CMD
    mov al, 0xE7
    out dx, al
    mov dx, ATA_STATUS
.aws_flush:
    in al, dx
    test al, 0x80
    jnz .aws_flush

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Disk save/load filesystem
; ═══════════════════════════════════════════════════
; Format on disk:
;   Sector 100: Magic(4) + fs_count(4) + padding to 512
;   Sector 101+: fs_tab entries (64 bytes each, 8 per sector)
;   After fs_tab: file data blocks

disk_save_fs:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    cmp byte [ata_present], 1
    jne .dsf_err

    ; Build header sector
    mov edi, disk_sector_buf
    mov ecx, 512
    xor al, al
    rep stosb              ; clear buffer

    mov edi, disk_sector_buf
    mov dword [edi], DISK_MAGIC
    movzx eax, byte [fs_count]
    mov [edi + 4], eax

    ; Write header to sector 100
    mov eax, DISK_START_SEC
    mov esi, disk_sector_buf
    call ata_write_sector

    ; Write fs_tab: 64 entries * 64 bytes = 4096 bytes = 8 sectors
    mov ecx, 0             ; sector offset
    mov ebx, 0             ; byte offset in fs_tab
.dsf_tab_loop:
    cmp ecx, 8
    jge .dsf_data

    ; Copy 512 bytes of fs_tab to sector buffer
    push ecx
    mov edi, disk_sector_buf
    lea esi, [fs_tab + ebx]
    mov ecx, 512
    rep movsb
    pop ecx

    ; Write sector
    push ecx
    lea eax, [DISK_START_SEC + 1]
    add eax, ecx
    mov esi, disk_sector_buf
    call ata_write_sector
    pop ecx

    add ebx, 512
    inc ecx
    jmp .dsf_tab_loop

.dsf_data:
    ; Now write file content data
    ; For each file entry with data, write its content
    ; We'll write the heap region
    ; Sector 109+: heap data
    mov esi, heap
    mov eax, [heap_ptr]
    sub eax, esi           ; eax = bytes used in heap
    test eax, eax
    jz .dsf_done

    mov ebx, eax           ; total bytes to write
    mov ecx, 0             ; sector counter
.dsf_heap_loop:
    cmp ebx, 0
    jle .dsf_done

    ; Copy up to 512 bytes to sector buffer
    push ecx
    push ebx
    mov edi, disk_sector_buf
    ; Clear buffer first
    push ecx
    push edi
    mov ecx, 512
    xor al, al
    rep stosb
    pop edi
    pop ecx

    ; Copy bytes
    cmp ebx, 512
    jle .dsf_partial
    mov ecx, 512
    jmp .dsf_copy
.dsf_partial:
    mov ecx, ebx
.dsf_copy:
    rep movsb              ; esi advances through heap
    pop ebx
    pop ecx

    ; Write sector
    push ecx
    push esi
    lea eax, [DISK_START_SEC + 9]
    add eax, ecx
    mov esi, disk_sector_buf
    call ata_write_sector
    pop esi
    pop ecx

    sub ebx, 512
    inc ecx
    jmp .dsf_heap_loop

.dsf_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

.dsf_err:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; disk_load_fs: load filesystem from disk
disk_load_fs:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    cmp byte [ata_present], 1
    jne .dlf_done

    ; Read header sector
    mov eax, DISK_START_SEC
    mov edi, disk_sector_buf
    call ata_read_sector

    ; Check magic
    cmp dword [disk_sector_buf], DISK_MAGIC
    jne .dlf_done

    ; Get fs_count
    mov eax, [disk_sector_buf + 4]
    cmp eax, FS_MAXF
    jg .dlf_done
    mov [fs_count], al

    ; Read fs_tab: 8 sectors
    mov ecx, 0
    mov ebx, 0
.dlf_tab_loop:
    cmp ecx, 8
    jge .dlf_heap

    push ecx
    lea eax, [DISK_START_SEC + 1]
    add eax, ecx
    lea edi, [fs_tab + ebx]
    call ata_read_sector
    pop ecx

    add ebx, 512
    inc ecx
    jmp .dlf_tab_loop

.dlf_heap:
    ; Read heap data
    ; We need to know how much heap was used
    ; Reconstruct heap_ptr from file entries
    mov eax, heap
    mov [heap_ptr], eax

    ; Read heap sectors (read up to 128 sectors = 64KB)
    mov ecx, 0
    mov edi, heap
.dlf_heap_loop:
    cmp ecx, 128
    jge .dlf_fixptrs

    push ecx
    push edi
    lea eax, [DISK_START_SEC + 9]
    add eax, ecx
    call ata_read_sector
    pop edi
    pop ecx

    add edi, 512
    inc ecx
    jmp .dlf_heap_loop

.dlf_fixptrs:
    ; Fix heap_ptr by scanning entries for max (dptr + size)
    movzx ecx, byte [fs_count]
    mov ebx, 0             ; max end
    xor edx, edx
.dlf_scan:
    cmp edx, ecx
    jge .dlf_setptr
    imul eax, edx, FS_ENTRY_SZ
    cmp byte [fs_tab + eax + FS_TYPE], FS_FILE
    jne .dlf_scan_next
    mov eax, [fs_tab + eax + FS_DPTR]
    test eax, eax
    jz .dlf_scan_next
    ; eax = data pointer; add size
    push edx
    imul edx, edx, FS_ENTRY_SZ
    add eax, [fs_tab + edx + FS_SIZE]
    inc eax                ; null terminator
    pop edx
    cmp eax, ebx
    jle .dlf_scan_next
    mov ebx, eax
.dlf_scan_next:
    inc edx
    jmp .dlf_scan
.dlf_setptr:
    test ebx, ebx
    jz .dlf_done
    mov [heap_ptr], ebx

.dlf_done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ═══════════════════════════════════════════════════
; Print prompt with user@fluxos:/path>
; ═══════════════════════════════════════════════════
print_prompt:
    push eax
    push esi
    ; Print username
    movzx eax, byte [current_uid]
    imul eax, USER_ENTRY_SZ
    lea esi, [user_table + eax]
    mov bl, COL_OK
    call lp_print
    ; Print @fluxos:
    mov esi, s_at_flux
    mov bl, COL_OUTPUT
    call lp_print
    ; Print path
    call build_path
    mov esi, pathbuf
    mov bl, COL_BLUE
    call lp_print
    ; Print >
    mov esi, prompt_sfx
    mov bl, COL_PROMPT
    call lp_print
    pop esi
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
    call print_prompt

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
    mov esi, tab_cmds2
    call lp_println
    call print_prompt
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

    ; Check for pipe
    call check_pipe
    test al, al
    jnz cmd_done

    ; No pipe, execute normally
    call parse_args
    call exec_command
    jmp cmd_done

; ═══════════════════════════════════════════════════
; exec_command: dispatch current cmd_buf to handler
; ═══════════════════════════════════════════════════
exec_command:
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
    mov esi, cmd_buf
    mov edi, c_whoami
    call strcmp
    je .cmd_whoami
    mov esi, cmd_buf
    mov edi, c_login
    call strcmp
    je .cmd_login
    mov esi, cmd_buf
    mov edi, c_save
    call strcmp
    je .cmd_save
    mov esi, cmd_buf
    mov edi, c_load
    call strcmp
    je .cmd_load_disk
    mov esi, cmd_buf
    mov edi, c_disk
    call strcmp
    je .cmd_disk
    mov esi, cmd_buf
    mov edi, c_ifconfig
    call strcmp
    je .cmd_ifconfig
    mov esi, cmd_buf
    mov edi, c_wc
    call strcmp
    je .cmd_wc

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
    mov edi, c_adduser
    call strcmp_pfx
    je .cmd_adduser
    mov edi, c_ping
    call strcmp_pfx
    je .cmd_ping
    mov edi, c_grep
    call strcmp_pfx
    je .cmd_grep

    ; Unknown
    mov esi, unknown_msg
    mov bl, COL_ERR
    call lp_print
    mov esi, cmd_buf
    mov bl, COL_ERR
    call lp_println
    ret

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
    mov esi, h17
    call lp_println
    mov esi, h18
    call lp_println
    mov esi, h19
    call lp_println
    mov esi, h20
    call lp_println
    mov esi, h21
    call lp_println
    mov esi, h22
    call lp_println
    mov esi, h23
    call lp_println
    mov esi, h24
    call lp_println
    mov esi, h25
    call lp_println
    mov esi, h26
    call lp_println
    mov esi, h27
    call lp_println
    ret

.cmd_clear:
    call lp_clear
    ret

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
    mov esi, info6
    call lp_println
    ret

.cmd_hello:
    mov esi, hello_msg
    mov bl, COL_OK
    call lp_println
    ret

.cmd_pwd:
    call build_path
    mov esi, pathbuf
    mov bl, COL_VER
    call lp_println
    ret

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
    ret

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
    ret

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
    ret

; ── cd ──
.cmd_cd:
    cmp byte [arg1], 0
    je .cmd_cd_ret
    ; cd /
    cmp byte [arg1], '/'
    jne .cd_nr
    cmp byte [arg1 + 1], 0
    jne .cd_nr
    mov byte [fs_cwd], 0
    call update_panel
    ret
.cd_nr:
    ; cd ..
    mov esi, arg1
    mov edi, s_dotdot
    call strcmp
    jne .cd_named
    movzx eax, byte [fs_cwd]
    test eax, eax
    jz .cmd_cd_ret
    imul eax, FS_ENTRY_SZ
    movzx eax, byte [fs_tab + eax + FS_PAR]
    cmp al, 0xFF
    jne .cd_par
    xor al, al
.cd_par:
    mov [fs_cwd], al
    call update_panel
    ret
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
    ret
.cd_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.cd_nd:
    mov esi, e_ndir
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.cmd_cd_ret:
    ret

; ── mkdir ──
.cmd_mkdir:
    cmp byte [arg1], 0
    je .cmd_mkdir_ret
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
    ret
.mk_ex:
    mov esi, e_exist
    mov bl, COL_ERR
    call lp_println
    ret
.cmd_mkdir_ret:
    ret

; ── touch ──
.cmd_touch:
    cmp byte [arg1], 0
    je .cmd_touch_ret
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    jne .cmd_touch_ret
    movzx eax, byte [fs_cwd]
    push dword arg1
    push dword FS_FILE
    push eax
    call fs_add_entry
    add esp, 12
    call update_panel
    ret
.cmd_touch_ret:
    ret

; ── write ──
.cmd_write:
    cmp byte [arg1], 0
    je .cmd_write_ret
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
    je .cmd_write_ret
    push dword arg2
    push eax
    call fs_set_content
    add esp, 8
    call update_panel
    ret
.wr_isdir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    ret
.wr_full:
    mov esi, e_full
    mov bl, COL_ERR
    call lp_println
    ret
.cmd_write_ret:
    ret

; ── cat ──
.cmd_cat:
    cmp byte [arg1], 0
    je .cmd_cat_ret
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
    jz .cmd_cat_ret
    mov bl, COL_OUTPUT
    call lp_println
    ret
.cat_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.cat_dir:
    mov esi, e_isdir
    mov bl, COL_ERR
    call lp_println
    ret
.cmd_cat_ret:
    ret

; ── rm ──
.cmd_rm:
    cmp byte [arg1], 0
    je .cmd_rm_ret
    ; Permission check: non-root can't remove system files
    cmp byte [current_uid], 0
    je .rm_allowed
    ; Check if it's in root dir (parent=0) - protect system files for non-root
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .rm_nf
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_PAR], 0
    jne .rm_allowed_entry
    ; Non-root trying to remove root-level entry
    mov esi, s_perm_denied
    mov bl, COL_ERR
    call lp_println
    ret
.rm_allowed:
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .rm_nf
    imul ebx, eax, FS_ENTRY_SZ
.rm_allowed_entry:
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
    ret
.rm_empty:
    pop ebx
    pop eax
.rm_do:
    mov byte [fs_tab + ebx + FS_TYPE], FS_FREE
    call update_panel
    ret
.rm_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.cmd_rm_ret:
    ret

; ── stat ──
.cmd_stat:
    cmp byte [arg1], 0
    je .cmd_stat_ret
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
    ret
.st_nf:
    mov esi, e_nf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.cmd_stat_ret:
    ret

; ── whoami ──
.cmd_whoami:
    movzx eax, byte [current_uid]
    imul eax, USER_ENTRY_SZ
    lea esi, [user_table + eax]
    mov bl, COL_VER
    call lp_println
    ret

; ── login ──
.cmd_login:
    ; Re-show login screen
    call login_screen
    ; Redraw UI
    call draw_ui
    call update_panel
    ret

; ── adduser ──
.cmd_adduser:
    ; Only root can add users
    cmp byte [current_uid], 0
    jne .au_denied
    cmp byte [arg1], 0
    je .au_noarg
    ; Check if user_count < MAX_USERS
    movzx eax, byte [user_count]
    cmp eax, MAX_USERS
    jge .au_full
    ; Check if user already exists
    xor ecx, ecx
.au_check:
    cmp ecx, eax
    jge .au_add
    push eax
    push ecx
    imul edx, ecx, USER_ENTRY_SZ
    lea esi, [user_table + edx]
    mov edi, arg1
    call strcmp
    pop ecx
    pop eax
    je .au_exists
    inc ecx
    jmp .au_check
.au_add:
    ; Add new user with empty password
    imul edx, eax, USER_ENTRY_SZ
    lea edi, [user_table + edx]
    mov esi, arg1
    mov ecx, 15
.au_cpname:
    lodsb
    mov [edi], al
    test al, al
    jz .au_cpname_done
    inc edi
    dec ecx
    jnz .au_cpname
    mov byte [edi], 0
.au_cpname_done:
    ; Set uid
    imul edx, eax, USER_ENTRY_SZ
    lea edi, [user_table + edx + USER_NAME_SZ + USER_PASS_SZ + USER_HOME_SZ]
    mov [edi], al
    inc byte [user_count]
    mov esi, s_user_added
    mov bl, COL_OK
    call lp_print
    mov esi, arg1
    call lp_println
    ret
.au_denied:
    mov esi, s_root_only
    mov bl, COL_ERR
    call lp_println
    ret
.au_full:
    mov esi, s_users_full
    mov bl, COL_ERR
    call lp_println
    ret
.au_exists:
    mov esi, s_user_exists
    mov bl, COL_ERR
    call lp_println
    ret
.au_noarg:
    mov esi, s_no_arg
    mov bl, COL_ERR
    call lp_println
    ret

; ── save ──
.cmd_save:
    cmp byte [ata_present], 1
    jne .save_nodisk
    call disk_save_fs
    mov esi, s_disk_saved
    mov bl, COL_OK
    call lp_println
    ret
.save_nodisk:
    mov esi, s_disk_unavail
    mov bl, COL_ERR
    call lp_println
    ret

; ── load ──
.cmd_load_disk:
    cmp byte [ata_present], 1
    jne .load_nodisk
    call disk_load_fs
    ; Check if load succeeded by verifying magic
    cmp dword [disk_sector_buf], DISK_MAGIC
    jne .load_nodata
    mov esi, s_disk_loaded
    mov bl, COL_OK
    call lp_println
    call update_panel
    ret
.load_nodisk:
    mov esi, s_disk_unavail
    mov bl, COL_ERR
    call lp_println
    ret
.load_nodata:
    mov esi, s_disk_nodata
    mov bl, COL_ERR
    call lp_println
    ret

; ── disk ──
.cmd_disk:
    mov esi, s_disk_status
    mov bl, COL_OUTPUT
    call lp_print
    cmp byte [ata_present], 1
    jne .disk_no
    mov esi, s_disk_avail
    mov bl, COL_OK
    call lp_println
    ret
.disk_no:
    mov esi, s_disk_unavail
    mov bl, COL_ERR
    call lp_println
    ret

; ── ping (simulated) ──
.cmd_ping:
    cmp byte [arg1], 0
    je .ping_noarg
    ; Display simulated ping output
    mov esi, s_ping_prefix
    mov bl, COL_OUTPUT
    call lp_print
    mov esi, arg1
    call lp_println

    ; Simulate 4 ping replies
    mov ecx, 1
.ping_loop:
    cmp ecx, 5
    jge .ping_stats
    push ecx

    mov esi, s_ping_64
    mov bl, COL_OUTPUT
    call lp_print
    mov esi, s_ping_reply
    call lp_print
    mov esi, arg1
    call lp_print
    mov esi, s_ping_seq
    call lp_print
    ; Print sequence number
    pop ecx
    push ecx
    mov eax, ecx
    call itoa
    mov esi, numbuf
    call lp_print
    mov esi, s_ping_ttl
    call lp_print
    ; Simulated time (1-4ms)
    pop ecx
    push ecx
    mov eax, ecx
    call itoa
    mov esi, numbuf
    call lp_print
    mov esi, s_ping_ms
    call lp_println

    ; Small delay for realism
    mov eax, 20
    call wait_ticks

    pop ecx
    inc ecx
    jmp .ping_loop

.ping_stats:
    mov esi, s_ping_stats
    mov bl, COL_OUTPUT
    call lp_println
    mov esi, s_ping_summ
    call lp_println
    ret
.ping_noarg:
    mov esi, s_no_arg
    mov bl, COL_ERR
    call lp_println
    ret

; ── ifconfig ──
.cmd_ifconfig:
    mov esi, s_net_iface
    mov bl, COL_OK
    call lp_println
    mov bl, COL_OUTPUT
    mov esi, s_net_mac
    call lp_println
    mov esi, s_net_ip
    call lp_println
    mov esi, s_net_mask
    call lp_println
    mov esi, s_net_gw
    call lp_println
    ret

; ── grep (works with pipe input or standalone) ──
.cmd_grep:
    cmp byte [arg1], 0
    je .grep_noarg
    cmp byte [pipe_input], 1
    jne .grep_nodata
    ; Filter pipe_buf lines by arg1 pattern
    mov esi, pipe_buf
    mov bl, COL_OUTPUT
.grep_line:
    cmp byte [esi], 0
    je .grep_done
    ; Copy current line to a temp check
    push esi
    call .grep_check_line
    pop esi
    ; Advance to next line
.grep_advance:
    cmp byte [esi], 0
    je .grep_done
    cmp byte [esi], 10     ; LF
    je .grep_nextline
    inc esi
    jmp .grep_advance
.grep_nextline:
    inc esi
    jmp .grep_line
.grep_done:
    ret
.grep_noarg:
    mov esi, s_no_arg
    mov bl, COL_ERR
    call lp_println
    ret
.grep_nodata:
    ; No pipe input - just print message
    mov esi, .s_grep_usage
    mov bl, COL_OUTPUT
    call lp_println
    ret
.s_grep_usage: db "Usage: cmd | grep <pattern>", 0

; .grep_check_line: esi=start of line in pipe_buf
; Check if line contains arg1 substring. If yes, print it.
.grep_check_line:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    ; First, find end of this line
    mov edi, esi
    xor ecx, ecx
.gcl_findend:
    mov al, [edi + ecx]
    test al, al
    jz .gcl_gotend
    cmp al, 10
    je .gcl_gotend
    inc ecx
    cmp ecx, 127
    jge .gcl_gotend
    jmp .gcl_findend
.gcl_gotend:
    ; ecx = length of line
    test ecx, ecx
    jz .gcl_nomatch
    ; Search for arg1 substring in line
    mov edx, 0             ; position in line
.gcl_outer:
    cmp edx, ecx
    jge .gcl_nomatch
    ; Compare arg1 starting at line[edx]
    push esi
    push edx
    lea esi, [esi + edx]   ; esi = line + edx (source to compare)
    mov edi, arg1
    xor ebx, ebx
.gcl_inner:
    mov al, [edi + ebx]
    test al, al
    jz .gcl_match           ; end of pattern = match!
    cmp al, [esi + ebx]
    jne .gcl_inner_fail
    inc ebx
    jmp .gcl_inner
.gcl_inner_fail:
    pop edx
    pop esi
    inc edx
    jmp .gcl_outer
.gcl_match:
    pop edx
    pop esi
    ; Match found! Print this line
    push esi
    mov bl, COL_OUTPUT
    ; Print chars until LF or null
    xor ecx, ecx
.gcl_print:
    mov al, [esi + ecx]
    test al, al
    jz .gcl_print_done
    cmp al, 10
    je .gcl_print_done
    mov ah, bl
    call lp_putc
    inc ecx
    jmp .gcl_print
.gcl_print_done:
    call lp_newline
    pop esi
.gcl_nomatch:
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ── wc (count lines/words/chars from pipe or empty) ──
.cmd_wc:
    cmp byte [pipe_input], 1
    jne .wc_nodata
    ; Count lines, words, chars in pipe_buf
    mov esi, pipe_buf
    xor eax, eax           ; line count
    xor ebx, ebx           ; word count
    xor ecx, ecx           ; char count
    xor edx, edx           ; in_word flag
.wc_loop:
    mov dl, [esi]
    test dl, dl
    jz .wc_print
    inc ecx                 ; char count
    cmp dl, 10              ; newline
    je .wc_newline
    cmp dl, ' '
    je .wc_space
    ; Non-space char
    cmp byte [esi - 1], ' '
    je .wc_newword
    cmp byte [esi - 1], 10
    je .wc_newword
    ; Check if esi == pipe_buf (start of buffer)
    cmp esi, pipe_buf
    je .wc_newword
    jmp .wc_next
.wc_newword:
    inc ebx
    jmp .wc_next
.wc_newline:
    inc eax
    jmp .wc_next
.wc_space:
.wc_next:
    inc esi
    jmp .wc_loop
.wc_print:
    ; If last char was not newline but there was content, count one more line
    test ecx, ecx
    jz .wc_show
    cmp byte [esi - 1], 10
    je .wc_show
    inc eax                 ; partial last line
.wc_show:
    push ebx
    push ecx
    call itoa               ; eax = lines
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_lines
    mov bl, COL_OUTPUT
    call lp_print
    pop ecx
    pop eax                 ; was ebx (word count)
    push ecx
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_words
    mov bl, COL_OUTPUT
    call lp_print
    pop eax                 ; char count
    call itoa
    mov esi, numbuf
    mov bl, COL_VER
    call lp_print
    mov esi, s_chars
    mov bl, COL_OUTPUT
    call lp_println
    ret
.wc_nodata:
    mov esi, .s_wc_usage
    mov bl, COL_OUTPUT
    call lp_println
    ret
.s_wc_usage: db "Usage: cmd | wc", 0
