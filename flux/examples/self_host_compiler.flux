// ═══════════════════════════════════════════════════════════
// Self-hosting Bytecode Compiler for Flux
// Tokenizer + Parser (reused from self_host.flux)
// Bytecode Compiler + Stack-based VM (new)
// ═══════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────
// Utility functions
// ─────────────────────────────────────────────────────────

func is_digit(c) {
    var digits = "0123456789"
    return digits.contains(c)
}

func is_alpha(c) {
    var letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"
    return letters.contains(c)
}

func is_alnum(c) {
    if (is_alpha(c)) { return true }
    return is_digit(c)
}

func is_space(c) {
    if (c == " ") { return true }
    if (c == "\t") { return true }
    return c == "\r"
}

// ═══════════════════════════════════════════════════════════
// Tokenizer (from self_host.flux)
// ═══════════════════════════════════════════════════════════

func make_token(tp, val, line) {
    var t = Map()
    t.set("type", tp)
    t.set("value", val)
    t.set("line", line)
    return t
}

func tokenize(source) {
    var tokens = []
    var pos = 0
    var line = 1
    var src_len = len(source)

    while (pos < src_len) {
        var c = source[pos]

        if (is_space(c)) {
            pos = pos + 1
        } else if (c == "\n") {
            tokens.push(make_token("NEWLINE", "\\n", line))
            line = line + 1
            pos = pos + 1
        } else if (c == "/" && pos + 1 < src_len && source[pos + 1] == "/") {
            while (pos < src_len && source[pos] != "\n") {
                pos = pos + 1
            }
        } else if (is_digit(c)) {
            var num_s = ""
            while (pos < src_len && (is_digit(source[pos]) || source[pos] == ".")) {
                num_s = num_s + source[pos]
                pos = pos + 1
            }
            tokens.push(make_token("NUMBER", num_s, line))
        } else if (c == "\"") {
            pos = pos + 1
            var s = ""
            while (pos < src_len && source[pos] != "\"") {
                if (source[pos] == "\\" && pos + 1 < src_len) {
                    var nc = source[pos + 1]
                    if (nc == "n") {
                        s = s + "\n"
                    } else if (nc == "t") {
                        s = s + "\t"
                    } else if (nc == "\"") {
                        s = s + "\""
                    } else if (nc == "\\") {
                        s = s + "\\"
                    } else {
                        s = s + nc
                    }
                    pos = pos + 2
                } else {
                    s = s + source[pos]
                    pos = pos + 1
                }
            }
            if (pos < src_len) {
                pos = pos + 1
            }
            tokens.push(make_token("STRING", s, line))
        } else if (is_alpha(c)) {
            var word = ""
            while (pos < src_len && is_alnum(source[pos])) {
                word = word + source[pos]
                pos = pos + 1
            }
            if (word == "var") { tokens.push(make_token("VAR", word, line)) }
            else if (word == "func") { tokens.push(make_token("FUNC", word, line)) }
            else if (word == "return") { tokens.push(make_token("RETURN", word, line)) }
            else if (word == "if") { tokens.push(make_token("IF", word, line)) }
            else if (word == "else") { tokens.push(make_token("ELSE", word, line)) }
            else if (word == "while") { tokens.push(make_token("WHILE", word, line)) }
            else if (word == "for") { tokens.push(make_token("FOR", word, line)) }
            else if (word == "in") { tokens.push(make_token("IN", word, line)) }
            else if (word == "true") { tokens.push(make_token("TRUE", word, line)) }
            else if (word == "false") { tokens.push(make_token("FALSE", word, line)) }
            else if (word == "null") { tokens.push(make_token("NULL", word, line)) }
            else if (word == "not") { tokens.push(make_token("NOT", "!", line)) }
            else { tokens.push(make_token("ID", word, line)) }
        } else if (c == "+") {
            if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("PLUS_ASSIGN", "+=", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("PLUS", "+", line))
                pos = pos + 1
            }
        } else if (c == "-") {
            if (pos + 1 < src_len && source[pos + 1] == ">") {
                tokens.push(make_token("ARROW", "->", line))
                pos = pos + 2
            } else if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("MINUS_ASSIGN", "-=", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("MINUS", "-", line))
                pos = pos + 1
            }
        } else if (c == "*") { tokens.push(make_token("STAR", "*", line))    ; pos = pos + 1 }
        else if (c == "/") { tokens.push(make_token("SLASH", "/", line))  ; pos = pos + 1 }
        else if (c == "%") { tokens.push(make_token("PERCENT", "%", line)); pos = pos + 1 }
        else if (c == "(") { tokens.push(make_token("LPAREN", "(", line)) ; pos = pos + 1 }
        else if (c == ")") { tokens.push(make_token("RPAREN", ")", line)) ; pos = pos + 1 }
        else if (c == "{") { tokens.push(make_token("LBRACE", "{", line)) ; pos = pos + 1 }
        else if (c == "}") { tokens.push(make_token("RBRACE", "}", line)) ; pos = pos + 1 }
        else if (c == "[") { tokens.push(make_token("LBRACKET", "[", line)); pos = pos + 1 }
        else if (c == "]") { tokens.push(make_token("RBRACKET", "]", line)); pos = pos + 1 }
        else if (c == ",") { tokens.push(make_token("COMMA", ",", line))  ; pos = pos + 1 }
        else if (c == ":") { tokens.push(make_token("COLON", ":", line))  ; pos = pos + 1 }
        else if (c == ".") { tokens.push(make_token("DOT", ".", line))    ; pos = pos + 1 }
        else if (c == "=") {
            if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("EQ", "==", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("ASSIGN", "=", line))
                pos = pos + 1
            }
        } else if (c == "!") {
            if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("NEQ", "!=", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("NOT", "!", line))
                pos = pos + 1
            }
        } else if (c == "<") {
            if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("LEQ", "<=", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("LT", "<", line))
                pos = pos + 1
            }
        } else if (c == ">") {
            if (pos + 1 < src_len && source[pos + 1] == "=") {
                tokens.push(make_token("GEQ", ">=", line))
                pos = pos + 2
            } else {
                tokens.push(make_token("GT", ">", line))
                pos = pos + 1
            }
        } else if (c == "&" && pos + 1 < src_len && source[pos + 1] == "&") {
            tokens.push(make_token("AND", "&&", line))
            pos = pos + 2
        } else if (c == "|" && pos + 1 < src_len && source[pos + 1] == "|") {
            tokens.push(make_token("OR", "||", line))
            pos = pos + 2
        } else {
            pos = pos + 1
        }
    }

    tokens.push(make_token("EOF", "", line))
    return tokens
}

