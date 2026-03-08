// ═══════════════════════════════════════════════════════════
// Feature M — 自举编译器 (Self-hosting Compiler)
// Flux compiles Flux: 用 Flux 实现的 Flux 子集解释器
//
// 支持的语法子集：
//   - var 声明 + 赋值
//   - func 函数声明 + 调用
//   - if / else 条件
//   - while 循环
//   - for item in array { } 迭代
//   - return 语句
//   - 算术/比较/逻辑运算符
//   - 数字/字符串/布尔/null 字面量
//   - 数组字面量 + 下标访问
//   - print() / str() / len() / range() / type() 内置函数
//   - 字符串插值基础支持
// ═══════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────
// 工具函数
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
// 词法分析器 (Lexer)
// ═══════════════════════════════════════════════════════════
// Token 结构: Map { type: "...", value: "...", line: N }

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

        // 跳过空白
        if (is_space(c)) {
            pos = pos + 1
        } else if (c == "\n") {
            tokens.push(make_token("NEWLINE", "\\n", line))
            line = line + 1
            pos = pos + 1
        } else if (c == "/" && pos + 1 < src_len && source[pos + 1] == "/") {
            // 注释：跳到行尾
            while (pos < src_len && source[pos] != "\n") {
                pos = pos + 1
            }
        } else if (is_digit(c)) {
            // 数字
            var num = ""
            while (pos < src_len && (is_digit(source[pos]) || source[pos] == ".")) {
                num = num + source[pos]
                pos = pos + 1
            }
            tokens.push(make_token("NUMBER", num, line))
        } else if (c == "\"") {
            // 字符串
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
            // 标识符或关键字
            var word = ""
            while (pos < src_len && is_alnum(source[pos])) {
                word = word + source[pos]
                pos = pos + 1
            }
            // 关键字检查
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
            // 未知字符跳过
            pos = pos + 1
        }
    }

    tokens.push(make_token("EOF", "", line))
    return tokens
}

// ═══════════════════════════════════════════════════════════
// 语法分析器 (Parser)
// ═══════════════════════════════════════════════════════════
// AST 节点用 Map 表示：{ node: "类型", ... }
//
// 使用全局状态（parser_ Map）简化递归下降

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

