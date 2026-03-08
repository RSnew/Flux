#pragma once
#include "token.h"
#include "ast.h"
#include <vector>
#include <stdexcept>

class ParseError : public std::runtime_error {
public:
    int line;
    ParseError(const std::string& msg, int line)
        : std::runtime_error(msg), line(line) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    std::unique_ptr<Program> parse();

private:
    std::vector<Token> tokens_;
    size_t             pos_ = 0;

    Token&       current();
    Token&       peek(int offset = 1);
    Token        consume();
    Token        expect(TokenType type, const std::string& msg);
    bool         check(TokenType type) const;
    bool         match(TokenType type);
    void         skipNewlines();

    // 顶层
    NodePtr parseTopLevel();
    std::unique_ptr<FnDecl> parseFnDecl(bool forceOverride = false);
    NodePtr parsePersistentBlock();
    NodePtr parseMigrateBlock();
    NodePtr parseModuleDecl(RestartPolicy rp = RestartPolicy::None, int maxRetries = 3,
                             std::string poolName = "", int poolQueue = 100,
                             std::string poolOverflow = "block");
    NodePtr parseThreadPoolDecl();
    NodePtr parseExceptionDecl();   // exception 关键字
    // Phase 5-7 新语法
    NodePtr parseEnumDecl();        // enum 声明
    NodePtr parseAppendDecl();      // append 扩展
    NodePtr parseAsmBlock();        // asm {} 内联汇编
    NodePtr parsePlatformDecl();    // @platform 声明

    // 语句
    NodePtr      parseStatement();
    NodePtr      parseVarDecl(bool forceOverride = false);
    NodePtr      parseConfDecl();
    NodePtr      parseIf();
    NodePtr      parseWhile();
    NodePtr      parseForIn();
    NodePtr      parseReturn();
    NodePtr      parseSpawn();
    std::vector<NodePtr> parseBlock();

    // Spec v1.0 新语法
    NodePtr parseStructLit(std::string interfaceName = "");
    NodePtr parseInterfaceLit();

    // 表达式（递归下降，优先级从低到高）
    NodePtr parseExpr();
    NodePtr parseOr();
    NodePtr parseAnd();
    NodePtr parseEquality();
    NodePtr parseComparison();
    NodePtr parseAddSub();
    NodePtr parseMulDiv();
    NodePtr parseUnary();
    NodePtr parsePrimary();
};