// ═══════════════════════════════════════════════════════════
// Parser (from self_host.flux)
// ═══════════════════════════════════════════════════════════

var parser_ = Map()

func p_init(tokens) {
    parser_.set("tokens", tokens)
    parser_.set("pos", 0)
}

func p_cur() {
    var tokens = parser_.get("tokens")
    var pos = parser_.get("pos")
    if (pos >= len(tokens)) { return tokens[len(tokens) - 1] }
    return tokens[pos]
}

func p_advance() {
    var t = p_cur()
    parser_.set("pos", parser_.get("pos") + 1)
    return t
}

func p_check(tp) {
    return p_cur().get("type") == tp
}

func p_match(tp) {
    if (p_check(tp)) { p_advance() ; return true }
    return false
}

func p_expect(tp, msg) {
    if (!p_check(tp)) {
        panic("Parse error line " + str(p_cur().get("line")) + ": " + msg + " (got '" + p_cur().get("value") + "')")
    }
    return p_advance()
}

func p_skip_nl() {
    while (p_check("NEWLINE")) { p_advance() }
}

func make_node(kind) {
    var n = Map()
    n.set("node", kind)
    return n
}

// Expression parsing

func parse_expr() {
    return parse_or()
}

func parse_or() {
    var left = parse_and()
    while (p_check("OR")) {
        p_advance()
        var right = parse_and()
        var n = make_node("binary")
        n.set("op", "||")
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_and() {
    var left = parse_eq()
    while (p_check("AND")) {
        p_advance()
        var right = parse_eq()
        var n = make_node("binary")
        n.set("op", "&&")
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_eq() {
    var left = parse_cmp()
    while (p_check("EQ") || p_check("NEQ")) {
        var op = p_advance().get("value")
        var right = parse_cmp()
        var n = make_node("binary")
        n.set("op", op)
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_cmp() {
    var left = parse_add()
    while (p_check("LT") || p_check("GT") || p_check("LEQ") || p_check("GEQ")) {
        var op = p_advance().get("value")
        var right = parse_add()
        var n = make_node("binary")
        n.set("op", op)
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_add() {
    var left = parse_mul()
    while (p_check("PLUS") || p_check("MINUS")) {
        var op = p_advance().get("value")
        var right = parse_mul()
        var n = make_node("binary")
        n.set("op", op)
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_mul() {
    var left = parse_unary()
    while (p_check("STAR") || p_check("SLASH") || p_check("PERCENT")) {
        var op = p_advance().get("value")
        var right = parse_unary()
        var n = make_node("binary")
        n.set("op", op)
        n.set("left", left)
        n.set("right", right)
        left = n
    }
    return left
}

func parse_unary() {
    if (p_check("MINUS")) {
        p_advance()
        var operand = parse_unary()
        var n = make_node("unary")
        n.set("op", "-")
        n.set("operand", operand)
        return n
    }
    if (p_check("NOT")) {
        p_advance()
        var operand = parse_unary()
        var n = make_node("unary")
        n.set("op", "!")
        n.set("operand", operand)
        return n
    }
    return parse_postfix()
}

func parse_postfix() {
    var expr = parse_primary()
    var cont = true
    while (cont) {
        if (p_check("LBRACKET")) {
            p_advance()
            var idx = parse_expr()
            p_expect("RBRACKET", "expected ']'")
            var n = make_node("index")
            n.set("object", expr)
            n.set("index", idx)
            expr = n
        } else if (p_check("DOT")) {
            p_advance()
            var method = p_expect("ID", "expected method name").get("value")
            if (p_check("LPAREN")) {
                p_advance()
                var args = []
                p_skip_nl()
                var mc_cont = true
                while (mc_cont && !p_check("RPAREN") && !p_check("EOF")) {
                    args.push(parse_expr())
                    p_skip_nl()
                    if (!p_match("COMMA")) { mc_cont = false }
                    p_skip_nl()
                }
                p_expect("RPAREN", "expected ')'")
                var n = make_node("method_call")
                n.set("object", expr)
                n.set("method", method)
                n.set("args", args)
                expr = n
            } else {
                var n = make_node("field_access")
                n.set("object", expr)
                n.set("field", method)
                expr = n
            }
        } else {
            cont = false
        }
    }
    return expr
}

func parse_primary() {
    if (p_check("NUMBER")) {
        var t = p_advance()
        var n = make_node("number")
        n.set("value", num(t.get("value")))
        return n
    }
    if (p_check("STRING")) {
        var t = p_advance()
        var n = make_node("string")
        n.set("value", t.get("value"))
        return n
    }
    if (p_check("TRUE"))  { p_advance() ; var n = make_node("bool") ; n.set("value", true) ; return n }
    if (p_check("FALSE")) { p_advance() ; var n = make_node("bool") ; n.set("value", false) ; return n }
    if (p_check("NULL"))   { p_advance() ; return make_node("null") }

    if (p_check("LBRACKET")) {
        p_advance()
        var elems = []
        p_skip_nl()
        var arr_cont = true
        while (arr_cont && !p_check("RBRACKET") && !p_check("EOF")) {
            elems.push(parse_expr())
            p_skip_nl()
            if (!p_match("COMMA")) { arr_cont = false }
            p_skip_nl()
        }
        p_expect("RBRACKET", "expected ']'")
        var n = make_node("array")
        n.set("elements", elems)
        return n
    }

    if (p_check("LPAREN")) {
        p_advance()
        var expr = parse_expr()
        p_expect("RPAREN", "expected ')'")
        return expr
    }

    if (p_check("ID")) {
        var name = p_advance().get("value")
        if (p_check("LPAREN")) {
            p_advance()
            var args = []
            p_skip_nl()
            var call_cont = true
            while (call_cont && !p_check("RPAREN") && !p_check("EOF")) {
                args.push(parse_expr())
                p_skip_nl()
                if (!p_match("COMMA")) { call_cont = false }
                p_skip_nl()
            }
            p_expect("RPAREN", "expected ')'")
            var n = make_node("call")
            n.set("name", name)
            n.set("args", args)
            return n
        }
        var n = make_node("id")
        n.set("name", name)
        return n
    }

    panic("Parse error line " + str(p_cur().get("line")) + ": unexpected token '" + p_cur().get("value") + "'")
}

// Statement parsing

func parse_block() {
    p_expect("LBRACE", "expected '{'")
    p_skip_nl()
    var stmts = []
    while (!p_check("RBRACE") && !p_check("EOF")) {
        stmts.push(parse_stmt())
        p_skip_nl()
    }
    p_expect("RBRACE", "expected '}'")
    return stmts
}

func parse_stmt() {
    p_skip_nl()

    if (p_check("VAR")) {
        p_advance()
        var name = p_expect("ID", "expected variable name").get("value")
        if (p_check("COLON")) { p_advance() ; p_expect("ID", "expected type") }
        p_expect("ASSIGN", "expected '='")
        var init = parse_expr()
        var n = make_node("var_decl")
        n.set("name", name)
        n.set("init", init)
        return n
    }

    if (p_check("FUNC")) {
        p_advance()
        var name = p_expect("ID", "expected function name").get("value")
        p_expect("LPAREN", "expected '('")
        var params = []
        var param_cont = true
        while (param_cont && !p_check("RPAREN") && !p_check("EOF")) {
            params.push(p_expect("ID", "expected param name").get("value"))
            if (p_check("COLON")) { p_advance() ; p_expect("ID", "expected type") }
            if (!p_match("COMMA")) { param_cont = false }
        }
        p_expect("RPAREN", "expected ')'")
        if (p_match("ARROW")) { p_expect("ID", "expected return type") }
        p_skip_nl()
        var body = parse_block()
        var n = make_node("fn_decl")
        n.set("name", name)
        n.set("params", params)
        n.set("body", body)
        return n
    }

    if (p_check("IF")) {
        p_advance()
        var has_paren = p_match("LPAREN")
        var cond = parse_expr()
        if (has_paren) { p_expect("RPAREN", "expected ')'") }
        p_skip_nl()
        var then_body = parse_block()
        p_skip_nl()
        var else_body = []
        if (p_match("ELSE")) {
            p_skip_nl()
            if (p_check("IF")) {
                else_body = [parse_stmt()]
            } else {
                else_body = parse_block()
            }
        }
        var n = make_node("if")
        n.set("cond", cond)
        n.set("then", then_body)
        n.set("else", else_body)
        return n
    }

    if (p_check("WHILE")) {
        p_advance()
        var has_paren = p_match("LPAREN")
        var cond = parse_expr()
        if (has_paren) { p_expect("RPAREN", "expected ')'") }
        p_skip_nl()
        var body = parse_block()
        var n = make_node("while")
        n.set("cond", cond)
        n.set("body", body)
        return n
    }

    if (p_check("FOR")) {
        p_advance()
        var varname = p_expect("ID", "expected loop variable").get("value")
        p_expect("IN", "expected 'in'")
        var iter = parse_expr()
        p_skip_nl()
        var body = parse_block()
        var n = make_node("for_in")
        n.set("var", varname)
        n.set("iter", iter)
        n.set("body", body)
        return n
    }

    if (p_check("RETURN")) {
        p_advance()
        var val = null
        if (!p_check("NEWLINE") && !p_check("RBRACE") && !p_check("EOF")) {
            val = parse_expr()
        }
        var n = make_node("return")
        n.set("value", val)
        return n
    }

    var expr = parse_expr()

    if (p_check("ASSIGN")) {
        p_advance()
        var val = parse_expr()
        if (expr.get("node") == "id") {
            var n = make_node("assign")
            n.set("name", expr.get("name"))
            n.set("value", val)
            return n
        }
        if (expr.get("node") == "index") {
            var n = make_node("index_assign")
            n.set("object", expr.get("object"))
            n.set("index", expr.get("index"))
            n.set("value", val)
            return n
        }
        panic("invalid assignment target")
    }

    if (p_check("PLUS_ASSIGN") || p_check("MINUS_ASSIGN")) {
        var op_tok = p_advance()
        var val = parse_expr()
        var op = "+"
        if (op_tok.get("type") == "MINUS_ASSIGN") { op = "-" }
        if (expr.get("node") == "id") {
            var bin = make_node("binary")
            bin.set("op", op)
            bin.set("left", expr)
            bin.set("right", val)
            var n = make_node("assign")
            n.set("name", expr.get("name"))
            n.set("value", bin)
            return n
        }
        panic("invalid compound assignment target")
    }

    var n = make_node("expr_stmt")
    n.set("expr", expr)
    return n
}

func parse_program(tokens) {
    p_init(tokens)
    var stmts = []
    p_skip_nl()
    while (!p_check("EOF")) {
        stmts.push(parse_stmt())
        p_skip_nl()
    }
    return stmts
}

// ═══════════════════════════════════════════════════════════
// Bytecode Compiler
// ═══════════════════════════════════════════════════════════

// Chunk: holds bytecode, constants, and name tables
func new_chunk() {
    var ch = Map()
    ch.set("code", [])
    ch.set("constants", [])
    ch.set("names", [])
    ch.set("const_map", Map())
    ch.set("name_map", Map())
    return ch
}

func make_instr(op, a, b) {
    var ins = Map()
    ins.set("op", op)
    ins.set("a", a)
    ins.set("b", b)
    return ins
}

func chunk_add_const(ch, value) {
    // Use string key for deduplication
    var key = str(value)
    if (type(value) == "String") {
        key = "s:" + value
    } else if (value == null) {
        key = "null"
    } else if (type(value) == "Bool") {
        if (value) { key = "b:true" } else { key = "b:false" }
    } else {
        key = "n:" + str(value)
    }
    var cmap = ch.get("const_map")
    if (cmap.has(key)) {
        return cmap.get(key)
    }
    var consts = ch.get("constants")
    var idx = len(consts)
    consts.push(value)
    cmap.set(key, idx)
    return idx
}

func chunk_add_name(ch, name) {
    var nmap = ch.get("name_map")
    if (nmap.has(name)) {
        return nmap.get(name)
    }
    var names = ch.get("names")
    var idx = len(names)
    names.push(name)
    nmap.set(name, idx)
    return idx
}

func chunk_emit(ch, op, a, b) {
    var code = ch.get("code")
    code.push(make_instr(op, a, b))
}

func chunk_here(ch) {
    return len(ch.get("code"))
}

func chunk_emit_jump(ch, op) {
    var pos = chunk_here(ch)
    chunk_emit(ch, op, 0, 0)
    return pos
}

func chunk_patch_jump(ch, pos, target) {
    var code = ch.get("code")
    var ins = code[pos]
    ins.set("a", target)
}

// ── Compiler state ──────────────────────────────────────
// functions_map: name -> { chunk, params }
var compiler_functions = Map()

func compile_expr(ch, node) {
    var kind = node.get("node")

    if (kind == "number") {
        var idx = chunk_add_const(ch, node.get("value"))
        chunk_emit(ch, "PUSH_CONST", idx, 0)
        return null
    }
    if (kind == "string") {
        var idx = chunk_add_const(ch, node.get("value"))
        chunk_emit(ch, "PUSH_CONST", idx, 0)
        return null
    }
    if (kind == "bool") {
        if (node.get("value")) {
            chunk_emit(ch, "PUSH_TRUE", 0, 0)
        } else {
            chunk_emit(ch, "PUSH_FALSE", 0, 0)
        }
        return null
    }
    if (kind == "null") {
        chunk_emit(ch, "PUSH_NIL", 0, 0)
        return null
    }

    if (kind == "id") {
        var name_idx = chunk_add_name(ch, node.get("name"))
        chunk_emit(ch, "LOAD", name_idx, 0)
        return null
    }

    if (kind == "array") {
        var elems = node.get("elements")
        var i = 0
        while (i < len(elems)) {
            compile_expr(ch, elems[i])
            i = i + 1
        }
        chunk_emit(ch, "MAKE_ARRAY", len(elems), 0)
        return null
    }

    if (kind == "index") {
        compile_expr(ch, node.get("object"))
        compile_expr(ch, node.get("index"))
        chunk_emit(ch, "INDEX_GET", 0, 0)
        return null
    }

    if (kind == "field_access") {
        compile_expr(ch, node.get("object"))
        var field = node.get("field")
        var fidx = chunk_add_const(ch, field)
        chunk_emit(ch, "FIELD_GET", fidx, 0)
        return null
    }

    if (kind == "method_call") {
        var method = node.get("method")
        var args = node.get("args")
        compile_expr(ch, node.get("object"))
        var i = 0
        while (i < len(args)) {
            compile_expr(ch, args[i])
            i = i + 1
        }
        var midx = chunk_add_const(ch, method)
        chunk_emit(ch, "METHOD_CALL", midx, len(args))
        return null
    }

    if (kind == "unary") {
        compile_expr(ch, node.get("operand"))
        var op = node.get("op")
        if (op == "-") { chunk_emit(ch, "NEG", 0, 0) }
        else if (op == "!") { chunk_emit(ch, "NOT", 0, 0) }
        return null
    }

    if (kind == "binary") {
        var op = node.get("op")

        // Short-circuit: &&
        if (op == "&&") {
            compile_expr(ch, node.get("left"))
            var jump_false = chunk_emit_jump(ch, "JUMP_IF_FALSE_PEEK")
            chunk_emit(ch, "POP", 0, 0)
            compile_expr(ch, node.get("right"))
            chunk_patch_jump(ch, jump_false, chunk_here(ch))
            return null
        }
        // Short-circuit: ||
        if (op == "||") {
            compile_expr(ch, node.get("left"))
            var jump_true = chunk_emit_jump(ch, "JUMP_IF_TRUE_PEEK")
            chunk_emit(ch, "POP", 0, 0)
            compile_expr(ch, node.get("right"))
            chunk_patch_jump(ch, jump_true, chunk_here(ch))
            return null
        }

        compile_expr(ch, node.get("left"))
        compile_expr(ch, node.get("right"))
        if (op == "+") { chunk_emit(ch, "ADD", 0, 0) }
        else if (op == "-") { chunk_emit(ch, "SUB", 0, 0) }
        else if (op == "*") { chunk_emit(ch, "MUL", 0, 0) }
        else if (op == "/") { chunk_emit(ch, "DIV", 0, 0) }
        else if (op == "%") { chunk_emit(ch, "MOD", 0, 0) }
        else if (op == "==") { chunk_emit(ch, "EQ", 0, 0) }
        else if (op == "!=") { chunk_emit(ch, "NEQ", 0, 0) }
        else if (op == "<") { chunk_emit(ch, "LT", 0, 0) }
        else if (op == ">") { chunk_emit(ch, "GT", 0, 0) }
        else if (op == "<=") { chunk_emit(ch, "LEQ", 0, 0) }
        else if (op == ">=") { chunk_emit(ch, "GEQ", 0, 0) }
        return null
    }

    if (kind == "call") {
        var name = node.get("name")
        var args = node.get("args")
        var i = 0
        while (i < len(args)) {
            compile_expr(ch, args[i])
            i = i + 1
        }
        var name_idx = chunk_add_name(ch, name)
        chunk_emit(ch, "CALL", name_idx, len(args))
        return null
    }

    panic("compile_expr: unknown node kind: " + kind)
}

func compile_stmt(ch, node) {
    var kind = node.get("node")

    if (kind == "var_decl") {
        if (node.get("init") != null) {
            compile_expr(ch, node.get("init"))
        } else {
            chunk_emit(ch, "PUSH_NIL", 0, 0)
        }
        var name_idx = chunk_add_name(ch, node.get("name"))
        chunk_emit(ch, "DEFINE", name_idx, 0)
        return null
    }

    if (kind == "assign") {
        compile_expr(ch, node.get("value"))
        var name_idx = chunk_add_name(ch, node.get("name"))
        chunk_emit(ch, "STORE", name_idx, 0)
        return null
    }

    if (kind == "index_assign") {
        compile_expr(ch, node.get("object"))
        compile_expr(ch, node.get("index"))
        compile_expr(ch, node.get("value"))
        chunk_emit(ch, "INDEX_SET", 0, 0)
        return null
    }

    if (kind == "fn_decl") {
        // Compile function body into a separate chunk
        var fn_chunk = new_chunk()
        var body = node.get("body")
        var i = 0
        while (i < len(body)) {
            compile_stmt(fn_chunk, body[i])
            i = i + 1
        }
        // Ensure function ends with a return
        chunk_emit(fn_chunk, "RETURN_NIL", 0, 0)

        var fn_info = Map()
        fn_info.set("chunk", fn_chunk)
        fn_info.set("params", node.get("params"))
        compiler_functions.set(node.get("name"), fn_info)
        return null
    }

    if (kind == "return") {
        if (node.get("value") != null) {
            compile_expr(ch, node.get("value"))
            chunk_emit(ch, "RETURN", 0, 0)
        } else {
            chunk_emit(ch, "RETURN_NIL", 0, 0)
        }
        return null
    }

    if (kind == "if") {
        compile_expr(ch, node.get("cond"))
        var jump_false = chunk_emit_jump(ch, "JUMP_IF_FALSE")
        // then body
        var then_body = node.get("then")
        var ti = 0
        while (ti < len(then_body)) {
            compile_stmt(ch, then_body[ti])
            ti = ti + 1
        }
        var else_body = node.get("else")
        if (len(else_body) > 0) {
            var jump_end = chunk_emit_jump(ch, "JUMP")
            chunk_patch_jump(ch, jump_false, chunk_here(ch))
            var ei = 0
            while (ei < len(else_body)) {
                compile_stmt(ch, else_body[ei])
                ei = ei + 1
            }
            chunk_patch_jump(ch, jump_end, chunk_here(ch))
        } else {
            chunk_patch_jump(ch, jump_false, chunk_here(ch))
        }
        return null
    }

    if (kind == "while") {
        var loop_start = chunk_here(ch)
        compile_expr(ch, node.get("cond"))
        var jump_exit = chunk_emit_jump(ch, "JUMP_IF_FALSE")
        var body = node.get("body")
        var bi = 0
        while (bi < len(body)) {
            compile_stmt(ch, body[bi])
            bi = bi + 1
        }
        chunk_emit(ch, "JUMP", loop_start, 0)
        chunk_patch_jump(ch, jump_exit, chunk_here(ch))
        return null
    }

    if (kind == "for_in") {
        // compile iterator expression
        compile_expr(ch, node.get("iter"))
        // Store iterator array in a temp variable
        var iter_name = "__iter_" + str(chunk_here(ch))
        var iter_idx = chunk_add_name(ch, iter_name)
        chunk_emit(ch, "DEFINE", iter_idx, 0)

        // Counter variable
        var counter_name = "__i_" + str(chunk_here(ch))
        var counter_idx = chunk_add_name(ch, counter_name)
        var zero_idx = chunk_add_const(ch, 0)
        chunk_emit(ch, "PUSH_CONST", zero_idx, 0)
        chunk_emit(ch, "DEFINE", counter_idx, 0)

        // Loop start
        var loop_start = chunk_here(ch)
        // Load counter, load len of iter, compare
        chunk_emit(ch, "LOAD", counter_idx, 0)
        chunk_emit(ch, "LOAD", iter_idx, 0)
        chunk_emit(ch, "LEN", 0, 0)
        chunk_emit(ch, "LT", 0, 0)
        var jump_exit = chunk_emit_jump(ch, "JUMP_IF_FALSE")

        // Define loop variable = iter[counter]
        var var_name = node.get("var")
        var var_idx = chunk_add_name(ch, var_name)
        chunk_emit(ch, "LOAD", iter_idx, 0)
        chunk_emit(ch, "LOAD", counter_idx, 0)
        chunk_emit(ch, "INDEX_GET", 0, 0)
        chunk_emit(ch, "DEFINE", var_idx, 0)

        // Compile body
        var body = node.get("body")
        var bi = 0
        while (bi < len(body)) {
            compile_stmt(ch, body[bi])
            bi = bi + 1
        }

        // Increment counter
        chunk_emit(ch, "LOAD", counter_idx, 0)
        var one_idx = chunk_add_const(ch, 1)
        chunk_emit(ch, "PUSH_CONST", one_idx, 0)
        chunk_emit(ch, "ADD", 0, 0)
        chunk_emit(ch, "STORE", counter_idx, 0)

        chunk_emit(ch, "JUMP", loop_start, 0)
        chunk_patch_jump(ch, jump_exit, chunk_here(ch))
        return null
    }

    if (kind == "expr_stmt") {
        compile_expr(ch, node.get("expr"))
        chunk_emit(ch, "POP", 0, 0)
        return null
    }

    panic("compile_stmt: unknown node kind: " + kind)
}

func compile_program(ch, stmts) {
    var i = 0
    while (i < len(stmts)) {
        var stmt = stmts[i]
        if (stmt.get("node") == "fn_decl") {
            compile_stmt(ch, stmt)
        }
        i = i + 1
    }
    // Now compile top-level non-function statements
    i = 0
    while (i < len(stmts)) {
        var stmt = stmts[i]
        if (stmt.get("node") != "fn_decl") {
            compile_stmt(ch, stmt)
        }
        i = i + 1
    }
}

// ═══════════════════════════════════════════════════════════
// Stack-based VM
// ═══════════════════════════════════════════════════════════

var vm_output = []

func val_to_str(v) {
    if (v == null) { return "null" }
    if (type(v) == "Number") { return str(v) }
    if (type(v) == "Bool") {
        if (v) { return "true" }
        return "false"
    }
    if (type(v) == "String") { return v }
    if (type(v) == "Array") {
        var parts = []
        var ai = 0
        while (ai < len(v)) {
            parts.push(val_to_str(v[ai]))
            ai = ai + 1
        }
        return "[" + parts.join(", ") + "]"
    }
    return str(v)
}

func vm_env_get(env, name) {
    var i = len(env) - 1
    while (i >= 0) {
        var scope = env[i]
        if (scope.has(name)) { return scope.get(name) }
        i = i - 1
    }
    panic("VM: undefined variable: " + name)
}

func vm_env_set(env, name, val) {
    var i = len(env) - 1
    while (i >= 0) {
        var scope = env[i]
        if (scope.has(name)) { scope.set(name, val) ; return null }
        i = i - 1
    }
    panic("VM: cannot assign undefined: " + name)
}

func vm_env_define(env, name, val) {
    var top = env[len(env) - 1]
    top.set(name, val)
}

func vm_is_builtin(name) {
    if (name == "print") { return true }
    if (name == "str") { return true }
    if (name == "num") { return true }
    if (name == "len") { return true }
    if (name == "type") { return true }
    if (name == "range") { return true }
    if (name == "panic") { return true }
    return false
}

func vm_call_builtin(name, args) {
    if (name == "print") {
        var parts = []
        var i = 0
        while (i < len(args)) {
            parts.push(val_to_str(args[i]))
            i = i + 1
        }
        var line = parts.join(" ")
        vm_output.push(line)
        return null
    }
    if (name == "str") {
        if (len(args) == 0) { return "" }
        return val_to_str(args[0])
    }
    if (name == "num") {
        if (len(args) == 0) { return 0 }
        return num(val_to_str(args[0]))
    }
    if (name == "len") {
        if (len(args) == 0) { return 0 }
        return len(args[0])
    }
    if (name == "type") {
        if (len(args) == 0) { return "Nil" }
        return type(args[0])
    }
    if (name == "range") {
        if (len(args) == 0) { return [] }
        return range(args[0])
    }
    if (name == "panic") {
        var msg = "panic"
        if (len(args) > 0) { msg = val_to_str(args[0]) }
        panic(msg)
    }
    return null
}

// Execute a chunk with given env and functions map
// Returns { signal: "return", value: V } or null
func vm_exec(ch, env, functions) {
    var code = ch.get("code")
    var constants = ch.get("constants")
    var names = ch.get("names")
    var stack = []
    var pc = 0
    var code_len = len(code)

    while (pc < code_len) {
        var ins = code[pc]
        var op = ins.get("op")
        var a = ins.get("a")
        var b = ins.get("b")

        if (op == "PUSH_CONST") {
            stack.push(constants[a])
        } else if (op == "PUSH_NIL") {
            stack.push(null)
        } else if (op == "PUSH_TRUE") {
            stack.push(true)
        } else if (op == "PUSH_FALSE") {
            stack.push(false)
        } else if (op == "LOAD") {
            var name = names[a]
            var val = vm_env_get(env, name)
            stack.push(val)
        } else if (op == "STORE") {
            var name = names[a]
            var val = stack.pop()
            vm_env_set(env, name, val)
        } else if (op == "DEFINE") {
            var name = names[a]
            var val = stack.pop()
            vm_env_define(env, name, val)
        } else if (op == "ADD") {
            var r = stack.pop()
            var l = stack.pop()
            if (type(l) == "String" || type(r) == "String") {
                stack.push(val_to_str(l) + val_to_str(r))
            } else {
                stack.push(l + r)
            }
        } else if (op == "SUB") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l - r)
        } else if (op == "MUL") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l * r)
        } else if (op == "DIV") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l / r)
        } else if (op == "MOD") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l % r)
        } else if (op == "EQ") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l == r)
        } else if (op == "NEQ") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l != r)
        } else if (op == "LT") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l < r)
        } else if (op == "GT") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l > r)
        } else if (op == "LEQ") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l <= r)
        } else if (op == "GEQ") {
            var r = stack.pop()
            var l = stack.pop()
            stack.push(l >= r)
        } else if (op == "NEG") {
            var val = stack.pop()
            stack.push(0 - val)
        } else if (op == "NOT") {
            var val = stack.pop()
            stack.push(!val)
        } else if (op == "JUMP") {
            pc = a
            pc = pc - 1
        } else if (op == "JUMP_IF_FALSE") {
            var val = stack.pop()
            if (!val) {
                pc = a - 1
            }
        } else if (op == "JUMP_IF_TRUE") {
            var val = stack.pop()
            if (val) {
                pc = a - 1
            }
        } else if (op == "JUMP_IF_FALSE_PEEK") {
            var val = stack[len(stack) - 1]
            if (!val) {
                pc = a - 1
            }
        } else if (op == "JUMP_IF_TRUE_PEEK") {
            var val = stack[len(stack) - 1]
            if (val) {
                pc = a - 1
            }
        } else if (op == "POP") {
            if (len(stack) > 0) {
                stack.pop()
            }
        } else if (op == "CALL") {
            var name = names[a]
            var argc = b
            var args = []
            var ai = 0
            while (ai < argc) {
                args.push(null)
                ai = ai + 1
            }
            // Pop args in reverse order
            ai = argc - 1
            while (ai >= 0) {
                args[ai] = stack.pop()
                ai = ai - 1
            }

            if (vm_is_builtin(name)) {
                var result = vm_call_builtin(name, args)
                stack.push(result)
            } else if (functions.has(name)) {
                var fn_info = functions.get(name)
                var fn_chunk = fn_info.get("chunk")
                var params = fn_info.get("params")
                // Create new env for function
                var fn_env = [Map()]
                var pi = 0
                while (pi < len(params)) {
                    var pval = null
                    if (pi < len(args)) { pval = args[pi] }
                    vm_env_define(fn_env, params[pi], pval)
                    pi = pi + 1
                }
                var result = vm_exec(fn_chunk, fn_env, functions)
                if (result != null && type(result) == "Map" && result.has("signal")) {
                    stack.push(result.get("value"))
                } else {
                    stack.push(null)
                }
            } else {
                panic("VM: undefined function: " + name)
            }
        } else if (op == "RETURN") {
            var val = stack.pop()
            var sig = Map()
            sig.set("signal", "return")
            sig.set("value", val)
            return sig
        } else if (op == "RETURN_NIL") {
            var sig = Map()
            sig.set("signal", "return")
            sig.set("value", null)
            return sig
        } else if (op == "MAKE_ARRAY") {
            var arr = []
            var elems = []
            var ai = 0
            while (ai < a) {
                elems.push(null)
                ai = ai + 1
            }
            ai = a - 1
            while (ai >= 0) {
                elems[ai] = stack.pop()
                ai = ai - 1
            }
            ai = 0
            while (ai < a) {
                arr.push(elems[ai])
                ai = ai + 1
            }
            stack.push(arr)
        } else if (op == "INDEX_GET") {
            var idx = stack.pop()
            var obj = stack.pop()
            stack.push(obj[idx])
        } else if (op == "INDEX_SET") {
            var val = stack.pop()
            var idx = stack.pop()
            var obj = stack.pop()
            obj[idx] = val
        } else if (op == "LEN") {
            var obj = stack.pop()
            stack.push(len(obj))
        } else if (op == "FIELD_GET") {
            var obj = stack.pop()
            var field = constants[a]
            if (type(obj) == "Map") {
                stack.push(obj.get(field))
            } else {
                panic("VM: cannot get field '" + field + "' on non-map")
            }
        } else if (op == "METHOD_CALL") {
            var method = constants[a]
            var argc = b
            // Pop args
            var args = []
            var ai = 0
            while (ai < argc) {
                args.push(null)
                ai = ai + 1
            }
            ai = argc - 1
            while (ai >= 0) {
                args[ai] = stack.pop()
                ai = ai - 1
            }
            var obj = stack.pop()

            if (type(obj) == "Array") {
                if (method == "push") { obj.push(args[0]) ; stack.push(null) }
                else if (method == "pop") { stack.push(obj.pop()) }
                else if (method == "len") { stack.push(len(obj)) }
                else if (method == "join") { stack.push(obj.join(args[0])) }
                else if (method == "reverse") { stack.push(obj.reverse()) }
                else if (method == "contains") {
                    var found = false
                    var ci = 0
                    while (ci < len(obj)) {
                        if (obj[ci] == args[0]) { found = true }
                        ci = ci + 1
                    }
                    stack.push(found)
                }
                else { panic("VM: unknown array method: " + method) }
            } else if (type(obj) == "String") {
                if (method == "len") { stack.push(len(obj)) }
                else if (method == "upper") { stack.push(obj.upper()) }
                else if (method == "lower") { stack.push(obj.lower()) }
                else if (method == "split") { stack.push(obj.split(args[0])) }
                else if (method == "contains") { stack.push(obj.contains(args[0])) }
                else if (method == "trim") { stack.push(obj.trim()) }
                else { panic("VM: unknown string method: " + method) }
            } else if (type(obj) == "Map") {
                if (method == "get") { stack.push(obj.get(args[0])) }
                else if (method == "set") { obj.set(args[0], args[1]) ; stack.push(null) }
                else if (method == "has") { stack.push(obj.has(args[0])) }
                else if (method == "keys") { stack.push(obj.keys()) }
                else { panic("VM: unknown map method: " + method) }
            } else {
                panic("VM: cannot call method '" + method + "' on " + type(obj))
            }
        } else {
            panic("VM: unknown opcode: " + op)
        }

        pc = pc + 1
    }

    return null
}

