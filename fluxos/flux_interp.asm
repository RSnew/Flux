; ═══════════════════════════════════════════════════════════════════
; flux_interp.asm — Built-in Flux Language Interpreter for FluxOS
; ═══════════════════════════════════════════════════════════════════
;
; A minimal tree-walking interpreter for .flux scripts stored in
; the FluxOS RAM filesystem.  Supports:
;   var x = 42 / var s = "hello"
;   print(expr)
;   Arithmetic: + - * / %
;   Comparison: == != < > <= >=
;   if cond { ... } else { ... }
;   while cond { ... }
;   for i in range(n) { ... }
;   func name(params) { ... return value }
;   String concatenation with +
;
; ═══════════════════════════════════════════════════════════════════
; INTEGRATION INSTRUCTIONS
; ═══════════════════════════════════════════════════════════════════
; Add to fluxos_flat.asm:
;
; 1. BSS variables — add after existing BSS block:
;      fi_src:        resd 1          ; pointer to source code
;      fi_srclen:     resd 1          ; source length
;      fi_pos:        resd 1          ; tokenizer position
;      fi_tokbuf:     resb 2048       ; token buffer (type[1]+val_off[4]+len[2] = 7 each)
;      fi_tokcnt:     resd 1          ; number of tokens
;      fi_tokpos:     resd 1          ; current token index for parser
;      fi_vars:       resb 64*21      ; 64 vars: name(16B)+value(4B)+type(1B)
;      fi_varcnt:     resd 1          ; number of variables
;      fi_stpool:     resb 4096       ; string pool
;      fi_stptr:      resd 1          ; next free byte in string pool
;      fi_funcs:      resb 16*25      ; 16 funcs: name(16B)+body_tokidx(4B)+param_cnt(1B)+param_off(4B)
;      fi_fncnt:      resd 1          ; number of functions
;      fi_callstk:    resd 64         ; call stack (saved var counts, 8 levels * 8 dwords)
;      fi_calldepth:  resd 1          ; current call depth
;      fi_retval:     resd 1          ; return value from function
;      fi_retflag:    resd 1          ; 1 if return was hit
;      fi_tmpbuf:     resb 256        ; temporary buffer for string ops
;      fi_errbuf:     resb 64         ; error message buffer
;
; 2. Data strings — add to data section:
;      c_flux:   db "flux", 0
;      fi_e_syn: db "Syntax error", 0
;      fi_e_var: db "Undefined var: ", 0
;      fi_e_ovf: db "Stack overflow", 0
;      fi_e_fnf: db "File not found: ", 0
;      fi_e_div: db "Division by zero", 0
;      fi_s_true:  db "true", 0
;      fi_s_false: db "false", 0
;
; 3. Shell command — add to command dispatch (after other strcmp_pfx calls):
;      mov edi, c_flux
;      call strcmp_pfx
;      je .cmd_flux
;
;    And add handler:
;      .cmd_flux:
;          cmp byte [arg1], 0
;          je cmd_loop
;          call flux_run_file
;          jmp cmd_done
;
; 4. Help text — add:
;      h17: db " flux <f> - run script", 0
;
; 5. Include directive — add near end of file, before final data:
;      %include "flux_interp.asm"
;
; ═══════════════════════════════════════════════════════════════════

; Token type constants
TK_NUM     equ 0
TK_STR     equ 1
TK_ID      equ 2
TK_VAR     equ 3       ; keyword 'var'
TK_IF      equ 4
TK_ELSE    equ 5
TK_WHILE   equ 6
TK_FOR     equ 7
TK_IN      equ 8
TK_FUNC    equ 9
TK_RETURN  equ 10
TK_PLUS    equ 11
TK_MINUS   equ 12
TK_STAR    equ 13
TK_SLASH   equ 14
TK_PERCENT equ 15
TK_EQ      equ 16      ; ==
TK_NEQ     equ 17      ; !=
TK_LT      equ 18
TK_GT      equ 19
TK_LEQ     equ 20      ; <=
TK_GEQ     equ 21      ; >=
TK_ASSIGN  equ 22      ; =
TK_LPAREN  equ 23
TK_RPAREN  equ 24
TK_LBRACE  equ 25
TK_RBRACE  equ 26
TK_COMMA   equ 27
TK_NL      equ 28      ; newline
TK_EOF     equ 29
TK_RANGE   equ 30      ; keyword 'range'
TK_PRINT   equ 31      ; keyword 'print'
TK_TRUE    equ 32
TK_FALSE   equ 33

; Token entry size: type(1B) + value_offset(4B) + length(2B) = 7 bytes
TK_ENTRY_SZ equ 7

; Variable entry: name(16B) + value(4B) + type(1B) = 21 bytes
; type: 0=int, 1=string_ptr
VAR_ENTRY_SZ equ 21
VAR_NAME     equ 0
VAR_VALUE    equ 16
VAR_TYPE     equ 20
VTYPE_INT    equ 0
VTYPE_STR    equ 1

; Function entry: name(16B) + body_tok_idx(4B) + param_count(1B) + params_str_off(4B) = 25B
FUNC_ENTRY_SZ equ 25
FUNC_NAME     equ 0
FUNC_TOKIDX   equ 16
FUNC_PCNT     equ 20
FUNC_POFF     equ 21

FI_MAX_VARS  equ 64
FI_MAX_FUNCS equ 16
FI_MAX_CALL  equ 8
FI_MAX_TOKENS equ 290   ; 2048/7 ~= 292

; ═══════════════════════════════════════════════════════════════════
; flux_run_file — Main entry point
; Looks up arg1 in filesystem, tokenizes, parses+executes
; ═══════════════════════════════════════════════════════════════════
flux_run_file:
    pushad

    ; Find file in FS
    mov esi, arg1
    movzx edx, byte [fs_cwd]
    call fs_find
    cmp eax, -1
    je .frf_notfound

    ; Check it's a file
    imul ebx, eax, FS_ENTRY_SZ
    cmp byte [fs_tab + ebx + FS_TYPE], FS_FILE
    jne .frf_notfound

    ; Get file content pointer
    mov esi, [fs_tab + ebx + FS_DPTR]
    test esi, esi
    jz .frf_empty
    mov eax, [fs_tab + ebx + FS_SIZE]
    test eax, eax
    jz .frf_empty

    ; Store source info
    mov [fi_src], esi
    mov [fi_srclen], eax

    ; Initialize interpreter state
    mov dword [fi_pos], 0
    mov dword [fi_tokcnt], 0
    mov dword [fi_tokpos], 0
    mov dword [fi_varcnt], 0
    mov dword [fi_fncnt], 0
    mov dword [fi_calldepth], 0
    mov dword [fi_retflag], 0
    mov dword [fi_retval], 0

    ; Initialize string pool pointer
    mov dword [fi_stptr], fi_stpool

    ; Tokenize
    call fi_tokenize
    test eax, eax
    jnz .frf_err

    ; Execute all statements
    call fi_exec_program

    popad
    ret

.frf_notfound:
    mov esi, fi_e_fnf
    mov bl, COL_ERR
    call lp_print
    mov esi, arg1
    mov bl, COL_ERR
    call lp_println
    popad
    ret

