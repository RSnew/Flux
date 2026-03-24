; ═══════════════════════════════════════════════════════════════════
; multitask.asm — Cooperative Multitasking for FluxOS
; ═══════════════════════════════════════════════════════════════════
;
; Simple round-robin cooperative multitasking with:
;   - 4 task slots (task 0 = shell, tasks 1-3 = user scripts)
;   - Per-task 4KB stacks allocated from the page allocator
;   - Cooperative yield (tasks must call mt_yield voluntarily)
;   - Per-task output buffers (foreground task writes to VGA)
;   - Shell commands: run, ps, kill
;   - Ctrl+Tab to cycle foreground task
;
; ═══════════════════════════════════════════════════════════════════
; INTEGRATION INSTRUCTIONS
; ═══════════════════════════════════════════════════════════════════
; Add to fluxos_flat.asm:
;
; 1. BSS variables — add after existing BSS block:
;      mt_tasks:       resb MT_MAX_TASKS * MT_TASK_SZ  ; task table
;      mt_current:     resd 1          ; index of currently running task
;      mt_fg_task:     resd 1          ; index of foreground task (gets VGA output)
;      mt_task_count:  resd 1          ; number of active tasks
;      mt_outbufs:     resb MT_MAX_TASKS * MT_OUTBUF_SZ ; per-task output ring buffers
;      mt_outlen:      resd MT_MAX_TASKS  ; bytes in each output buffer
;
; 2. Data strings — add to data section:
;      c_run:     db "run", 0
;      c_ps:      db "ps", 0
;      c_kill:    db "kill", 0
;      mt_s_id:   db "ID  ", 0
;      mt_s_name: db "Name            ", 0
;      mt_s_state: db "State", 0
;      mt_s_running: db "running", 0
;      mt_s_ready:   db "ready", 0
;      mt_s_blocked: db "blocked", 0
;      mt_s_dead:    db "dead", 0
;      mt_s_shell:   db "shell", 0
;      mt_s_noslot:  db "No free task slot", 0
;      mt_s_killed:  db "Task killed", 0
;      mt_s_badid:   db "Invalid task ID", 0
;      mt_s_fg:      db "[fg: task ", 0
;      mt_s_rbracket: db "]", 0
;
; 3. Shell commands — add to command dispatch:
;      mov edi, c_run
;      call strcmp_pfx
;      je .cmd_run
;
;      mov edi, c_ps
;      call strcmp
;      je .cmd_ps
;
;      mov edi, c_kill
;      call strcmp_pfx
;      je .cmd_kill
;
;    And add handlers:
;      .cmd_run:
;          cmp byte [arg1], 0
;          je cmd_loop
;          call mt_run_script
;          jmp cmd_done
;
;      .cmd_ps:
;          call mt_show_ps
;          jmp cmd_done
;
;      .cmd_kill:
;          cmp byte [arg1], 0
;          je cmd_loop
;          call mt_kill_task
;          jmp cmd_done
;
; 4. Keyboard handler — add Ctrl+Tab check:
;      In the keyboard ISR or key processing code, add:
;        cmp al, SC_TAB
;        jne .not_ctrl_tab
;        cmp byte [ctrl_held], 1
;        jne .not_ctrl_tab
;        call mt_cycle_fg
;        jmp .key_done
;      .not_ctrl_tab:
;
; 5. Initialization — add to _start, after fs_init:
;      call mt_init
;
; 6. Help text — add:
;      h18: db " run <f> & - background task", 0
;      h19: db " ps      - list tasks", 0
;      h20: db " kill <n> - kill task", 0
;
; 7. Include directive — add near end of file:
;      %include "multitask.asm"
;
; ═══════════════════════════════════════════════════════════════════

; Task states
MT_STATE_DEAD    equ 0
MT_STATE_READY   equ 1
MT_STATE_RUNNING equ 2
MT_STATE_BLOCKED equ 3