// ═══════════════════════════════════════════════════════════
// Main entry: compile + run
// ═══════════════════════════════════════════════════════════

func flux_compile_and_run(source) {
    compiler_functions = Map()
    vm_output = []

    // 1. Tokenize
    var tokens = tokenize(source)

    // 2. Parse
    var ast = parse_program(tokens)

    // 3. Compile
    var ch = new_chunk()
    compile_program(ch, ast)

    // 4. Execute on VM
    var env = [Map()]
    vm_exec(ch, env, compiler_functions)

    return vm_output
}

// ═══════════════════════════════════════════════════════════
// Test Suite
// ═══════════════════════════════════════════════════════════

var total_tests = 0
var passed_tests = 0

func check(test_name, source, expected) {
    total_tests = total_tests + 1
    var result = flux_compile_and_run(source)
    var result_str = result.join("|")
    var expect_str = expected.join("|")
    if (result_str == expect_str) {
        print("  PASS: " + test_name)
        passed_tests = passed_tests + 1
    } else {
        print("  FAIL: " + test_name)
        print("    expected: " + expect_str)
        print("    got:      " + result_str)
    }
}

print("=== Flux Self-hosting Bytecode Compiler ===")
print("=== Compile Flux to bytecode, run on VM ===")
print("")

// Test 1: Basic arithmetic
var t1_src = "print(2 + 3 * 4)\nprint(10 - 3)\nprint(15 / 3)\nprint(17 % 5)"
check("basic arithmetic", t1_src, ["14", "7", "5", "2"])