.frf_empty:
    popad
    ret

.frf_err:
    mov esi, fi_e_syn
    mov bl, COL_ERR
    call lp_println
    popad
    ret

; ═══════════════════════════════════════════════════════════════════
; TOKENIZER
; ═══════════════════════════════════════════════════════════════════

; fi_tokenize — tokenize source at [fi_src], length [fi_srclen]
; Returns eax=0 on success, 1 on error
fi_tokenize:
    pushad
    mov dword [fi_pos], 0
    mov dword [fi_tokcnt], 0

.tk_loop:
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .tk_eof

    mov esi, [fi_src]
    add esi, eax
    movzx ecx, byte [esi]      ; current char

    ; Skip spaces and tabs
    cmp cl, ' '
    je .tk_skip
    cmp cl, 9                   ; tab
    je .tk_skip
    cmp cl, 13                  ; CR
    je .tk_skip

    ; Newline
    cmp cl, 10                  ; LF
    je .tk_newline

    ; Comment: // skip to end of line
    cmp cl, '/'
    jne .tk_not_comment
    mov eax, [fi_pos]
    inc eax
    cmp eax, [fi_srclen]
    jge .tk_not_comment
    mov esi, [fi_src]
    add esi, eax
    cmp byte [esi], '/'
    jne .tk_not_comment
    ; Skip to end of line
.tk_skip_comment:
    inc dword [fi_pos]
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .tk_eof
    mov esi, [fi_src]
    add esi, eax
    cmp byte [esi], 10
    jne .tk_skip_comment
    jmp .tk_loop

.tk_not_comment:
    ; Number
    cmp cl, '0'
    jb .tk_not_num
    cmp cl, '9'
    ja .tk_not_num
    call fi_tok_number
    jmp .tk_loop

.tk_not_num:
    ; String literal
    cmp cl, '"'
    jne .tk_not_str
    call fi_tok_string
    jmp .tk_loop

.tk_not_str:
    ; Identifier or keyword
    call fi_is_alpha
    test eax, eax
    jz .tk_not_id
    call fi_tok_ident
    jmp .tk_loop

.tk_not_id:
    ; Two-char operators: == != <= >=
    call fi_tok_operator
    jmp .tk_loop

.tk_skip:
    inc dword [fi_pos]
    jmp .tk_loop

.tk_newline:
    ; Add newline token
    mov al, TK_NL
    xor ebx, ebx               ; no value
    xor ecx, ecx
    call fi_add_token
    inc dword [fi_pos]
    jmp .tk_loop

.tk_eof:
    mov al, TK_EOF
    xor ebx, ebx
    xor ecx, ecx
    call fi_add_token
    popad
    xor eax, eax
    ret

; fi_add_token — add token: al=type, ebx=value_offset, cx=length
fi_add_token:
    push edx
    push edi
    mov edx, [fi_tokcnt]
    cmp edx, FI_MAX_TOKENS
    jge .at_full
    imul edi, edx, TK_ENTRY_SZ
    add edi, fi_tokbuf
    mov [edi], al               ; type
    mov [edi + 1], ebx          ; value offset (ptr into source or stpool)
    mov [edi + 5], cx           ; length
    inc dword [fi_tokcnt]
.at_full:
    pop edi
    pop edx
    ret

; fi_tok_number — parse a decimal integer from source
fi_tok_number:
    push ebx
    push ecx
    mov ebx, [fi_pos]          ; start offset (value_offset = start in source)
    mov ecx, [fi_src]
    add ecx, ebx               ; ecx = pointer to start
.tn_loop:
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .tn_done
    mov esi, [fi_src]
    add esi, eax
    movzx edx, byte [esi]
    cmp dl, '0'
    jb .tn_done
    cmp dl, '9'
    ja .tn_done
    inc dword [fi_pos]
    jmp .tn_loop
.tn_done:
    mov eax, [fi_pos]
    sub eax, ebx               ; length
    mov cx, ax
    mov al, TK_NUM
    ; ebx = offset into source
    add ebx, [fi_src]          ; make it absolute pointer
    call fi_add_token
    pop ecx
    pop ebx
    ret

; fi_tok_string — parse "..." from source, store in string pool
fi_tok_string:
    push ebx
    push ecx
    push edi
    inc dword [fi_pos]          ; skip opening "
    mov edi, [fi_stptr]         ; destination in string pool
    mov ebx, edi                ; remember start
.ts_loop:
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .ts_done
    mov esi, [fi_src]
    add esi, eax
    movzx edx, byte [esi]
    cmp dl, '"'
    je .ts_end
    cmp dl, '\'
    jne .ts_normal
    ; Escape sequence
    inc dword [fi_pos]
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .ts_done
    mov esi, [fi_src]
    add esi, eax
    movzx edx, byte [esi]
    cmp dl, 'n'
    jne .ts_esc2
    mov byte [edi], 10          ; \n
    jmp .ts_esc_done
.ts_esc2:
    cmp dl, 't'
    jne .ts_esc3
    mov byte [edi], 9           ; \t
    jmp .ts_esc_done
.ts_esc3:
    mov [edi], dl               ; literal escaped char
.ts_esc_done:
    inc edi
    inc dword [fi_pos]
    jmp .ts_loop
.ts_normal:
    mov [edi], dl
    inc edi
    inc dword [fi_pos]
    jmp .ts_loop
.ts_end:
    inc dword [fi_pos]          ; skip closing "
.ts_done:
    mov byte [edi], 0           ; null terminate
    inc edi
    mov [fi_stptr], edi         ; update pool pointer
    ; Add token: type=STR, value=ptr to pool string, length
    mov eax, edi
    sub eax, ebx
    dec eax                     ; don't count null
    mov cx, ax
    mov al, TK_STR
    ; ebx is already the pointer
    call fi_add_token
    pop edi
    pop ecx
    pop ebx
    ret

; fi_tok_ident — parse identifier/keyword
fi_tok_ident:
    push ebx
    push ecx
    push edx
    mov ebx, [fi_pos]
    mov edx, [fi_src]
    add edx, ebx               ; start pointer
.ti_loop:
    mov eax, [fi_pos]
    cmp eax, [fi_srclen]
    jge .ti_done
    mov esi, [fi_src]
    add esi, eax
    movzx ecx, byte [esi]
    call fi_is_alnum
    test eax, eax
    jz .ti_done
    inc dword [fi_pos]
    jmp .ti_loop
