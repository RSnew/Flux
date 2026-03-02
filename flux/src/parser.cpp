#include "parser.h"
#include <sstream>

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

Token& Parser::current() { return tokens_[pos_]; }
Token& Parser::peek(int offset) {
    size_t idx = pos_ + offset;
    return tokens_[idx < tokens_.size() ? idx : tokens_.size() - 1];
}
Token Parser::consume() { return tokens_[pos_++]; }

Token Parser::expect(TokenType type, const std::string& msg) {
    if (!check(type)) throw ParseError(msg + " (got '" + current().value + "')", current().line);
    return consume();
}

bool Parser::check(TokenType type) const { return tokens_[pos_].type == type; }
bool Parser::match(TokenType type) { if (check(type)) { consume(); return true; } return false; }
void Parser::skipNewlines() { while (check(TokenType::NEWLINE)) consume(); }

// ── 顶层 ──────────────────────────────────────────────────
std::unique_ptr<Program> Parser::parse() {
    auto prog = std::make_unique<Program>();
    skipNewlines();
    while (!check(TokenType::EOF_TOKEN)) {
        prog->statements.push_back(parseTopLevel());
        skipNewlines();
    }
    return prog;
}

// ── 解析 @threadpool(name: "io-pool", size: 4) ───────────
NodePtr Parser::parseThreadPoolDecl() {
    expect(TokenType::LPAREN, "expected '(' after @threadpool");
    std::string poolName;
    int         poolSize = 4;
    while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
        std::string key = expect(TokenType::IDENTIFIER, "expected key in @threadpool").value;
        expect(TokenType::COLON, "expected ':'");
        if (key == "name") {
            poolName = expect(TokenType::STRING, "expected string for pool name").value;
        } else if (key == "size") {
            poolSize = (int)std::stod(expect(TokenType::NUMBER, "expected number for pool size").value);
        } else {
            consume(); // 跳过未知键
        }
        if (!match(TokenType::COMMA)) break;
    }
    expect(TokenType::RPAREN, "expected ')'");
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<ThreadPoolDecl>(poolName, poolSize);
}

NodePtr Parser::parseTopLevel() {
    // @annotation 处理
    if (check(TokenType::AT)) {
        consume(); // @

        // @threadpool(name: "io-pool", size: 4)
        if (check(TokenType::THREADPOOL)) {
            consume();
            return parseThreadPoolDecl();
        }

        // @concurrent(pool: "io-pool", queue: 100, overflow: .block)
        if (check(TokenType::CONCURRENT)) {
            consume();
            std::string poolName;
            int         poolQueue    = 100;
            std::string poolOverflow = "block";

            if (match(TokenType::LPAREN)) {
                while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
                    std::string key = expect(TokenType::IDENTIFIER, "expected key in @concurrent").value;
                    expect(TokenType::COLON, "expected ':'");
                    if (key == "pool") {
                        poolName = expect(TokenType::STRING, "expected string for pool").value;
                    } else if (key == "queue") {
                        poolQueue = (int)std::stod(expect(TokenType::NUMBER, "expected number").value);
                    } else if (key == "overflow") {
                        if      (check(TokenType::DOT_BLOCK)) { consume(); poolOverflow = "block"; }
                        else if (check(TokenType::DOT_DROP))  { consume(); poolOverflow = "drop";  }
                        else if (check(TokenType::DOT_ERROR)) { consume(); poolOverflow = "error"; }
                        else throw ParseError("expected .block, .drop, or .error", current().line);
                    } else {
                        consume();
                    }
                    if (!match(TokenType::COMMA)) break;
                }
                expect(TokenType::RPAREN, "expected ')'");
            }
            skipNewlines();

            if (!check(TokenType::MODULE))
                throw ParseError("@concurrent must be followed by module declaration", current().line);
            return parseModuleDecl(RestartPolicy::None, 3, poolName, poolQueue, poolOverflow);
        }

        // @supervised(restart: .always, maxRetries: 3)
        if (check(TokenType::SUPERVISED)) {
            consume();
            RestartPolicy rp = RestartPolicy::Always;
            int maxRetries = 3;

            if (match(TokenType::LPAREN)) {
                while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
                    std::string key = expect(TokenType::IDENTIFIER, "expected key").value;
                    expect(TokenType::COLON, "expected ':'");
                    if (key == "restart") {
                        if      (check(TokenType::DOT_ALWAYS)) { consume(); rp = RestartPolicy::Always; }
                        else if (check(TokenType::DOT_NEVER))  { consume(); rp = RestartPolicy::Never; }
                        else throw ParseError("expected .always or .never", current().line);
                    } else if (key == "maxRetries") {
                        maxRetries = (int)std::stod(expect(TokenType::NUMBER, "expected number").value);
                    } else {
                        consume();
                    }
                    if (!match(TokenType::COMMA)) break;
                }
                expect(TokenType::RPAREN, "expected ')'");
            }
            skipNewlines();

            if (!check(TokenType::MODULE))
                throw ParseError("@supervised must be followed by module declaration", current().line);
            return parseModuleDecl(rp, maxRetries);
        }

        throw ParseError("unknown annotation (expected supervised, concurrent, or threadpool)", current().line);
    }

    if (check(TokenType::FN))         return parseFnDecl();
    if (check(TokenType::LET))        return parseVarDecl(true);
    if (check(TokenType::VAR))        return parseVarDecl(false);
    if (check(TokenType::PERSISTENT)) return parsePersistentBlock();
    if (check(TokenType::MODULE))     return parseModuleDecl();
    return parseStatement();
}

