#pragma once
#include "token.h"
#include <vector>
#include <string>
#include <unordered_map>

class Lexer {
public:
    explicit Lexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    std::string source_;
    size_t      pos_  = 0;
    int         line_ = 1;

    char peek(int offset = 0) const;
    char advance();
    void skipWhitespace();
    void skipComment();
    Token readNumber();
    std::vector<Token> readStringWithInterp();
    Token readIdentifier();

    static const std::unordered_map<std::string, TokenType> keywords_;
};