; Task table layout: id(1B) + state(1B) + name(16B) + stack_ptr(4B)
;                    + stack_base(4B) + entry_point(4B) = 30 bytes
MT_TASK_SZ       equ 30
MT_T_ID          equ 0
MT_T_STATE       equ 1
MT_T_NAME        equ 2
MT_T_SP          equ 18
MT_T_SBASE       equ 22
MT_T_ENTRY       equ 26

MT_MAX_TASKS     equ 4
MT_STACK_SZ      equ 4096       ; 4KB per task (1 page)

; Output buffer per task
MT_OUTBUF_SZ     equ 1024       ; 1KB ring buffer per task

; ═══════════════════════════════════════════════════════════════════
; mt_init — Initialize multitasking, register shell as task 0
; ═══════════════════════════════════════════════════════════════════
mt_init:
    pushad
    ; Clear task table
    mov edi, mt_tasks
    mov ecx, MT_MAX_TASKS * MT_TASK_SZ
    xor al, al
    rep stosb

    ; Clear output buffers
    mov edi, mt_outbufs
    mov ecx, MT_MAX_TASKS * MT_OUTBUF_SZ
    xor al, al
    rep stosb

    ; Clear output lengths
    mov edi, mt_outlen
    mov ecx, MT_MAX_TASKS
    xor eax, eax
.mt_clr_len:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .mt_clr_len

    ; Register task 0 = shell
    mov edi, mt_tasks
    mov byte [edi + MT_T_ID], 0
    mov byte [edi + MT_T_STATE], MT_STATE_RUNNING
    ; Copy name "shell"
    mov esi, mt_s_shell
    lea edi, [mt_tasks + MT_T_NAME]
    mov ecx, 5
    rep movsb
    mov byte [edi], 0
    ; Shell uses the main stack — no separate stack_base/stack_ptr
    mov dword [mt_tasks + MT_T_SP], 0
    mov dword [mt_tasks + MT_T_SBASE], 0
    mov dword [mt_tasks + MT_T_ENTRY], 0

    mov dword [mt_current], 0
    mov dword [mt_fg_task], 0
    mov dword [mt_task_count], 1

    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_run_script — Start a .flux script as a background task
; arg1 = filename (stripped of trailing '&' if present)
; ═══════════════════════════════════════════════════════════════════
mt_run_script:
    pushad

    ; Strip trailing ' &' or '&' from arg1 if present
    mov esi, arg1
    xor ecx, ecx
.mrs_len:
    cmp byte [esi + ecx], 0
    je .mrs_len_done
    inc ecx
    jmp .mrs_len
.mrs_len_done:
    ; Check for trailing '&'
    test ecx, ecx
    jz .mrs_nostrip
    dec ecx
    cmp byte [esi + ecx], '&'
    jne .mrs_check_space
    mov byte [esi + ecx], 0     ; strip '&'
    jmp .mrs_stripped
.mrs_check_space:
    inc ecx                     ; undo dec
    cmp ecx, 2
    jb .mrs_nostrip
    sub ecx, 2
    cmp byte [esi + ecx], ' '
    jne .mrs_nostrip
    cmp byte [esi + ecx + 1], '&'
    jne .mrs_nostrip
    mov byte [esi + ecx], 0     ; strip ' &'
.mrs_stripped:
.mrs_nostrip:

    ; Verify file exists in FS
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .mrs_nf

    ; Check it's a file
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_FILE
    jne .mrs_nf

    ; Find a free task slot
    xor ecx, ecx
    inc ecx                     ; start from 1 (0 = shell)
.mrs_find_slot:
    cmp ecx, MT_MAX_TASKS
    jge .mrs_noslot
    imul edi, ecx, MT_TASK_SZ
    add edi, mt_tasks
    cmp byte [edi + MT_T_STATE], MT_STATE_DEAD
    je .mrs_got_slot
    inc ecx
    jmp .mrs_find_slot