// migrate { version: 1, errors: 0 }
NodePtr Parser::parseMigrateBlock() {
    expect(TokenType::MIGRATE, "expected 'migrate'");
    skipNewlines();
    expect(TokenType::LBRACE, "expected '{' after migrate");
    skipNewlines();

    auto block = std::make_unique<MigrateBlock>();

    while (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        MigrateField field;
        field.name  = expect(TokenType::IDENTIFIER, "expected field name").value;
        expect(TokenType::COLON, "expected ':' after field name");
        field.value = parseExpr();
        block->fields.push_back(std::move(field));
        match(TokenType::COMMA);
        while (match(TokenType::NEWLINE)) {}
    }

    expect(TokenType::RBRACE, "expected '}' to close migrate block");
    while (match(TokenType::NEWLINE)) {}
    return block;
}

NodePtr Parser::parseModuleDecl(RestartPolicy rp, int maxRetries,
                                  std::string poolName, int poolQueue,
                                  std::string poolOverflow) {
    expect(TokenType::MODULE, "expected 'module'");
    std::string name = expect(TokenType::IDENTIFIER, "expected module name").value;
    skipNewlines();
    expect(TokenType::LBRACE, "expected '{' after module name");
    skipNewlines();

    std::vector<NodePtr> body;
    while (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        if      (check(TokenType::FN))         body.push_back(parseFnDecl());
        else if (check(TokenType::PERSISTENT)) body.push_back(parsePersistentBlock());
        else if (check(TokenType::MIGRATE))    body.push_back(parseMigrateBlock());
        else if (check(TokenType::LET))        body.push_back(parseVarDecl(true));
        else if (check(TokenType::VAR))        body.push_back(parseVarDecl(false));
        else                                   body.push_back(parseStatement());
        skipNewlines();
    }
    expect(TokenType::RBRACE, "expected '}' to close module");
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<ModuleDecl>(name, std::move(body), rp, maxRetries,
                                        poolName, poolQueue, poolOverflow);
}

// persistent { visits: 0, errors: 0 }
NodePtr Parser::parsePersistentBlock() {
    expect(TokenType::PERSISTENT, "expected 'persistent'");
    skipNewlines();
    expect(TokenType::LBRACE, "expected '{' after persistent");
    skipNewlines();

    auto block = std::make_unique<PersistentBlock>();

    while (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        PersistentField field;
        field.name = expect(TokenType::IDENTIFIER, "expected field name").value;
        expect(TokenType::COLON, "expected ':' after field name");
        field.defaultValue = parseExpr();
        block->fields.push_back(std::move(field));
        // 可选逗号或换行
        match(TokenType::COMMA);
        while (match(TokenType::NEWLINE)) {}
    }

    expect(TokenType::RBRACE, "expected '}' to close persistent block");
    while (match(TokenType::NEWLINE)) {}
    return block;
}