.ti_done:
    mov eax, [fi_pos]
    sub eax, ebx               ; length
    mov cx, ax
    ; Check keywords
    push ecx
    ; Copy word to fi_tmpbuf for comparison
    mov esi, edx                ; source pointer
    mov edi, fi_tmpbuf
    movzx ecx, cx
    push ecx
    rep movsb
    mov byte [edi], 0
    pop ecx

    ; Check each keyword
    mov esi, fi_tmpbuf
    mov edi, fi_kw_var
    call strcmp
    je .ti_kw_var
    mov esi, fi_tmpbuf
    mov edi, fi_kw_if
    call strcmp
    je .ti_kw_if
    mov esi, fi_tmpbuf
    mov edi, fi_kw_else
    call strcmp
    je .ti_kw_else
    mov esi, fi_tmpbuf
    mov edi, fi_kw_while
    call strcmp
    je .ti_kw_while
    mov esi, fi_tmpbuf
    mov edi, fi_kw_for
    call strcmp
    je .ti_kw_for
    mov esi, fi_tmpbuf
    mov edi, fi_kw_in
    call strcmp
    je .ti_kw_in
    mov esi, fi_tmpbuf
    mov edi, fi_kw_func
    call strcmp
    je .ti_kw_func
    mov esi, fi_tmpbuf
    mov edi, fi_kw_return
    call strcmp
    je .ti_kw_return
    mov esi, fi_tmpbuf
    mov edi, fi_kw_range
    call strcmp
    je .ti_kw_range
    mov esi, fi_tmpbuf
    mov edi, fi_kw_print
    call strcmp
    je .ti_kw_print
    mov esi, fi_tmpbuf
    mov edi, fi_kw_true
    call strcmp
    je .ti_kw_true
    mov esi, fi_tmpbuf
    mov edi, fi_kw_false
    call strcmp
    je .ti_kw_false

    ; Regular identifier
    pop ecx
    mov al, TK_ID
    mov ebx, edx               ; pointer to identifier in source
    call fi_add_token
    pop edx
    pop ecx
    pop ebx
    ret

.ti_kw_var:     pop ecx
                mov al, TK_VAR
                jmp .ti_kw_emit
.ti_kw_if:      pop ecx
                mov al, TK_IF
                jmp .ti_kw_emit
.ti_kw_else:    pop ecx
                mov al, TK_ELSE
                jmp .ti_kw_emit
.ti_kw_while:   pop ecx
                mov al, TK_WHILE
                jmp .ti_kw_emit
.ti_kw_for:     pop ecx
                mov al, TK_FOR
                jmp .ti_kw_emit
.ti_kw_in:      pop ecx
                mov al, TK_IN
                jmp .ti_kw_emit
.ti_kw_func:    pop ecx
                mov al, TK_FUNC
                jmp .ti_kw_emit
.ti_kw_return:  pop ecx
                mov al, TK_RETURN
                jmp .ti_kw_emit
.ti_kw_range:   pop ecx
                mov al, TK_RANGE
                jmp .ti_kw_emit
.ti_kw_print:   pop ecx
                mov al, TK_PRINT
                jmp .ti_kw_emit
.ti_kw_true:    pop ecx
                mov al, TK_TRUE
                jmp .ti_kw_emit
.ti_kw_false:   pop ecx
                mov al, TK_FALSE
                jmp .ti_kw_emit

.ti_kw_emit:
    mov ebx, edx               ; pointer to keyword in source
    call fi_add_token
    pop edx
    pop ecx
    pop ebx
    ret

; fi_tok_operator — parse single/double-char operators
fi_tok_operator:
    push ebx
    push ecx
    mov eax, [fi_pos]
    mov esi, [fi_src]
    add esi, eax
    movzx ecx, byte [esi]

    ; Check two-char operators first
    mov edx, eax
    inc edx
    cmp edx, [fi_srclen]
    jge .to_single

    movzx edx, byte [esi + 1]
    cmp cl, '='
    jne .to_not_eq
    cmp dl, '='
    jne .to_assign
    mov al, TK_EQ
    add dword [fi_pos], 2
    jmp .to_emit
.to_assign:
    mov al, TK_ASSIGN
    inc dword [fi_pos]
    jmp .to_emit
.to_not_eq:
    cmp cl, '!'
    jne .to_not_neq
    cmp dl, '='
    jne .to_single
    mov al, TK_NEQ
    add dword [fi_pos], 2
    jmp .to_emit
.to_not_neq:
    cmp cl, '<'
    jne .to_not_lt
    cmp dl, '='
    jne .to_lt
    mov al, TK_LEQ
    add dword [fi_pos], 2
    jmp .to_emit
.to_lt:
    mov al, TK_LT
    inc dword [fi_pos]
    jmp .to_emit
.to_not_lt:
    cmp cl, '>'
    jne .to_not_gt
    cmp dl, '='
    jne .to_gt
    mov al, TK_GEQ
    add dword [fi_pos], 2
    jmp .to_emit
.to_gt:
    mov al, TK_GT
    inc dword [fi_pos]
    jmp .to_emit
.to_not_gt:

.to_single:
    cmp cl, '+'
    jne .to_s2
    mov al, TK_PLUS
    inc dword [fi_pos]
    jmp .to_emit
.to_s2:
    cmp cl, '-'
    jne .to_s3
    mov al, TK_MINUS
    inc dword [fi_pos]
    jmp .to_emit
.to_s3:
    cmp cl, '*'
    jne .to_s4
    mov al, TK_STAR
    inc dword [fi_pos]
    jmp .to_emit
.to_s4:
    cmp cl, '/'
    jne .to_s5
    mov al, TK_SLASH
    inc dword [fi_pos]
    jmp .to_emit
.to_s5:
    cmp cl, '%'
    jne .to_s6
    mov al, TK_PERCENT
    inc dword [fi_pos]
    jmp .to_emit
.to_s6:
    cmp cl, '('
    jne .to_s7
    mov al, TK_LPAREN
    inc dword [fi_pos]
    jmp .to_emit
.to_s7:
    cmp cl, ')'
    jne .to_s8
    mov al, TK_RPAREN
    inc dword [fi_pos]
    jmp .to_emit
.to_s8:
    cmp cl, '{'
    jne .to_s9
    mov al, TK_LBRACE
    inc dword [fi_pos]
    jmp .to_emit
.to_s9:
    cmp cl, '}'
    jne .to_s10
    mov al, TK_RBRACE
    inc dword [fi_pos]
    jmp .to_emit
.to_s10:
    cmp cl, ','
    jne .to_s11
    mov al, TK_COMMA
    inc dword [fi_pos]
    jmp .to_emit
.to_s11:
    ; Unknown char — skip
    inc dword [fi_pos]
    pop ecx
    pop ebx
    ret

.to_emit:
    xor ebx, ebx
    xor ecx, ecx
    call fi_add_token
    pop ecx
    pop ebx
    ret

; fi_is_alpha — check if cl is alpha or underscore; returns eax=1/0
fi_is_alpha:
    xor eax, eax
    cmp cl, 'a'
    jb .ia_upper
    cmp cl, 'z'
    ja .ia_upper
    inc eax
    ret
.ia_upper:
    cmp cl, 'A'
    jb .ia_under
    cmp cl, 'Z'
    ja .ia_under
    inc eax
    ret
.ia_under:
    cmp cl, '_'
    jne .ia_no
    inc eax
.ia_no:
    ret

; fi_is_alnum — check if cl is alphanumeric or underscore; returns eax=1/0
fi_is_alnum:
    call fi_is_alpha
    test eax, eax
    jnz .ian_yes
    cmp cl, '0'
    jb .ian_no
    cmp cl, '9'
    ja .ian_no
    mov eax, 1
    ret