// Test 2: Variables
var t2_src = "var x = 42\nprint(x)\nvar y = 10\ny = y + 5\nprint(y)"
check("variables", t2_src, ["42", "15"])

// Test 3: String concatenation
var t3_src = "var s = \"hello\"\nvar t = \" world\"\nprint(s + t)"
check("string concatenation", t3_src, ["hello world"])

// Test 4: If/else true branch
var t4_src = "var x = 10\nif (x > 5) { print(\"big\") } else { print(\"small\") }"
check("if/else true", t4_src, ["big"])

// Test 5: If/else false branch
var t5_src = "var x = 2\nif (x > 5) { print(\"big\") } else { print(\"small\") }"
check("if/else false", t5_src, ["small"])

// Test 6: Else-if chain
var t6_src = "var x = 5\nif (x == 1) { print(\"one\") } else if (x == 5) { print(\"five\") } else { print(\"other\") }"
check("else-if chain", t6_src, ["five"])

// Test 7: While loop
var t7_src = "var sum = 0\nvar i = 1\nwhile (i <= 10) {\nsum = sum + i\ni = i + 1\n}\nprint(sum)"
check("while loop sum", t7_src, ["55"])

// Test 8: Simple function
var t8_src = "func double(x) {\nreturn x * 2\n}\nprint(double(21))"
check("function call", t8_src, ["42"])

