#pragma once
#include <string>

enum class TokenType {
    // 字面量
    NUMBER, STRING, IDENTIFIER,
    // 关键字
    LET, VAR, FN, RETURN, IF, ELSE, WHILE, TRUE, FALSE, NIL,
    FOR, IN,
    PERSISTENT, STATE, MODULE, MIGRATE,
    SUPERVISED, AT,
    DOT_ALWAYS, DOT_NEVER,
    // 并发关键字 (Feature K)
    ASYNC, AWAIT, SPAWN,
    // 运算符
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, NEQ, LT, GT, LEQ, GEQ,
    AND, OR, NOT,
    // 赋值
    ASSIGN, PLUS_ASSIGN, MINUS_ASSIGN,
    // 分隔符
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    COMMA, COLON, ARROW, DOT,
    // 特殊
    NEWLINE, EOF_TOKEN
};

struct Token {
    TokenType   type;
    std::string value;
    int         line;

    Token(TokenType t, std::string v, int l)
        : type(t), value(std::move(v)), line(l) {}
};