.ian_yes:
    ret
.ian_no:
    xor eax, eax
    ret

; ═══════════════════════════════════════════════════════════════════
; TOKEN ACCESS HELPERS
; ═══════════════════════════════════════════════════════════════════

; fi_cur_tok — get current token type in al, value_ptr in ebx, len in cx
fi_cur_tok:
    push edx
    mov edx, [fi_tokpos]
    cmp edx, [fi_tokcnt]
    jge .ct_eof
    imul edx, TK_ENTRY_SZ
    add edx, fi_tokbuf
    movzx eax, byte [edx]      ; type
    mov ebx, [edx + 1]         ; value ptr
    movzx ecx, word [edx + 5]  ; length
    pop edx
    ret
.ct_eof:
    mov al, TK_EOF
    xor ebx, ebx
    xor ecx, ecx
    pop edx
    ret

; fi_advance — move to next token
fi_advance:
    inc dword [fi_tokpos]
    ret

; fi_skip_newlines — skip over any newline tokens
fi_skip_newlines:
    push eax
.snl:
    call fi_cur_tok
    cmp al, TK_NL
    jne .snl_done
    call fi_advance
    jmp .snl
.snl_done:
    pop eax
    ret

; fi_expect — expect current token type == al, advance; else error
; Input: al = expected type
; Returns: eax=0 ok, eax=1 error
fi_expect:
    push ebx
    push ecx
    mov bl, al                  ; save expected
    call fi_cur_tok
    cmp al, bl
    jne .exp_err
    call fi_advance
    xor eax, eax
    pop ecx
    pop ebx
    ret
.exp_err:
    mov eax, 1
    pop ecx
    pop ebx
    ret

; ═══════════════════════════════════════════════════════════════════
; PARSER / INTERPRETER — Recursive Descent
; ═══════════════════════════════════════════════════════════════════

; fi_exec_program — execute all statements until EOF
fi_exec_program:
    pushad
.ep_loop:
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_EOF
    je .ep_done
    cmp al, TK_RBRACE
    je .ep_done

    ; Check for return flag from nested call
    cmp dword [fi_retflag], 1
    je .ep_done

    call fi_exec_stmt
    jmp .ep_loop
.ep_done:
    popad
    ret

; fi_exec_stmt — execute one statement
fi_exec_stmt:
    call fi_skip_newlines
    call fi_cur_tok

    cmp al, TK_VAR
    je .es_var
    cmp al, TK_IF
    je .es_if
    cmp al, TK_WHILE
    je .es_while
    cmp al, TK_FOR
    je .es_for
    cmp al, TK_FUNC
    je .es_func
    cmp al, TK_RETURN
    je .es_return
    cmp al, TK_PRINT
    je .es_print
    cmp al, TK_ID
    je .es_id_stmt
    ; Skip unknown tokens
    cmp al, TK_NL
    je .es_skip
    cmp al, TK_EOF
    je .es_done
    call fi_advance             ; skip unknown
.es_done:
    ret
.es_skip:
    call fi_advance
    ret

; ── var declaration ──
.es_var:
    call fi_advance             ; skip 'var'
    call fi_cur_tok
    cmp al, TK_ID
    jne .es_done
    ; Save var name
    push ebx                    ; name ptr
    push ecx                    ; name len
    call fi_advance             ; skip name
    call fi_cur_tok
    cmp al, TK_ASSIGN
    jne .es_var_noval
    call fi_advance             ; skip '='
    call fi_eval_expr           ; result in eax, type in dl
    pop ecx                     ; name len
    pop ebx                     ; name ptr
    call fi_set_var
    ret
.es_var_noval:
    xor eax, eax               ; default value 0
    mov dl, VTYPE_INT
    pop ecx
    pop ebx
    call fi_set_var
    ret

; ── if statement ──
.es_if:
    jmp fi_exec_if

; ── while statement ──
.es_while:
    jmp fi_exec_while

; ── for statement ──
.es_for:
    jmp fi_exec_for

; ── func declaration ──
.es_func:
    jmp fi_exec_func_decl

; ── return statement ──
.es_return:
    call fi_advance             ; skip 'return'
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_NL
    je .es_ret_void
    cmp al, TK_EOF
    je .es_ret_void
    cmp al, TK_RBRACE
    je .es_ret_void
    call fi_eval_expr
    mov [fi_retval], eax
    mov dword [fi_retflag], 1
    ret
.es_ret_void:
    mov dword [fi_retval], 0
    mov dword [fi_retflag], 1
    ret

; ── print(expr) ──
.es_print:
    call fi_advance             ; skip 'print'
    call fi_cur_tok
    cmp al, TK_LPAREN
    jne .es_done
    call fi_advance             ; skip '('
    call fi_eval_expr           ; result in eax, type in dl
    push eax
    push edx
    call fi_cur_tok
    cmp al, TK_RPAREN
    jne .es_print_norp
    call fi_advance             ; skip ')'
.es_print_norp:
    pop edx
    pop eax
    ; Print based on type
    cmp dl, VTYPE_STR
    je .es_print_str
    ; Print integer
    call itoa
    mov esi, numbuf
    mov bl, COL_OUTPUT
    call lp_println
    ret
.es_print_str:
    mov esi, eax                ; eax is pointer to string
    mov bl, COL_OUTPUT
    call lp_println
    ret

; ── identifier statement (assignment or function call) ──
.es_id_stmt:
    ; Save identifier info
    push ebx
    push ecx
    call fi_advance             ; skip identifier
    call fi_cur_tok
    cmp al, TK_ASSIGN
    je .es_assign
    cmp al, TK_LPAREN
    je .es_call
    ; Unknown — skip
    pop ecx
    pop ebx
    ret

.es_assign:
    call fi_advance             ; skip '='
    ; Name is on stack
    call fi_eval_expr           ; value in eax, type in dl
    pop ecx                     ; name len
    pop ebx                     ; name ptr
    call fi_set_var
    ret

.es_call:
    pop ecx                     ; name len
    pop ebx                     ; name ptr
    ; ebx=name ptr, ecx=name len — call the function
    call fi_call_func
    ret

; ═══════════════════════════════════════════════════════════════════
; IF STATEMENT (proper implementation)
; ═══════════════════════════════════════════════════════════════════
; Patching .es_if to jump here
; We re-enter from the top since the original was buggy

fi_exec_if:
    ; Called after TK_IF is current (caller already checked)
    call fi_advance             ; skip 'if'
    call fi_eval_expr           ; condition in eax
    push eax                    ; save condition
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .eif_err
    pop eax                     ; restore condition
    test eax, eax
    jz .eif_skip_true

    ; Condition true — execute block
    call fi_advance             ; skip '{'
    call fi_exec_program        ; execute until '}'
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .eif_done
    call fi_advance             ; skip '}'
    ; Check for else
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_ELSE
    jne .eif_done
    call fi_advance             ; skip 'else'
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .eif_done
    ; Skip else block
    call fi_skip_block
    jmp .eif_done

