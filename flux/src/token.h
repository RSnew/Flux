#pragma once
#include <string>

enum class TokenType {
    // 字面量
    NUMBER, STRING, IDENTIFIER,
    // 关键字
    VAR, CONF, FUNC, RETURN, IF, ELSE, WHILE, TRUE, FALSE, NULL_TOKEN,
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
    INTERFACE,      // interface declaration keyword
    EXCEPTION,      // exception error-description keyword
    // Phase 5-7 新关键字
    PROFILE,        // @profile 性能分析装饰器
    PLATFORM,       // @platform 平台声明
    ENUM,           // enum 枚举关键字
    APPEND,         // append 扩展机制
    ALLOC,          // alloc 手动内存分配
    FREE,           // free 手动内存释放（已废弃，由 del 替代）
    DEL,            // del 统一删除/释放
    ASM,            // asm 内联汇编
    DEFAULT,        // default 错误恢复
    TEST,           // test 测试覆盖声明
    BENCH,          // bench 基准测试声明
    // 规格声明 (specify)
    SPECIFY,        // specify 类型声明
    REQUIRES,       // requires 前置条件（合约）
    ENSURES,        // ensures 后置条件（合约）
    // 运算符
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, NEQ, LT, GT, LEQ, GEQ,
    AND, OR, NOT,
    // QUESTION_QUESTION removed — ?? syntax removed from Flux v1.0
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