// Test 9: Function with string return
var t9_src = "func greet(name) {\nreturn \"Hi \" + name\n}\nprint(greet(\"Flux\"))"
check("function string", t9_src, ["Hi Flux"])

// Test 10: Recursion - factorial
var t10_src = "func fact(n) {\nif (n <= 1) { return 1 }\nreturn n * fact(n - 1)\n}\nprint(fact(10))"
check("factorial", t10_src, ["3628800"])

// Test 11: Recursion - fibonacci
var t11_src = "func fib(n) {\nif (n <= 1) { return n }\nreturn fib(n - 1) + fib(n - 2)\n}\nprint(fib(10))"
check("fibonacci", t11_src, ["55"])

// Test 12: Nested function calls
var t12_src = "func add(a, b) { return a + b }\nfunc mul(a, b) { return a * b }\nprint(add(mul(3, 4), mul(5, 6)))"
check("nested calls", t12_src, ["42"])

// Test 13: Boolean logic and short-circuit
var t13_src = "var a = true\nvar b = false\nif (a && !b) { print(\"yes\") } else { print(\"no\") }"
check("boolean ops", t13_src, ["yes"])

// Test 14: Comparison operators
var t14_src = "print(3 < 5)\nprint(5 > 3)\nprint(3 <= 3)\nprint(3 >= 4)"
check("comparisons", t14_src, ["true", "true", "true", "false"])

