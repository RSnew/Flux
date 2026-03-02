#pragma once
#include <string>

enum class TokenType {
    // 字面量
    NUMBER, STRING, IDENTIFIER,
    // 关键字
    LET, VAR, FN, FUNC, RETURN, IF, ELSE, WHILE, TRUE, FALSE, NIL,
    FOR, IN,
    PERSISTENT, STATE, MODULE, MIGRATE,
    SUPERVISED, AT,
    DOT_ALWAYS, DOT_NEVER,
    // 并发关键字 (Feature K)
    ASYNC, AWAIT, SPAWN,
    // 线程池注解 (Feature K v2)
    THREADPOOL, CONCURRENT,
    DOT_BLOCK, DOT_DROP, DOT_ERROR,
    // 新关键字 (Spec v1.0)
    EXCEPTION,      // exception error-description keyword
    // 运算符
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, NEQ, LT, GT, LEQ, GEQ,
    AND, OR, NOT,
    QUESTION_QUESTION, // ?? nil-coalescing
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