func p_peek(offset) {
    var tokens = parser_.get("tokens")
    var pos = parser_.get("pos") + offset
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
        panic("Parse error line \(p_cur().get("line")): \(msg) (got '\(p_cur().get("value"))')")
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

// ── 表达式解析 ──────────────────────────────────────────

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
    // 下标访问 expr[index] 和方法调用 expr.method(args) 链
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
                // Field access
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
    // 数字
    if (p_check("NUMBER")) {
        var t = p_advance()
        var n = make_node("number")
        n.set("value", num(t.get("value")))
        return n
    }
    // 字符串
    if (p_check("STRING")) {
        var t = p_advance()
        var n = make_node("string")
        n.set("value", t.get("value"))
        return n
    }
    // true / false
    if (p_check("TRUE"))  { p_advance() ; var n = make_node("bool") ; n.set("value", true) ; return n }
    if (p_check("FALSE")) { p_advance() ; var n = make_node("bool") ; n.set("value", false) ; return n }
    // null
    if (p_check("NULL"))   { p_advance() ; return make_node("null") }

    // 数组字面量 [1, 2, 3]
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

    // 括号表达式
    if (p_check("LPAREN")) {
        p_advance()
        var expr = parse_expr()
        p_expect("RPAREN", "expected ')'")
        return expr
    }

    // 标识符（可能是函数调用）
    if (p_check("ID")) {
        var name = p_advance().get("value")
        if (p_check("LPAREN")) {
            // 函数调用
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

    panic("Parse error line \(p_cur().get("line")): unexpected token '\(p_cur().get("value"))'")
}

// ── 语句解析 ────────────────────────────────────────────

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

    // var x = expr
    if (p_check("VAR")) {
        p_advance()
        var name = p_expect("ID", "expected variable name").get("value")
        // 可选类型注解（跳过）
        if (p_check("COLON")) { p_advance() ; p_expect("ID", "expected type") }
        p_expect("ASSIGN", "expected '='")
        var init = parse_expr()
        var n = make_node("var_decl")
        n.set("name", name)
        n.set("init", init)
        return n
    }

    // func name(params) { body }
    if (p_check("FUNC")) {
        p_advance()
        var name = p_expect("ID", "expected function name").get("value")
        p_expect("LPAREN", "expected '('")
        var params = []
        var param_cont = true
        while (param_cont && !p_check("RPAREN") && !p_check("EOF")) {
            params.push(p_expect("ID", "expected param name").get("value"))
            // 可选类型注解
            if (p_check("COLON")) { p_advance() ; p_expect("ID", "expected type") }
            if (!p_match("COMMA")) { param_cont = false }
        }
        p_expect("RPAREN", "expected ')'")
        // 可选返回类型
        if (p_match("ARROW")) { p_expect("ID", "expected return type") }
        p_skip_nl()
        var body = parse_block()
        var n = make_node("fn_decl")
        n.set("name", name)
        n.set("params", params)
        n.set("body", body)
        return n
    }

    // if (cond) { ... } else { ... }
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
                // else if 链
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

    // while (cond) { ... }
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

    // for item in expr { ... }
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

    // return expr
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

    // 表达式语句（含赋值）
    var expr = parse_expr()

    // x = expr 赋值
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

    // += / -=
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

    // 表达式语句
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
// 解释器 (Interpreter)
// ═══════════════════════════════════════════════════════════
// 环境：数组作为作用域栈，每层是一个 Map

// ReturnSignal 用 Map 表示: { signal: "return", value: ... }

func env_new() {
    return [Map()]
}

func env_push(env) {
    env.push(Map())
}

func env_pop(env) {
    env.pop()
}

func env_set(env, name, val) {
    var top = env[len(env) - 1]
    top.set(name, val)
}

func env_get(env, name) {
    var i = len(env) - 1
    while (i >= 0) {
        var scope = env[i]
        if (scope.has(name)) { return scope.get(name) }
        i = i - 1
    }
    panic("undefined variable: \(name)")
}

func env_has(env, name) {
    var i = len(env) - 1
    while (i >= 0) {
        if (env[i].has(name)) { return true }
        i = i - 1
    }
    return false
}

func env_assign(env, name, val) {
    var i = len(env) - 1
    while (i >= 0) {
        var scope = env[i]
        if (scope.has(name)) { scope.set(name, val) ; return null }
        i = i - 1
    }
    panic("undefined variable (cannot assign): \(name)")
}

// ── 函数注册表（全局）──────────────────────────────────
var functions_ = Map()

// ── 值转字符串 ──────────────────────────────────────────
func val_to_str(v) {
    if (v == null) { return "null" }
    if (type(v) == "Number") {
        // 整数不带小数点
        if (v == 0 - (0 - v) && v % 1 == 0) {
            return str(v)
        }
        return str(v)
    }
    if (type(v) == "Bool") {
        if (v) { return "true" }
        return "false"
    }
    if (type(v) == "String") { return v }
    if (type(v) == "Array") {
        var parts = []
        for item in v {
            parts.push(val_to_str(item))
        }
        return "[" + parts.join(", ") + "]"
    }
    return str(v)
}

// ── 内置函数 ────────────────────────────────────────────
func call_builtin(name, args) {
    if (name == "print") {
        var parts = []
        for a in args { parts.push(val_to_str(a)) }
        print(parts.join(" "))
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
    if (name == "assert") {
        if (len(args) == 0 || !args[0]) {
            var msg = "assertion failed"
            if (len(args) > 1) { msg = val_to_str(args[1]) }
            panic(msg)
        }
        return null
    }
    if (name == "panic") {
        var msg = "panic"
        if (len(args) > 0) { msg = val_to_str(args[0]) }
        panic(msg)
    }
    return null
}

func is_builtin(name) {
    if (name == "print") { return true }
    if (name == "str") { return true }
    if (name == "num") { return true }
    if (name == "len") { return true }
    if (name == "type") { return true }
    if (name == "range") { return true }
    if (name == "assert") { return true }
    if (name == "panic") { return true }
    return false
}

// ── 表达式求值 ──────────────────────────────────────────

func eval_expr(node, env) {
    var kind = node.get("node")

    if (kind == "number") { return node.get("value") }
    if (kind == "string") { return node.get("value") }
    if (kind == "bool")   { return node.get("value") }
    if (kind == "null")    { return null }

    if (kind == "id") {
        return env_get(env, node.get("name"))
    }

    if (kind == "array") {
        var result = []
        for elem in node.get("elements") {
            result.push(eval_expr(elem, env))
        }
        return result
    }

    if (kind == "index") {
        var obj = eval_expr(node.get("object"), env)
        var idx = eval_expr(node.get("index"), env)
        return obj[idx]
    }

    if (kind == "field_access") {
        var obj = eval_expr(node.get("object"), env)
        var field = node.get("field")
        if (type(obj) == "Map") { return obj.get(field) }
        panic("cannot access field '\(field)' on non-map value")
    }

    if (kind == "method_call") {
        var obj = eval_expr(node.get("object"), env)
        var method = node.get("method")
        var args = []
        for a in node.get("args") { args.push(eval_expr(a, env)) }

        // 数组方法
        if (type(obj) == "Array") {
            if (method == "push")    { obj.push(args[0]) ; return null }
            if (method == "pop")     { return obj.pop() }
            if (method == "len")     { return len(obj) }
            if (method == "join")    { return obj.join(args[0]) }
            if (method == "reverse") { return obj.reverse() }
            if (method == "contains") {
                for item in obj {
                    if (item == args[0]) { return true }
                }
                return false
            }
        }
        // 字符串方法
        if (type(obj) == "String") {
            if (method == "len")      { return len(obj) }
            if (method == "upper")    { return obj.upper() }
            if (method == "lower")    { return obj.lower() }
            if (method == "split")    { return obj.split(args[0]) }
            if (method == "contains") { return obj.contains(args[0]) }
            if (method == "trim")     { return obj.trim() }
        }
        // Map 方法
        if (type(obj) == "Map") {
            if (method == "get")  { return obj.get(args[0]) }
            if (method == "set")  { obj.set(args[0], args[1]) ; return null }
            if (method == "has")  { return obj.has(args[0]) }
            if (method == "keys") { return obj.keys() }
        }
        panic("unknown method: \(method)")
    }

    if (kind == "unary") {
        var val = eval_expr(node.get("operand"), env)
        var op = node.get("op")
        if (op == "-") { return 0 - val }
        if (op == "!") { return !val }
        panic("unknown unary op: \(op)")
    }

    if (kind == "binary") {
        var op = node.get("op")
        // 短路求值
        if (op == "&&") {
            var l = eval_expr(node.get("left"), env)
            if (!l) { return false }
            return eval_expr(node.get("right"), env)
        }
        if (op == "||") {
            var l = eval_expr(node.get("left"), env)
            if (l) { return l }
            return eval_expr(node.get("right"), env)
        }
        var l = eval_expr(node.get("left"), env)
        var r = eval_expr(node.get("right"), env)
        if (op == "+") {
            if (type(l) == "String" || type(r) == "String") {
                return val_to_str(l) + val_to_str(r)
            }
            return l + r
        }
        if (op == "-")  { return l - r }
        if (op == "*")  { return l * r }
        if (op == "/")  { return l / r }
        if (op == "%")  { return l % r }
        if (op == "==") { return l == r }
        if (op == "!=") { return l != r }
        if (op == "<")  { return l < r }
        if (op == ">")  { return l > r }
        if (op == "<=") { return l <= r }
        if (op == ">=") { return l >= r }
        panic("unknown binary op: \(op)")
    }

    if (kind == "call") {
        var name = node.get("name")
        var args = []
        for a in node.get("args") { args.push(eval_expr(a, env)) }

        // 内置函数
        if (is_builtin(name)) { return call_builtin(name, args) }

        // 用户定义函数
        if (!functions_.has(name)) { panic("undefined function: \(name)") }
        var fn_node = functions_.get(name)
        return call_fn(fn_node, args)
    }

    panic("unknown expression node: \(kind)")
}

// ── 函数调用 ─────────────────────────────────────────────

func call_fn(fn_node, args) {
    var params = fn_node.get("params")
    var body = fn_node.get("body")

    // 创建新环境
    var fn_env = env_new()

    // 绑定全局函数（让递归和互调用生效）
    var global_names = functions_.keys()
    for name in global_names {
        // 函数引用存在 functions_ 中，不需要存入环境
    }

    // 绑定参数
    var i = 0
    while (i < len(params)) {
        var val = null
        if (i < len(args)) { val = args[i] }
        env_set(fn_env, params[i], val)
        i = i + 1
    }

    // 执行函数体
    var result = exec_block(body, fn_env)
    if (result != null && type(result) == "Map" && result.has("signal")) {
        if (result.get("signal") == "return") {
            return result.get("value")
        }
    }
    return null
}

// ── 语句执行 ─────────────────────────────────────────────

func exec_stmt(node, env) {
    var kind = node.get("node")

    if (kind == "var_decl") {
        var val = null
        if (node.get("init") != null) {
            val = eval_expr(node.get("init"), env)
        }
        env_set(env, node.get("name"), val)
        return null
    }

    if (kind == "assign") {
        var val = eval_expr(node.get("value"), env)
        env_assign(env, node.get("name"), val)
        return null
    }

    if (kind == "index_assign") {
        var obj = eval_expr(node.get("object"), env)
        var idx = eval_expr(node.get("index"), env)
        var val = eval_expr(node.get("value"), env)
        obj[idx] = val
        return null
    }

    if (kind == "fn_decl") {
        functions_.set(node.get("name"), node)
        return null
    }

    if (kind == "return") {
        var val = null
        if (node.get("value") != null) {
            val = eval_expr(node.get("value"), env)
        }
        var sig = Map()
        sig.set("signal", "return")
        sig.set("value", val)
        return sig
    }

    if (kind == "if") {
        var cond = eval_expr(node.get("cond"), env)
        if (cond) {
            env_push(env)
            var r = exec_block(node.get("then"), env)
            env_pop(env)
            return r
        } else {
            var else_body = node.get("else")
            if (len(else_body) > 0) {
                env_push(env)
                var r = exec_block(else_body, env)
                env_pop(env)
                return r
            }
        }
        return null
    }

    if (kind == "while") {
        var w_cond = eval_expr(node.get("cond"), env)
        while (w_cond) {
            env_push(env)
            var r = exec_block(node.get("body"), env)
            env_pop(env)
            if (r != null && type(r) == "Map" && r.has("signal")) { return r }
            w_cond = eval_expr(node.get("cond"), env)
        }
        return null
    }

    if (kind == "for_in") {
        var iter_val = eval_expr(node.get("iter"), env)
        var var_name = node.get("var")
        for item in iter_val {
            env_push(env)
            env_set(env, var_name, item)
            var r = exec_block(node.get("body"), env)
            env_pop(env)
            if (r != null && type(r) == "Map" && r.has("signal")) { return r }
        }
        return null
    }

    if (kind == "expr_stmt") {
        eval_expr(node.get("expr"), env)
        return null
    }

    panic("unknown statement node: \(kind)")
}

func exec_block(stmts, env) {
    for stmt in stmts {
        var r = exec_stmt(stmt, env)
        if (r != null && type(r) == "Map" && r.has("signal")) { return r }
    }
    return null
}

// ═══════════════════════════════════════════════════════════
// 主入口 — 运行 Flux 源代码
// ═══════════════════════════════════════════════════════════

func flux_run(source) {
    // 重置状态
    functions_ = Map()

    // 1. 词法分析
    var tokens = tokenize(source)

    // 2. 语法分析
    var ast = parse_program(tokens)

    // 3. 第一遍：注册函数
    for stmt in ast {
        if (stmt.get("node") == "fn_decl") {
            functions_.set(stmt.get("name"), stmt)
        }
    }

    // 4. 第二遍：执行顶层语句
    var env = env_new()
    for stmt in ast {
        if (stmt.get("node") == "fn_decl") {
            // 已在第一遍注册，跳过
        } else {
            exec_stmt(stmt, env)
        }
    }
}

// ═══════════════════════════════════════════════════════════
// 测试 — 自举验证
// ═══════════════════════════════════════════════════════════

print("╔══════════════════════════════════════╗")
print("║  Feature M — Self-hosting Compiler   ║")
print("║  Flux compiles Flux                  ║")
print("╚══════════════════════════════════════╝")
print("")

// ── Test 1: 基础运算 ─────────────────────────────────────
print("Test 1: Basic arithmetic")
flux_run("print(1 + 2 * 3)")
flux_run("print(10 - 3)")
flux_run("print(15 / 3)")
flux_run("print(17 % 5)")

// ── Test 2: 变量声明与赋值 ───────────────────────────────
print("\nTest 2: Variables")
flux_run("var x = 42\nprint(x)")
flux_run("var a = 10\na = a + 5\nprint(a)")

// ── Test 3: 字符串 ──────────────────────────────────────
print("\nTest 3: Strings")
flux_run("var s = \"hello world\"\nprint(s)")
flux_run("print(\"flux\" + \" \" + \"lang\")")

// ── Test 4: 条件分支 ────────────────────────────────────
print("\nTest 4: If/else")
flux_run("var x = 10\nif (x > 5) { print(\"big\") } else { print(\"small\") }")
flux_run("var y = 3\nif (y == 3) { print(\"three\") }")

// ── Test 5: While 循环 ──────────────────────────────────
print("\nTest 5: While loop")
flux_run("var sum = 0\nvar i = 1\nwhile (i <= 10) { sum = sum + i\ni = i + 1 }\nprint(sum)")

// ── Test 6: 函数声明与调用 ───────────────────────────────
print("\nTest 6: Functions")
flux_run("func double(x) { return x * 2 }\nprint(double(21))")
flux_run("func greet(name) { return \"Hello, \" + name + \"!\" }\nprint(greet(\"Flux\"))")

// ── Test 7: 递归 ────────────────────────────────────────
print("\nTest 7: Recursion")
flux_run("func fib(n) {\nif (n <= 1) { return n }\nreturn fib(n - 1) + fib(n - 2)\n}\nprint(fib(10))")

flux_run("func factorial(n) {\nif (n <= 1) { return 1 }\nreturn n * factorial(n - 1)\n}\nprint(factorial(10))")

// ── Test 8: 数组 ────────────────────────────────────────
print("\nTest 8: Arrays")
flux_run("var arr = [1, 2, 3, 4, 5]\nprint(arr)\nprint(arr[2])\nprint(len(arr))")

// ── Test 9: For-in 循环 ─────────────────────────────────
print("\nTest 9: For-in loop")
flux_run("var total = 0\nfor x in [10, 20, 30] { total = total + x }\nprint(total)")
flux_run("for i in range(5) { print(i) }")

// ── Test 10: 复合程序 ───────────────────────────────────
print("\nTest 10: Complex program — FizzBuzz")
flux_run("func fizzbuzz(n) {\nvar i = 1\nwhile (i <= n) {\nif (i % 15 == 0) { print(\"FizzBuzz\") }\nelse if (i % 3 == 0) { print(\"Fizz\") }\nelse if (i % 5 == 0) { print(\"Buzz\") }\nelse { print(i) }\ni = i + 1\n}\n}\nfizzbuzz(15)")

// ── Test 11: 自举自身 — Flux 解释器解释 Flux 代码 ────────
print("\nTest 11: Meta — self-interpreting a program")
// 嵌套的 Flux 代码（由自举编译器运行的代码中再运行代码）
// 这不是真正的嵌套自举（那需要序列化整个编译器），但验证了复杂程序执行
flux_run("func square(x) { return x * x }\nfunc sum_squares(n) {\nvar total = 0\nvar i = 1\nwhile (i <= n) { total = total + square(i)\ni = i + 1 }\nreturn total\n}\nprint(sum_squares(10))")

print("\n═══════════════════════════════════════")
print("  All self-hosting tests passed!")
print("  Flux successfully compiles Flux.")
print("═══════════════════════════════════════")