.eif_skip_true:
    ; Condition false — skip true block
    call fi_skip_block
    ; Check for else
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_ELSE
    jne .eif_done
    call fi_advance             ; skip 'else'
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .eif_done
    call fi_advance             ; skip '{'
    call fi_exec_program
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .eif_done
    call fi_advance             ; skip '}'

.eif_done:
    ret
.eif_err:
    pop eax
    ret

; fi_skip_block — skip a { ... } block (counting braces)
fi_skip_block:
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .sb_ret
    call fi_advance             ; skip '{'
    mov ecx, 1                  ; brace depth
.sb_loop:
    call fi_cur_tok
    cmp al, TK_EOF
    je .sb_ret
    cmp al, TK_LBRACE
    jne .sb_not_open
    inc ecx
    jmp .sb_next
.sb_not_open:
    cmp al, TK_RBRACE
    jne .sb_next
    dec ecx
    jz .sb_end
.sb_next:
    call fi_advance
    jmp .sb_loop
.sb_end:
    call fi_advance             ; skip final '}'
.sb_ret:
    ret

; ═══════════════════════════════════════════════════════════════════
; WHILE STATEMENT
; ═══════════════════════════════════════════════════════════════════
fi_exec_while:
    call fi_advance             ; skip 'while'
    ; Remember token position for loop restart
    mov edx, [fi_tokpos]        ; save condition start
    push edx
.ew_iter:
    ; Restore to condition position
    mov eax, [esp]
    mov [fi_tokpos], eax
    call fi_eval_expr           ; evaluate condition
    test eax, eax
    jz .ew_exit

    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .ew_exit
    call fi_advance             ; skip '{'
    call fi_exec_program
    ; Check return
    cmp dword [fi_retflag], 1
    je .ew_exit
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .ew_exit
    call fi_advance             ; skip '}'
    jmp .ew_iter

.ew_exit:
    pop edx                     ; clean saved position
    ; Skip remaining block if we broke out of condition
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .ew_done
    call fi_skip_block
.ew_done:
    ret

; ═══════════════════════════════════════════════════════════════════
; FOR STATEMENT: for i in range(n) { ... }
; ═══════════════════════════════════════════════════════════════════
fi_exec_for:
    call fi_advance             ; skip 'for'
    call fi_cur_tok
    cmp al, TK_ID
    jne .ef_err
    push ebx                    ; save loop var name ptr
    push ecx                    ; save loop var name len
    call fi_advance             ; skip identifier
    call fi_cur_tok
    cmp al, TK_IN
    jne .ef_err2
    call fi_advance             ; skip 'in'
    call fi_cur_tok
    cmp al, TK_RANGE
    jne .ef_err2
    call fi_advance             ; skip 'range'
    call fi_cur_tok
    cmp al, TK_LPAREN
    jne .ef_err2
    call fi_advance             ; skip '('
    call fi_eval_expr           ; range limit in eax
    push eax                    ; save limit
    call fi_cur_tok
    cmp al, TK_RPAREN
    jne .ef_err3
    call fi_advance             ; skip ')'
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .ef_err3
    call fi_advance             ; skip '{'
    ; Save body start position
    mov edx, [fi_tokpos]
    push edx                    ; body start

    ; Initialize counter = 0
    xor eax, eax                ; i = 0
    mov dl, VTYPE_INT
    mov ebx, [esp + 12]        ; name ptr
    mov ecx, [esp + 8]         ; name len
    call fi_set_var

    ; Loop
    xor esi, esi                ; counter
.ef_iter:
    cmp esi, [esp + 4]         ; compare with limit
    jge .ef_done
    ; Set loop variable
    mov eax, esi
    mov dl, VTYPE_INT
    mov ebx, [esp + 12]        ; name ptr
    mov ecx, [esp + 8]         ; name len
    call fi_set_var
    ; Reset to body start
    mov eax, [esp]              ; body start
    mov [fi_tokpos], eax
    ; Execute body
    push esi
    call fi_exec_program
    pop esi
    ; Check return
    cmp dword [fi_retflag], 1
    je .ef_done
    ; Skip closing brace
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .ef_done
    inc esi
    jmp .ef_iter

.ef_done:
    pop edx                     ; body start
    pop eax                     ; limit
    pop ecx                     ; name len
    pop ebx                     ; name ptr
    ; Make sure we skip past the closing brace
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .ef_ret
    call fi_advance
.ef_ret:
    ret
.ef_err3:
    pop eax
.ef_err2:
    pop ecx
    pop ebx
.ef_err:
    ret

; ═══════════════════════════════════════════════════════════════════
; FUNC DECLARATION
; ═══════════════════════════════════════════════════════════════════
fi_exec_func_decl:
    call fi_advance             ; skip 'func'
    call fi_cur_tok
    cmp al, TK_ID
    jne .efd_err
    ; Save function name
    push ebx                    ; name ptr
    push ecx                    ; name len

    call fi_advance             ; skip name
    call fi_cur_tok
    cmp al, TK_LPAREN
    jne .efd_err2

    call fi_advance             ; skip '('
    ; Count and record parameter names in string pool
    mov edi, [fi_stptr]
    push edi                    ; params string start
    xor edx, edx               ; param count
.efd_params:
    call fi_cur_tok
    cmp al, TK_RPAREN
    je .efd_params_done
    cmp al, TK_ID
    jne .efd_params_done
    ; Copy param name to string pool
    push edx
    mov esi, ebx
    movzx ecx, cx
    mov edi, [fi_stptr]
    rep movsb
    mov byte [edi], 0
    inc edi
    mov [fi_stptr], edi
    pop edx
    inc edx
    call fi_advance             ; skip param name
    call fi_cur_tok
    cmp al, TK_COMMA
    jne .efd_params
    call fi_advance             ; skip ','
    jmp .efd_params

.efd_params_done:
    call fi_cur_tok
    cmp al, TK_RPAREN
    jne .efd_err3
    call fi_advance             ; skip ')'

    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .efd_err3

    ; Record body token position (pointing at '{')
    mov eax, [fi_tokpos]
    push eax                    ; body token idx
    push edx                    ; param count

    ; Register function
    mov eax, [fi_fncnt]
    cmp eax, FI_MAX_FUNCS
    jge .efd_err4
    imul edi, eax, FUNC_ENTRY_SZ
    add edi, fi_funcs
    ; Copy name
    mov esi, [esp + 16]         ; name ptr
    movzx ecx, word [esp + 12] ; name len
    push edi
    cmp ecx, 15
    jle .efd_cpn
    mov ecx, 15
.efd_cpn:
    rep movsb
    mov byte [edi], 0
    pop edi
    ; Set fields
    mov eax, [esp + 4]          ; body tok idx
    mov [edi + FUNC_TOKIDX], eax
    mov eax, [esp]              ; param count
    mov [edi + FUNC_PCNT], al
    mov eax, [esp + 8]          ; params string offset
    mov [edi + FUNC_POFF], eax
    inc dword [fi_fncnt]

    pop edx                     ; param count
    pop eax                     ; body tok idx
    pop edi                     ; params start
    pop ecx                     ; name len
    pop ebx                     ; name ptr

    ; Skip the function body
    call fi_skip_block
    ret