// Test 15: Arrays
var t15_src = "var arr = [1, 2, 3, 4, 5]\nprint(arr[2])\nprint(len(arr))"
check("arrays", t15_src, ["3", "5"])

// Test 16: For-in with range
var t16_src = "var total = 0\nfor i in range(5) {\ntotal = total + i\n}\nprint(total)"
check("for-in range", t16_src, ["10"])

// Test 17: For-in with array
var t17_src = "var total = 0\nfor x in [10, 20, 30] { total = total + x }\nprint(total)"
check("for-in array", t17_src, ["60"])

// Test 18: str() builtin
var t18_src = "var x = 42\nprint(\"val=\" + str(x))"
check("str builtin", t18_src, ["val=42"])

// Test 19: Nested if
var t19_src = "var x = 15\nif (x > 10) {\nif (x > 20) { print(\"huge\") } else { print(\"medium\") }\n} else { print(\"small\") }"
check("nested if", t19_src, ["medium"])

// Test 20: GCD via recursion
var t20_src = "func gcd(a, b) {\nif (b == 0) { return a }\nreturn gcd(b, a % b)\n}\nprint(gcd(48, 18))\nprint(gcd(100, 75))"
check("GCD recursive", t20_src, ["6", "25"])

// Test 21: Power function
var t21_src = "func power(base, exp) {\nif (exp == 0) { return 1 }\nreturn base * power(base, exp - 1)\n}\nprint(power(2, 10))\nprint(power(3, 5))"
check("power function", t21_src, ["1024", "243"])