.mrs_got_slot:
    ; ecx = slot index, edi = task entry pointer
    push ecx
    push edi

    ; Set up task entry
    mov [edi + MT_T_ID], cl
    mov byte [edi + MT_T_STATE], MT_STATE_READY

    ; Copy filename as task name (up to 15 chars)
    push edi
    lea edi, [edi + MT_T_NAME]
    mov esi, arg1
    mov ecx, 15
.mrs_cpname:
    lodsb
    stosb
    test al, al
    jz .mrs_cpname_done
    dec ecx
    jnz .mrs_cpname
    mov byte [edi], 0
.mrs_cpname_done:
    pop edi

    ; Allocate a 4KB stack from heap
    ; (In a real OS we'd use the page allocator, but heap is simpler)
    mov eax, [heap_ptr]
    add eax, 15
    and eax, 0xFFFFFFF0         ; 16-byte align
    mov [edi + MT_T_SBASE], eax
    add eax, MT_STACK_SZ
    mov [edi + MT_T_SP], eax    ; stack grows down, so SP = top
    mov [heap_ptr], eax         ; advance heap

    ; Store the filesystem entry index as the "entry point"
    ; (We'll re-find the file when the task starts)
    mov eax, [esp + 4]          ; slot index
    ; Actually store the file's FS index found earlier
    ; We need to recover it — it was in eax before the slot search
    ; Let's re-find it
    push edi
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    pop edi
    mov [edi + MT_T_ENTRY], eax

    inc dword [mt_task_count]

    pop edi
    pop ecx

    ; Print confirmation
    mov esi, mt_s_started
    mov bl, COL_OK
    call lp_print
    mov esi, arg1
    mov bl, COL_OK
    call lp_print
    mov esi, mt_s_astask
    mov bl, COL_OK
    call lp_print
    ; Print task ID
    movzx eax, cl
    call itoa
    mov esi, numbuf
    mov bl, COL_OK
    call lp_println

    popad
    ret

.mrs_nf:
    mov esi, fi_e_fnf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    mov bl, COL_ERR
    call lp_println
    popad
    ret

.mrs_noslot:
    mov esi, mt_s_noslot
    mov bl, COL_ERR
    call lp_println
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_yield — Cooperative yield: save current context, switch to next
; Called by a running task to give up the CPU
; ═══════════════════════════════════════════════════════════════════
mt_yield:
    ; Save all registers of current task
    pushfd
    pushad

    ; Get current task
    mov eax, [mt_current]
    imul edi, eax, MT_TASK_SZ
    add edi, mt_tasks

    ; Save stack pointer
    mov [edi + MT_T_SP], esp

    ; Mark as READY (not RUNNING)
    cmp byte [edi + MT_T_STATE], MT_STATE_RUNNING
    jne .yield_find_next
    mov byte [edi + MT_T_STATE], MT_STATE_READY

.yield_find_next:
    ; Find next READY task (round-robin)
    mov ecx, [mt_current]
.yield_scan:
    inc ecx
    cmp ecx, MT_MAX_TASKS
    jl .yield_check
    xor ecx, ecx               ; wrap around
.yield_check:
    cmp ecx, [mt_current]
    je .yield_same              ; went full circle, no other ready task
    imul edi, ecx, MT_TASK_SZ
    add edi, mt_tasks
    cmp byte [edi + MT_T_STATE], MT_STATE_READY
    je .yield_switch
    jmp .yield_scan

.yield_same:
    ; No other task ready — stay with current
    mov eax, [mt_current]
    imul edi, eax, MT_TASK_SZ
    add edi, mt_tasks
    mov byte [edi + MT_T_STATE], MT_STATE_RUNNING
    popad
    popfd
    ret

.yield_switch:
    ; Switch to task ecx
    mov [mt_current], ecx
    mov byte [edi + MT_T_STATE], MT_STATE_RUNNING

    ; If this task has never run before (SP == stack top, i.e., nothing pushed)
    ; we need to set it up for first execution
    mov eax, [edi + MT_T_SBASE]
    add eax, MT_STACK_SZ
    cmp [edi + MT_T_SP], eax
    je .yield_first_run

    ; Restore stack pointer
    mov esp, [edi + MT_T_SP]
    popad
    popfd
    ret

.yield_first_run:
    ; First time running this task — set up its stack and call the entry
    mov esp, [edi + MT_T_SP]

    ; Push a return address that will clean up the task when it returns
    push dword mt_task_exit

    ; The entry point is an FS index — run the file
    push dword [edi + MT_T_ENTRY]

    ; Set up the file to run via the interpreter
    ; We need to call flux_run_file with the filename
    ; Copy task name to arg1
    push edi
    lea esi, [edi + MT_T_NAME]
    mov edi, arg1
    mov ecx, 16
.yf_cp:
    lodsb
    stosb
    test al, al
    jz .yf_cp_done
    dec ecx
    jnz .yf_cp
.yf_cp_done:
    pop edi

    ; Remove the FS index we pushed (not needed)
    add esp, 4

    ; Call the interpreter
    call flux_run_file

    ; Task finished — fall through to exit
    jmp mt_task_exit

; ═══════════════════════════════════════════════════════════════════
; mt_task_exit — Called when a task's entry function returns
; ═══════════════════════════════════════════════════════════════════
mt_task_exit:
    ; Mark current task as dead
    mov eax, [mt_current]
    imul edi, eax, MT_TASK_SZ
    add edi, mt_tasks
    mov byte [edi + MT_T_STATE], MT_STATE_DEAD
    dec dword [mt_task_count]

    ; If this was the foreground task, switch fg to shell
    cmp eax, [mt_fg_task]
    jne .mte_nofg
    mov dword [mt_fg_task], 0
.mte_nofg:
    ; Yield to find another task
    call mt_yield
    ; If we get here, we're the only task (shouldn't happen for non-shell)
    ; Halt as safety measure
    cli
    hlt

; ═══════════════════════════════════════════════════════════════════
; mt_show_ps — Display task list (ps command)
; ═══════════════════════════════════════════════════════════════════
mt_show_ps:
    pushad

    ; Header
    mov esi, mt_s_id
    mov bl, COL_HIST_LBL
    call lp_print
    mov esi, mt_s_name
    mov bl, COL_HIST_LBL
    call lp_print
    mov esi, mt_s_state
    mov bl, COL_HIST_LBL
    call lp_println

    ; Iterate tasks
    xor ecx, ecx
.ps_loop:
    cmp ecx, MT_MAX_TASKS
    jge .ps_done
    imul edi, ecx, MT_TASK_SZ
    add edi, mt_tasks

    ; Skip dead tasks (except slot 0)
    cmp byte [edi + MT_T_STATE], MT_STATE_DEAD
    je .ps_next

    ; Print ID
    push ecx
    movzx eax, byte [edi + MT_T_ID]
    call itoa
    mov esi, numbuf
    mov bl, COL_OUTPUT
    call lp_print
    ; Padding
    mov esi, mt_s_pad
    call lp_print

    ; Print name
    push edi
    lea esi, [edi + MT_T_NAME]
    mov bl, COL_WHITE
    call lp_print
    pop edi

    ; Padding to align state column
    mov esi, mt_s_pad
    call lp_print

    ; Print state
    movzx eax, byte [edi + MT_T_STATE]
    cmp al, MT_STATE_RUNNING
    je .ps_running
    cmp al, MT_STATE_READY
    je .ps_ready
    cmp al, MT_STATE_BLOCKED
    je .ps_blocked
    mov esi, mt_s_dead
    jmp .ps_print_state
.ps_running:
    mov esi, mt_s_running
    jmp .ps_print_state
.ps_ready:
    mov esi, mt_s_ready
    jmp .ps_print_state
.ps_blocked:
    mov esi, mt_s_blocked
.ps_print_state:
    mov bl, COL_VER
    call lp_print

    ; Show [fg] marker for foreground task
    pop ecx
    cmp ecx, [mt_fg_task]
    jne .ps_nofg
    mov esi, mt_s_fgmark
    mov bl, COL_PROMPT
    call lp_print
.ps_nofg:
    call lp_newline

.ps_next:
    inc ecx
    jmp .ps_loop

.ps_done:
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_kill_task — Kill a task by ID (from arg1)
; ═══════════════════════════════════════════════════════════════════
mt_kill_task:
    pushad

    ; Parse task ID from arg1
    mov esi, arg1
    movzx eax, byte [esi]
    sub eax, '0'
    cmp eax, 0
    jl .mk_bad
    cmp eax, MT_MAX_TASKS
    jge .mk_bad

    ; Can't kill task 0 (shell)
    test eax, eax
    jz .mk_bad

    ; Find task
    imul edi, eax, MT_TASK_SZ
    add edi, mt_tasks
    cmp byte [edi + MT_T_STATE], MT_STATE_DEAD
    je .mk_bad

    ; Kill it
    mov byte [edi + MT_T_STATE], MT_STATE_DEAD
    dec dword [mt_task_count]

    ; If it was foreground, reset to shell
    cmp eax, [mt_fg_task]
    jne .mk_nofg
    mov dword [mt_fg_task], 0
.mk_nofg:

    mov esi, mt_s_killed
    mov bl, COL_OK
    call lp_println
    popad
    ret

.mk_bad:
    mov esi, mt_s_badid
    mov bl, COL_ERR
    call lp_println
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_cycle_fg — Cycle foreground task (Ctrl+Tab)
; ═══════════════════════════════════════════════════════════════════
mt_cycle_fg:
    pushad

    mov ecx, [mt_fg_task]
.cf_scan:
    inc ecx
    cmp ecx, MT_MAX_TASKS
    jl .cf_check
    xor ecx, ecx               ; wrap
.cf_check:
    ; Accept if task is alive
    imul edi, ecx, MT_TASK_SZ
    add edi, mt_tasks
    cmp byte [edi + MT_T_STATE], MT_STATE_DEAD
    jne .cf_found
    cmp ecx, [mt_fg_task]
    je .cf_done                 ; went full circle
    jmp .cf_scan

.cf_found:
    mov [mt_fg_task], ecx

    ; Print indicator
    mov esi, mt_s_fg
    mov bl, COL_BLUE
    call lp_print
    movzx eax, cl
    call itoa
    mov esi, numbuf
    call lp_print
    mov esi, mt_s_rbracket
    call lp_println

.cf_done:
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_task_print — Print from a task context
; If current task == foreground, print to VGA directly
; Otherwise, buffer the output for later display
; esi = string, bl = color
; ═══════════════════════════════════════════════════════════════════
mt_task_print:
    pushad
    mov eax, [mt_current]
    cmp eax, [mt_fg_task]
    je .tp_direct

    ; Buffer the output
    imul edi, eax, MT_OUTBUF_SZ
    add edi, mt_outbufs
    mov ecx, [mt_outlen + eax * 4]

.tp_buf_loop:
    cmp ecx, MT_OUTBUF_SZ - 1
    jge .tp_buf_full            ; buffer full, drop
    lodsb
    test al, al
    jz .tp_buf_done
    mov [edi + ecx], al
    inc ecx
    jmp .tp_buf_loop

.tp_buf_done:
    mov byte [edi + ecx], 0
    mov eax, [mt_current]       ; reload (clobbered by pushad frame math isn't needed)
    ; Actually we still have it from before the loop
    mov eax, [esp + 28]         ; reload from pushad if needed... simpler:
    mov eax, [mt_current]
    mov [mt_outlen + eax * 4], ecx
    popad
    ret

.tp_buf_full:
    popad
    ret

.tp_direct:
    ; Print directly via lp_print
    ; esi and bl are from caller — but we pushad'd, so restore
    popad
    call lp_print
    ret

; mt_task_println — like mt_task_print but adds newline
mt_task_println:
    call mt_task_print
    pushad
    mov eax, [mt_current]
    cmp eax, [mt_fg_task]
    jne .tpl_buf_nl
    call lp_newline
    popad
    ret
.tpl_buf_nl:
    ; Add newline char to buffer
    imul edi, eax, MT_OUTBUF_SZ
    add edi, mt_outbufs
    mov ecx, [mt_outlen + eax * 4]
    cmp ecx, MT_OUTBUF_SZ - 1
    jge .tpl_full
    mov byte [edi + ecx], 10
    inc ecx
    mov [mt_outlen + eax * 4], ecx
.tpl_full:
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_flush_fg — Flush foreground task's output buffer to VGA
; Called periodically or on Ctrl+Tab to display buffered output
; ═══════════════════════════════════════════════════════════════════
mt_flush_fg:
    pushad
    mov eax, [mt_fg_task]
    mov ecx, [mt_outlen + eax * 4]
    test ecx, ecx
    jz .ff_done

    ; Print buffered content line by line
    imul esi, eax, MT_OUTBUF_SZ
    add esi, mt_outbufs
    mov bl, COL_OUTPUT

.ff_loop:
    test ecx, ecx
    jz .ff_clear
    lodsb
    dec ecx
    cmp al, 10                  ; newline
    je .ff_nl
    cmp al, 0
    je .ff_clear
    mov ah, bl
    call lp_putc
    jmp .ff_loop

.ff_nl:
    call lp_newline
    jmp .ff_loop

.ff_clear:
    ; Clear the buffer
    mov eax, [mt_fg_task]
    mov dword [mt_outlen + eax * 4], 0

.ff_done:
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_schedule_tick — Called from PIT timer handler to check tasks
; For cooperative multitasking, this just flushes output
; (True preemption would save/restore context here)
; ═══════════════════════════════════════════════════════════════════
mt_schedule_tick:
    pushad
    ; Flush foreground task output every tick
    call mt_flush_fg
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; mt_get_state_str — Get state string for a state value
; al = state, returns esi = string
; ═══════════════════════════════════════════════════════════════════
mt_get_state_str:
    cmp al, MT_STATE_RUNNING
    je .gs_running
    cmp al, MT_STATE_READY
    je .gs_ready
    cmp al, MT_STATE_BLOCKED
    je .gs_blocked
    mov esi, mt_s_dead
    ret
.gs_running:
    mov esi, mt_s_running
    ret
.gs_ready:
    mov esi, mt_s_ready
    ret
.gs_blocked:
    mov esi, mt_s_blocked
    ret

; ═══════════════════════════════════════════════════════════════════
; DATA STRINGS
; ═══════════════════════════════════════════════════════════════════
section .data

c_run:          db "run", 0
c_ps:           db "ps", 0
c_kill:         db "kill", 0

mt_s_id:        db "ID  ", 0
mt_s_name:      db "Name            ", 0
mt_s_state:     db "State", 0
mt_s_running:   db "running", 0
mt_s_ready:     db "ready", 0
mt_s_blocked:   db "blocked", 0
mt_s_dead:      db "dead", 0
mt_s_shell:     db "shell", 0
mt_s_noslot:    db "No free task slot", 0
mt_s_killed:    db "Task killed", 0
mt_s_badid:     db "Invalid task ID", 0
mt_s_fg:        db "[fg: task ", 0
mt_s_rbracket:  db "]", 0
mt_s_pad:       db "   ", 0
mt_s_fgmark:    db " [fg]", 0
mt_s_started:   db "Started: ", 0
mt_s_astask:    db " as task ", 0

; ═══════════════════════════════════════════════════════════════════
; BSS — Multitasking state
; ═══════════════════════════════════════════════════════════════════
section .bss

mt_tasks:       resb MT_MAX_TASKS * MT_TASK_SZ
mt_current:     resd 1
mt_fg_task:     resd 1
mt_task_count:  resd 1
mt_outbufs:     resb MT_MAX_TASKS * MT_OUTBUF_SZ
mt_outlen:      resd MT_MAX_TASKS