.efd_err4:
    pop edx
    pop eax
.efd_err3:
    pop edi
.efd_err2:
    pop ecx
    pop ebx
.efd_err:
    ret

; ═══════════════════════════════════════════════════════════════════
; FUNCTION CALL
; ═══════════════════════════════════════════════════════════════════
; ebx=name ptr, ecx=name len
fi_call_func:
    push edx
    push esi
    push edi

    ; Save name for lookup
    mov esi, ebx
    movzx ecx, cx
    mov edi, fi_tmpbuf
    push ecx
    rep movsb
    mov byte [edi], 0
    pop ecx

    call fi_advance             ; skip '('

    ; Evaluate arguments onto our "stack" (use fi_callstk)
    xor edx, edx               ; arg count
    push edx                    ; save arg count slot
.fc_args:
    call fi_cur_tok
    cmp al, TK_RPAREN
    je .fc_args_done
    push edx
    call fi_eval_expr           ; arg value in eax
    pop edx
    ; Store arg value
    mov [fi_callstk + edx * 4], eax
    inc edx
    call fi_cur_tok
    cmp al, TK_COMMA
    jne .fc_args
    call fi_advance             ; skip ','
    jmp .fc_args
.fc_args_done:
    mov [esp], edx              ; update arg count
    call fi_cur_tok
    cmp al, TK_RPAREN
    jne .fc_call_end
    call fi_advance             ; skip ')'

    ; Look up function
    pop edx                     ; arg count
    push edx
    mov esi, fi_tmpbuf
    mov eax, [fi_fncnt]
    xor ecx, ecx
.fc_lookup:
    cmp ecx, eax
    jge .fc_notfound
    imul edi, ecx, FUNC_ENTRY_SZ
    add edi, fi_funcs
    push esi
    push eax
    push ecx
    mov esi, fi_tmpbuf
    call strcmp
    pop ecx
    pop eax
    pop esi
    je .fc_found
    inc ecx
    jmp .fc_lookup

.fc_found:
    ; ecx = function index
    imul edi, ecx, FUNC_ENTRY_SZ
    add edi, fi_funcs

    ; Save current interpreter state
    push dword [fi_tokpos]
    push dword [fi_varcnt]
    push dword [fi_retflag]
    push dword [fi_retval]

    ; Set up parameters as local variables
    movzx ecx, byte [edi + FUNC_PCNT]
    mov esi, [edi + FUNC_POFF]  ; param names in string pool
    pop eax                     ; retval (pushed above)
    pop eax                     ; retflag
    pop eax                     ; varcnt -- we keep vars but can overwrite
    pop eax                     ; tokpos

    ; Re-save properly
    push dword [fi_tokpos]
    push dword [fi_retflag]
    push dword [fi_retval]

    mov dword [fi_retflag], 0
    mov dword [fi_retval], 0

    ; Bind parameters
    ; esi points to param name strings (null-separated)
    ; fi_callstk has argument values
    pop eax                     ; retval save
    pop eax                     ; retflag save
    pop eax                     ; tokpos save
    push eax                    ; tokpos
    push dword [fi_retflag]
    push dword [fi_retval]

    ; Simpler approach: iterate params
    movzx ecx, byte [edi + FUNC_PCNT]
    mov esi, [edi + FUNC_POFF]
    xor edx, edx               ; param index
.fc_bind:
    cmp edx, ecx
    jge .fc_bind_done
    push ecx
    push edx
    push esi
    push edi
    ; Get param name length
    mov ebx, esi                ; name ptr
    xor ecx, ecx
.fc_plen:
    cmp byte [esi + ecx], 0
    je .fc_plen_done
    inc ecx
    jmp .fc_plen
.fc_plen_done:
    ; Set variable: ebx=name, ecx=len, eax=value, dl=type
    mov eax, [fi_callstk + edx * 4]   ; Wait, edx was pushed
    pop edi
    pop esi                     ; original esi
    pop edx                     ; param index
    push esi
    push edx
    push edi
    mov eax, [fi_callstk + edx * 4]
    mov ebx, esi                ; param name ptr
    ; Recalculate length
    push eax
    xor ecx, ecx
.fc_plen2:
    cmp byte [esi + ecx], 0
    je .fc_plen2_done
    inc ecx
    jmp .fc_plen2
.fc_plen2_done:
    pop eax
    mov dl, VTYPE_INT
    call fi_set_var
    pop edi
    pop edx
    pop esi
    pop ecx
    ; Advance esi past this param name + null
.fc_skip_pname:
    cmp byte [esi], 0
    je .fc_skip_pname_done
    inc esi
    jmp .fc_skip_pname
.fc_skip_pname_done:
    inc esi                     ; skip null
    inc edx
    jmp .fc_bind
.fc_bind_done:

    ; Save current tokpos
    push dword [fi_tokpos]
    push dword [fi_retflag]

    ; Jump to function body
    mov eax, [edi + FUNC_TOKIDX]
    mov [fi_tokpos], eax
    mov dword [fi_retflag], 0

    ; Execute: skip '{', run body
    call fi_cur_tok
    cmp al, TK_LBRACE
    jne .fc_exec_end
    call fi_advance             ; skip '{'
    call fi_exec_program
    call fi_skip_newlines
    call fi_cur_tok
    cmp al, TK_RBRACE
    jne .fc_exec_end
    call fi_advance
.fc_exec_end:
    ; Get return value
    mov eax, [fi_retval]

    ; Restore state
    pop edx                     ; old retflag
    pop edx                     ; old tokpos
    mov [fi_tokpos], edx
    mov dword [fi_retflag], 0

.fc_call_end:
    pop edx                     ; arg count
    pop edi
    pop esi
    pop edx
    ret

.fc_notfound:
    pop edx                     ; arg count
    ; Print error
    mov esi, fi_e_var
    mov bl, COL_ERR
    call lp_print
    mov esi, fi_tmpbuf
    mov bl, COL_ERR
    call lp_println
    pop edi
    pop esi
    pop edx
    ret

; ═══════════════════════════════════════════════════════════════════
; EXPRESSION EVALUATOR
; Returns: eax = value, dl = type (VTYPE_INT or VTYPE_STR)
; ═══════════════════════════════════════════════════════════════════

fi_eval_expr:
    call fi_eval_comparison
    ret

; fi_eval_comparison — handles == != < > <= >=
fi_eval_comparison:
    call fi_eval_additive       ; left side in eax
    push eax
    push edx
.ec_loop:
    call fi_cur_tok
    cmp al, TK_EQ
    je .ec_op
    cmp al, TK_NEQ
    je .ec_op
    cmp al, TK_LT
    je .ec_op
    cmp al, TK_GT
    je .ec_op
    cmp al, TK_LEQ
    je .ec_op
    cmp al, TK_GEQ
    je .ec_op
    ; No comparison op — return left side
    pop edx
    pop eax
    ret