// Test 22: Prime count
var t22_src = "func is_prime(n) {\nif (n < 2) { return false }\nvar i = 2\nwhile (i * i <= n) {\nif (n % i == 0) { return false }\ni = i + 1\n}\nreturn true\n}\nvar count = 0\nvar k = 2\nwhile (k <= 30) {\nif (is_prime(k)) { count = count + 1 }\nk = k + 1\n}\nprint(count)"
check("prime count", t22_src, ["10"])

// Test 23: FizzBuzz
var t23_src = "func fizzbuzz(n) {\nvar i = 1\nwhile (i <= n) {\nif (i % 15 == 0) { print(\"FizzBuzz\") }\nelse if (i % 3 == 0) { print(\"Fizz\") }\nelse if (i % 5 == 0) { print(\"Buzz\") }\nelse { print(i) }\ni = i + 1\n}\n}\nfizzbuzz(15)"
var t23_exp = ["1", "2", "Fizz", "4", "Buzz", "Fizz", "7", "8", "Fizz", "Buzz", "11", "Fizz", "13", "14", "FizzBuzz"]
check("FizzBuzz", t23_src, t23_exp)

// Test 24: Sum of squares
var t24_src = "func square(x) { return x * x }\nfunc sum_sq(n) {\nvar total = 0\nvar i = 1\nwhile (i <= n) {\ntotal = total + square(i)\ni = i + 1\n}\nreturn total\n}\nprint(sum_sq(10))"
check("sum of squares", t24_src, ["385"])

// Test 25: Boolean short-circuit - OR
var t25_src = "var x = 0\nif (true || false) { x = 1 }\nprint(x)\nif (false || false) { x = 2 }\nprint(x)"
check("short-circuit OR", t25_src, ["1", "1"])

// Test 26: Unary negation
var t26_src = "var x = 5\nprint(0 - x)\nprint(-3 + 10)"
check("unary negation", t26_src, ["-5", "7"])

// Test 27: Null handling
var t27_src = "var x = null\nif (x == null) { print(\"is null\") } else { print(\"not null\") }"
check("null handling", t27_src, ["is null"])

// Summary
print("")
print("=== Results: " + str(passed_tests) + "/" + str(total_tests) + " tests passed ===")
if (passed_tests == total_tests) {
    print("ALL TESTS PASSED - Self-hosting bytecode compiler works!")
} else {
    print("SOME TESTS FAILED")
}