std::unique_ptr<FnDecl> Parser::parseFnDecl() {
    expect(TokenType::FN, "expected 'fn'");
    std::string name = expect(TokenType::IDENTIFIER, "expected function name").value;
    expect(TokenType::LPAREN, "expected '('");

    std::vector<Param> params;
    while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
        Param p;
        p.name = expect(TokenType::IDENTIFIER, "expected parameter name").value;
        if (match(TokenType::COLON))
            p.type = expect(TokenType::IDENTIFIER, "expected type").value;
        params.push_back(std::move(p));
        if (!match(TokenType::COMMA)) break;
    }
    expect(TokenType::RPAREN, "expected ')'");

    std::string retType;
    if (match(TokenType::ARROW))
        retType = expect(TokenType::IDENTIFIER, "expected return type").value;

    auto body = parseBlock();
    return std::make_unique<FnDecl>(name, std::move(params), retType, std::move(body));
}

// ── 语句 ──────────────────────────────────────────────────
NodePtr Parser::parseStatement() {
    skipNewlines();
    if (check(TokenType::LET))    return parseVarDecl(true);
    if (check(TokenType::VAR))    return parseVarDecl(false);
    if (check(TokenType::IF))     return parseIf();
    if (check(TokenType::WHILE))  return parseWhile();
    if (check(TokenType::FOR))    return parseForIn();
    if (check(TokenType::RETURN)) return parseReturn();
    if (check(TokenType::SPAWN))  return parseSpawn();

    // 赋值 or 表达式语句
    auto expr = parseExpr();

    // 检查是否是赋值：x = ... 或 state.field = ... 或 arr[i] = ...
    if (check(TokenType::ASSIGN)) {
        consume(); // =
        auto val = parseExpr();

        if (auto* sa = dynamic_cast<StateAccess*>(expr.get())) {
            std::string field = sa->field;
            expr = std::make_unique<StateAssign>(field, std::move(val));
        } else if (auto* ie = dynamic_cast<IndexExpr*>(expr.get())) {
            auto obj = std::move(ie->object);
            auto idx = std::move(ie->index);
            expr = std::make_unique<IndexAssign>(std::move(obj), std::move(idx), std::move(val));
        } else if (auto* id = dynamic_cast<Identifier*>(expr.get())) {
            expr = std::make_unique<Assign>(id->name, std::move(val));
        } else {
            throw ParseError("invalid assignment target", current().line);
        }
    }

    // 跳过换行
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<ExprStmt>(std::move(expr));
}

NodePtr Parser::parseVarDecl(bool immutable) {
    consume(); // let / var
    std::string name = expect(TokenType::IDENTIFIER, "expected variable name").value;
    std::string typeAnnotation;
    if (match(TokenType::COLON)) {
        typeAnnotation = expect(TokenType::IDENTIFIER, "expected type").value;
    }
    expect(TokenType::ASSIGN, "expected '=' in variable declaration");
    auto init = parseExpr();
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<VarDecl>(immutable, name, typeAnnotation, std::move(init));
}

NodePtr Parser::parseIf() {
    consume(); // if
    auto cond = parseExpr();
    auto thenBlock = parseBlock();
    std::vector<NodePtr> elseBlock;
    skipNewlines();
    if (match(TokenType::ELSE)) elseBlock = parseBlock();
    return std::make_unique<IfStmt>(std::move(cond), std::move(thenBlock), std::move(elseBlock));
}

NodePtr Parser::parseWhile() {
    consume(); // while
    auto cond = parseExpr();
    auto body = parseBlock();
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
}

NodePtr Parser::parseForIn() {
    consume(); // for
    std::string var = expect(TokenType::IDENTIFIER, "expected variable name").value;
    expect(TokenType::IN, "expected 'in' after variable");
    auto iterable = parseExpr();
    auto body     = parseBlock();
    return std::make_unique<ForIn>(var, std::move(iterable), std::move(body));
}

NodePtr Parser::parseReturn() {
    consume(); // return
    if (check(TokenType::NEWLINE) || check(TokenType::RBRACE)) {
        while (match(TokenType::NEWLINE)) {}
        return std::make_unique<ReturnStmt>(nullptr);
    }
    auto val = parseExpr();
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<ReturnStmt>(std::move(val));
}