.ec_op:
    push eax                    ; save operator type
    call fi_advance             ; skip operator
    pop ecx                     ; ecx = operator type
    pop edx                     ; discard old type
    pop eax                     ; left value
    push ecx                    ; save operator
    push eax                    ; save left
    call fi_eval_additive       ; right in eax
    mov ebx, eax                ; right in ebx
    pop eax                     ; left in eax
    pop ecx                     ; operator in ecx

    ; Compare
    cmp cl, TK_EQ
    je .ec_eq
    cmp cl, TK_NEQ
    je .ec_neq
    cmp cl, TK_LT
    je .ec_lt
    cmp cl, TK_GT
    je .ec_gt
    cmp cl, TK_LEQ
    je .ec_leq
    ; TK_GEQ
    cmp eax, ebx
    jge .ec_true
    jmp .ec_false

.ec_eq:
    cmp eax, ebx
    je .ec_true
    jmp .ec_false
.ec_neq:
    cmp eax, ebx
    jne .ec_true
    jmp .ec_false
.ec_lt:
    cmp eax, ebx
    jl .ec_true
    jmp .ec_false
.ec_gt:
    cmp eax, ebx
    jg .ec_true
    jmp .ec_false
.ec_leq:
    cmp eax, ebx
    jle .ec_true
    jmp .ec_false

.ec_true:
    mov eax, 1
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .ec_loop
.ec_false:
    xor eax, eax
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .ec_loop

; fi_eval_additive — handles + and -
fi_eval_additive:
    call fi_eval_multiplicative
    push eax
    push edx                    ; type
.ea_loop:
    call fi_cur_tok
    cmp al, TK_PLUS
    je .ea_add
    cmp al, TK_MINUS
    je .ea_sub
    pop edx
    pop eax
    ret

.ea_add:
    call fi_advance
    pop edx                     ; left type
    pop eax                     ; left value
    push edx                    ; save left type
    push eax                    ; save left value
    call fi_eval_multiplicative ; right in eax, type in dl
    mov ebx, eax                ; right value
    mov cl, dl                  ; right type
    pop eax                     ; left value
    pop edx                     ; left type

    ; Check if string concatenation
    cmp dl, VTYPE_STR
    je .ea_str_concat
    cmp cl, VTYPE_STR
    je .ea_str_concat

    ; Integer addition
    add eax, ebx
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .ea_loop

.ea_str_concat:
    ; Concatenate strings: eax=left (ptr or int), ebx=right (ptr or int)
    ; dl=left type, cl=right type
    push ecx
    push edx
    ; Destination in string pool
    mov edi, [fi_stptr]
    push edi                    ; save start

    ; Copy left operand
    pop edi
    push edi
    cmp dl, VTYPE_STR
    jne .ea_cat_lint
    ; Copy left string
    mov esi, eax
.ea_cat_ls:
    lodsb
    test al, al
    jz .ea_cat_lr
    stosb
    jmp .ea_cat_ls
.ea_cat_lr:
    jmp .ea_cat_right
.ea_cat_lint:
    ; Convert int to string, copy
    push ebx
    call itoa
    mov esi, numbuf
.ea_cat_lis:
    lodsb
    test al, al
    jz .ea_cat_lid
    stosb
    jmp .ea_cat_lis
.ea_cat_lid:
    pop ebx

.ea_cat_right:
    ; Copy right operand
    pop eax                     ; start ptr
    push eax
    pop edx                     ; (we just need it back)
    push edx
    mov eax, [esp + 8]          ; right type (cl saved)
    ; Actually let's get cl back properly
    pop eax                     ; start
    pop edx                     ; left type
    pop ecx                     ; right type (cl)
    push eax                    ; start

    cmp cl, VTYPE_STR
    jne .ea_cat_rint
    mov esi, ebx
.ea_cat_rs:
    lodsb
    test al, al
    jz .ea_cat_done
    stosb
    jmp .ea_cat_rs
.ea_cat_rint:
    push eax
    mov eax, ebx
    call itoa
    mov esi, numbuf
.ea_cat_ris:
    lodsb
    test al, al
    jz .ea_cat_rid
    stosb
    jmp .ea_cat_ris
.ea_cat_rid:
    pop eax

.ea_cat_done:
    mov byte [edi], 0
    inc edi
    mov [fi_stptr], edi
    pop eax                     ; result string ptr
    mov dl, VTYPE_STR
    push edx
    push eax
    jmp .ea_loop

.ea_sub:
    call fi_advance
    pop edx
    pop eax
    push eax
    call fi_eval_multiplicative
    mov ebx, eax
    pop eax
    sub eax, ebx
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .ea_loop

; fi_eval_multiplicative — handles * / %
fi_eval_multiplicative:
    call fi_eval_primary
    push eax
    push edx
.em_loop:
    call fi_cur_tok
    cmp al, TK_STAR
    je .em_mul
    cmp al, TK_SLASH
    je .em_div
    cmp al, TK_PERCENT
    je .em_mod
    pop edx
    pop eax
    ret

.em_mul:
    call fi_advance
    pop edx
    pop eax
    push eax
    call fi_eval_primary
    mov ebx, eax
    pop eax
    imul eax, ebx
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .em_loop

.em_div:
    call fi_advance
    pop edx
    pop eax
    push eax
    call fi_eval_primary
    mov ebx, eax
    pop eax
    test ebx, ebx
    jz .em_divzero
    cdq
    idiv ebx
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .em_loop

.em_mod:
    call fi_advance
    pop edx
    pop eax
    push eax
    call fi_eval_primary
    mov ebx, eax
    pop eax
    test ebx, ebx
    jz .em_divzero
    cdq
    idiv ebx
    mov eax, edx                ; remainder
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .em_loop

.em_divzero:
    mov esi, fi_e_div
    mov bl, COL_ERR
    call lp_println
    xor eax, eax
    mov dl, VTYPE_INT
    push edx
    push eax
    jmp .em_loop

; fi_eval_primary — numbers, strings, identifiers, parens, func calls
fi_eval_primary:
    call fi_skip_newlines
    call fi_cur_tok

    cmp al, TK_NUM
    je .ep_num
    cmp al, TK_STR
    je .ep_str
    cmp al, TK_TRUE
    je .ep_true
    cmp al, TK_FALSE
    je .ep_false
    cmp al, TK_LPAREN
    je .ep_paren
    cmp al, TK_ID
    je .ep_id
    cmp al, TK_MINUS
    je .ep_neg
    ; Unknown — return 0
    xor eax, eax
    mov dl, VTYPE_INT
    ret

.ep_num:
    ; Parse integer from source: ebx=ptr, cx=len
    push ecx
    push ebx
    ; Convert string at ebx, length cx to integer
    mov esi, ebx
    movzx ecx, cx
    xor eax, eax
    xor edx, edx
.ep_atoi:
    test ecx, ecx
    jz .ep_atoi_done
    imul eax, 10
    movzx edx, byte [esi]
    sub edx, '0'
    add eax, edx
    inc esi
    dec ecx
    jmp .ep_atoi
.ep_atoi_done:
    pop ebx
    pop ecx
    call fi_advance
    mov dl, VTYPE_INT
    ret

.ep_str:
    ; ebx = pointer to string in pool
    mov eax, ebx
    call fi_advance
    mov dl, VTYPE_STR
    ret