std::vector<NodePtr> Parser::parseBlock() {
    skipNewlines();
    expect(TokenType::LBRACE, "expected '{'");
    skipNewlines();
    std::vector<NodePtr> stmts;
    while (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN)) {
        stmts.push_back(parseStatement());
        skipNewlines();
    }
    expect(TokenType::RBRACE, "expected '}'");
    return stmts;
}

// spawn { ... } — 启动独立的并发任务（fire-and-forget）
NodePtr Parser::parseSpawn() {
    consume(); // spawn
    auto body = parseBlock();
    while (match(TokenType::NEWLINE)) {}
    return std::make_unique<SpawnStmt>(std::move(body));
}

// ── 表达式（递归下降）─────────────────────────────────────
NodePtr Parser::parseExpr()       { return parseOr(); }

NodePtr Parser::parseOr() {
    auto left = parseAnd();
    while (check(TokenType::OR)) {
        consume();
        left = std::make_unique<BinaryExpr>("||", std::move(left), parseAnd());
    }
    return left;
}

NodePtr Parser::parseAnd() {
    auto left = parseEquality();
    while (check(TokenType::AND)) {
        consume();
        left = std::make_unique<BinaryExpr>("&&", std::move(left), parseEquality());
    }
    return left;
}

NodePtr Parser::parseEquality() {
    auto left = parseComparison();
    while (check(TokenType::EQ) || check(TokenType::NEQ)) {
        std::string op = consume().value;
        left = std::make_unique<BinaryExpr>(op, std::move(left), parseComparison()); // op is now "==" or "!=" directly
    }
    return left;
}

NodePtr Parser::parseComparison() {
    auto left = parseAddSub();
    while (check(TokenType::LT) || check(TokenType::GT) ||
           check(TokenType::LEQ) || check(TokenType::GEQ)) {
        std::string op = consume().value;
        left = std::make_unique<BinaryExpr>(op, std::move(left), parseAddSub());
    }
    return left;
}

NodePtr Parser::parseAddSub() {
    auto left = parseMulDiv();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        std::string op = consume().value;
        left = std::make_unique<BinaryExpr>(op, std::move(left), parseMulDiv());
    }
    return left;
}

NodePtr Parser::parseMulDiv() {
    auto left = parseUnary();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        std::string op = consume().value;
        left = std::make_unique<BinaryExpr>(op, std::move(left), parseUnary());
    }
    return left;
}

NodePtr Parser::parseUnary() {
    if (check(TokenType::NOT))   { consume(); return std::make_unique<UnaryExpr>("!", parseUnary()); }
    if (check(TokenType::MINUS)) { consume(); return std::make_unique<UnaryExpr>("-", parseUnary()); }
    // async <call> — 异步执行函数调用，返回 Future 值
    if (check(TokenType::ASYNC)) {
        consume();
        auto call = parsePrimary();   // 解析被调用的函数（CallExpr 或 ModuleCall）
        return std::make_unique<AsyncExpr>(std::move(call));
    }
    // await <expr> — 等待 Future，返回其结果值
    if (check(TokenType::AWAIT)) {
        consume();
        auto expr = parseUnary();
        return std::make_unique<AwaitExpr>(std::move(expr));
    }
    return parsePrimary();
}

NodePtr Parser::parsePrimary() {
    // 数字
    if (check(TokenType::NUMBER)) {
        double v = std::stod(current().value);
        consume();
        return std::make_unique<NumberLit>(v);
    }
    // 字符串
    if (check(TokenType::STRING)) {
        std::string v = current().value;
        consume();
        return std::make_unique<StringLit>(v);
    }
    // nil 字面量
    if (check(TokenType::NIL))   { consume(); return std::make_unique<NilLit>(); }
    // bool
    if (check(TokenType::TRUE))  { consume(); return std::make_unique<BoolLit>(true); }
    if (check(TokenType::FALSE)) { consume(); return std::make_unique<BoolLit>(false); }

    // 数组字面量 [1, 2, 3]
    if (check(TokenType::LBRACKET)) {
        consume(); // [
        std::vector<NodePtr> elems;
        skipNewlines();
        while (!check(TokenType::RBRACKET) && !check(TokenType::EOF_TOKEN)) {
            elems.push_back(parseExpr());
            skipNewlines();
            if (!match(TokenType::COMMA)) break;
            skipNewlines();
        }
        expect(TokenType::RBRACKET, "expected ']'");
        NodePtr arr = std::make_unique<ArrayLit>(std::move(elems));
        // 支持立即访问 [1,2,3][0]
        while (check(TokenType::LBRACKET)) {
            consume();
            auto idx = parseExpr();
            expect(TokenType::RBRACKET, "expected ']'");
            arr = std::make_unique<IndexExpr>(std::move(arr), std::move(idx));
        }
        return arr;
    }

    // 标识符 or 函数调用 or state.field or Module.fn() or arr[i] or arr.method()
    if (check(TokenType::IDENTIFIER) || check(TokenType::STATE)) {
        std::string name = consume().value;

        // state.field 访问
        if (name == "state" && check(TokenType::DOT)) {
            consume(); // .
            std::string field = expect(TokenType::IDENTIFIER, "expected field name after 'state.'").value;
            return std::make_unique<StateAccess>(field);
        }

        NodePtr node;

        // 函数调用
        if (match(TokenType::LPAREN)) {
            std::vector<NodePtr> args;
            while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
                args.push_back(parseExpr());
                if (!match(TokenType::COMMA)) break;
            }
            expect(TokenType::RPAREN, "expected ')'");
            node = std::make_unique<CallExpr>(name, std::move(args));
        } else {
            node = std::make_unique<Identifier>(name);
        }

        // 后缀链：arr[i]、arr.method()、Module.fn()
        while (true) {
            if (check(TokenType::LBRACKET)) {
                // arr[i] 下标访问
                consume();
                auto idx = parseExpr();
                expect(TokenType::RBRACKET, "expected ']'");
                node = std::make_unique<IndexExpr>(std::move(node), std::move(idx));
            } else if (check(TokenType::DOT)) {
                consume(); // .
                // DOT 后可跟标识符或部分关键字（async/await）作为成员名
                std::string member;
                if      (check(TokenType::IDENTIFIER)) member = consume().value;
                else if (check(TokenType::ASYNC))      { consume(); member = "async"; }
                else if (check(TokenType::AWAIT))      { consume(); member = "await"; }
                else throw ParseError("expected member name after '.'", current().line);

                if (check(TokenType::LPAREN)) {
                    // .method(...) 调用
                    consume();
                    std::vector<NodePtr> args;
                    while (!check(TokenType::RPAREN) && !check(TokenType::EOF_TOKEN)) {
                        args.push_back(parseExpr());
                        if (!match(TokenType::COMMA)) break;
                    }
                    expect(TokenType::RPAREN, "expected ')'");

                    if (member == "async") {
                        // Module.fn.async(args) → AsyncCall（跨 pool 调用）
                        auto* mc = dynamic_cast<ModuleCall*>(node.get());
                        if (mc && mc->args.empty()) {
                            node = std::make_unique<AsyncCall>(mc->module, mc->fn, std::move(args));
                        } else {
                            throw ParseError(".async() must follow a Module.fn reference (no preceding args)", current().line);
                        }
                    } else if (auto* id = dynamic_cast<Identifier*>(node.get())) {
                        // 判断是否是 Module.fn（对象是纯标识符）
                        node = std::make_unique<ModuleCall>(id->name, member, std::move(args));
                    } else {
                        node = std::make_unique<MethodCall>(std::move(node), member, std::move(args));
                    }
                } else {
                    // 无括号：字段访问 / Module.fn 函数引用（为后续 .async() 做准备）
                    if (auto* id = dynamic_cast<Identifier*>(node.get())) {
                        // Module.fn → 暂存为零参 ModuleCall，后续 .async() 会改写
                        node = std::make_unique<ModuleCall>(id->name, member, std::vector<NodePtr>{});
                    }
                }
            } else {
                break;
            }
        }
        return node;
    }

    // 括号表达式
    if (match(TokenType::LPAREN)) {
        auto expr = parseExpr();
        expect(TokenType::RPAREN, "expected ')'");
        return expr;
    }

    throw ParseError("unexpected token '" + current().value + "'", current().line);
}