.ep_true:
    mov eax, 1
    call fi_advance
    mov dl, VTYPE_INT
    ret

.ep_false:
    xor eax, eax
    call fi_advance
    mov dl, VTYPE_INT
    ret

.ep_paren:
    call fi_advance             ; skip '('
    call fi_eval_expr
    push eax
    push edx
    call fi_cur_tok
    cmp al, TK_RPAREN
    jne .ep_paren_end
    call fi_advance             ; skip ')'
.ep_paren_end:
    pop edx
    pop eax
    ret

.ep_neg:
    call fi_advance             ; skip '-'
    call fi_eval_primary
    neg eax
    mov dl, VTYPE_INT
    ret

.ep_id:
    ; Could be variable or function call
    push ebx
    push ecx
    call fi_advance             ; skip identifier
    call fi_cur_tok
    cmp al, TK_LPAREN
    je .ep_id_call
    ; Variable lookup
    pop ecx                     ; name len
    pop ebx                     ; name ptr
    call fi_get_var             ; eax=value, dl=type
    ret

.ep_id_call:
    ; Function call
    pop ecx
    pop ebx
    call fi_call_func
    mov dl, VTYPE_INT
    ret

; ═══════════════════════════════════════════════════════════════════
; VARIABLE TABLE
; ═══════════════════════════════════════════════════════════════════

; fi_set_var — set variable: ebx=name_ptr, ecx=name_len, eax=value, dl=type
fi_set_var:
    pushad
    ; Copy name to tmpbuf for comparison
    mov esi, ebx
    mov edi, fi_errbuf          ; reuse as temp
    movzx ecx, cx
    cmp ecx, 15
    jle .sv_cp
    mov ecx, 15
.sv_cp:
    push ecx
    rep movsb
    mov byte [edi], 0
    pop ecx

    ; Search for existing variable
    mov edx, [fi_varcnt]
    xor ebx, ebx
.sv_search:
    cmp ebx, edx
    jge .sv_new
    imul edi, ebx, VAR_ENTRY_SZ
    add edi, fi_vars
    push esi
    push ebx
    push edx
    mov esi, fi_errbuf
    call strcmp
    pop edx
    pop ebx
    pop esi
    je .sv_update
    inc ebx
    jmp .sv_search

.sv_update:
    ; Found at index ebx
    imul edi, ebx, VAR_ENTRY_SZ
    add edi, fi_vars
    ; eax and dl are in the pushad frame, retrieve from stack
    mov eax, [esp + 28]         ; eax from pushad (eax is last pushed = esp+28)
    mov dl, [esp + 12]          ; edx from pushad (dl = low byte of edx)
    mov [edi + VAR_VALUE], eax
    mov [edi + VAR_TYPE], dl
    popad
    ret

.sv_new:
    ; Add new variable
    mov eax, [fi_varcnt]
    cmp eax, FI_MAX_VARS
    jge .sv_full
    imul edi, eax, VAR_ENTRY_SZ
    add edi, fi_vars
    ; Copy name
    mov esi, fi_errbuf
    push edi
    mov ecx, 16
.sv_cpn:
    lodsb
    stosb
    test al, al
    jz .sv_cpn_pad
    dec ecx
    jnz .sv_cpn
    jmp .sv_cpn_done
.sv_cpn_pad:
    ; Pad rest with zeros
    dec ecx
    jz .sv_cpn_done
    xor al, al
    rep stosb
.sv_cpn_done:
    pop edi
    ; Set value — retrieve from pushad frame
    mov eax, [esp + 28]         ; eax
    mov dl, [esp + 12]          ; dl from edx
    mov [edi + VAR_VALUE], eax
    mov [edi + VAR_TYPE], dl
    inc dword [fi_varcnt]
.sv_full:
    popad
    ret

; fi_get_var — look up variable: ebx=name_ptr, ecx=name_len
; Returns: eax=value, dl=type
fi_get_var:
    push ebx
    push ecx
    push esi
    push edi
    ; Copy name to tmpbuf
    mov esi, ebx
    mov edi, fi_errbuf
    movzx ecx, cx
    cmp ecx, 15
    jle .gv_cp
    mov ecx, 15
.gv_cp:
    push ecx
    rep movsb
    mov byte [edi], 0
    pop ecx

    mov edx, [fi_varcnt]
    xor ebx, ebx
.gv_search:
    cmp ebx, edx
    jge .gv_notfound
    imul edi, ebx, VAR_ENTRY_SZ
    add edi, fi_vars
    push esi
    push ebx
    push edx
    mov esi, fi_errbuf
    call strcmp
    pop edx
    pop ebx
    pop esi
    je .gv_found
    inc ebx
    jmp .gv_search

.gv_found:
    imul edi, ebx, VAR_ENTRY_SZ
    add edi, fi_vars
    mov eax, [edi + VAR_VALUE]
    movzx edx, byte [edi + VAR_TYPE]
    mov dl, [edi + VAR_TYPE]
    pop edi
    pop esi
    pop ecx
    pop ebx
    ret

.gv_notfound:
    xor eax, eax
    mov dl, VTYPE_INT
    pop edi
    pop esi
    pop ecx
    pop ebx
    ret

; (fi_exec_stmt .es_if dispatches to fi_exec_if above)

; ═══════════════════════════════════════════════════════════════════
; KEYWORD STRINGS (for tokenizer)
; ═══════════════════════════════════════════════════════════════════
section .data

fi_kw_var:    db "var", 0
fi_kw_if:     db "if", 0
fi_kw_else:   db "else", 0
fi_kw_while:  db "while", 0
fi_kw_for:    db "for", 0
fi_kw_in:     db "in", 0
fi_kw_func:   db "func", 0
fi_kw_return: db "return", 0
fi_kw_range:  db "range", 0
fi_kw_print:  db "print", 0
fi_kw_true:   db "true", 0
fi_kw_false:  db "false", 0

; Error/info strings
fi_e_syn:     db "Syntax error", 0
fi_e_var:     db "Undefined: ", 0
fi_e_ovf:     db "Stack overflow", 0
fi_e_fnf:     db "File not found: ", 0
fi_e_div:     db "Division by zero", 0

; Command name for shell
c_flux:       db "flux", 0

; ═══════════════════════════════════════════════════════════════════
; BSS — Interpreter state
; ═══════════════════════════════════════════════════════════════════
section .bss

fi_src:        resd 1
fi_srclen:     resd 1
fi_pos:        resd 1
fi_tokbuf:     resb FI_MAX_TOKENS * TK_ENTRY_SZ
fi_tokcnt:     resd 1
fi_tokpos:     resd 1
fi_vars:       resb FI_MAX_VARS * VAR_ENTRY_SZ
fi_varcnt:     resd 1
fi_stpool:     resb 4096
fi_stptr:      resd 1
fi_funcs:      resb FI_MAX_FUNCS * FUNC_ENTRY_SZ
fi_fncnt:      resd 1
fi_callstk:    resd 64
fi_calldepth:  resd 1
fi_retval:     resd 1
fi_retflag:    resd 1
fi_tmpbuf:     resb 256
fi_errbuf:     resb 64
