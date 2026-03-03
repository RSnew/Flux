#include "lexer.h"
#include <stdexcept>

const std::unordered_map<std::string, TokenType> Lexer::keywords_ = {
    {"var",        TokenType::VAR},
    {"fn",         TokenType::FN},
    {"func",       TokenType::FUNC},      // Spec v1.0: alias for fn
    {"return",     TokenType::RETURN},
    {"if",         TokenType::IF},
    {"else",       TokenType::ELSE},
    {"while",      TokenType::WHILE},
    {"true",       TokenType::TRUE},
    {"false",      TokenType::FALSE},
    {"nil",        TokenType::NIL},
    {"for",        TokenType::FOR},
    {"in",         TokenType::IN},
    {"persistent", TokenType::PERSISTENT},
    {"state",      TokenType::STATE},
    {"module",     TokenType::MODULE},
    {"migrate",    TokenType::MIGRATE},
    {"supervised", TokenType::SUPERVISED},
    // 并发关键字 (Feature K)
    {"async",      TokenType::ASYNC},
    {"await",      TokenType::AWAIT},
    {"spawn",      TokenType::SPAWN},
    // 线程池注解 (Feature K v2)
    {"threadpool", TokenType::THREADPOOL},
    {"concurrent", TokenType::CONCURRENT},
    // Spec v1.0 新关键字
    {"exception",  TokenType::EXCEPTION},
};

Lexer::Lexer(const std::string& source) : source_(source) {}

char Lexer::peek(int offset) const {
    size_t idx = pos_ + offset;
    return (idx < source_.size()) ? source_[idx] : '\0';
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') line_++;
    return c;
}

void Lexer::skipWhitespace() {
    while (pos_ < source_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') advance();
        else break;
    }
}

void Lexer::skipComment() {
    // 单行注释 //
    while (pos_ < source_.size() && peek() != '\n') advance();
}

Token Lexer::readNumber() {
    std::string num;
    while (pos_ < source_.size() && (std::isdigit(peek()) || peek() == '.'))
        num += advance();
    return {TokenType::NUMBER, num, line_};
}

// 字符串插值：把 "hello \(name)!" 展开为多个 Token
// "hello " + str(name) + "!"
// 返回值：所有相关 token（可能多个）
std::vector<Token> Lexer::readStringWithInterp() {
    int startLine = line_;
    advance(); // 跳过开头 "
    std::vector<Token> result;
    std::string segment;

    auto flushSegment = [&]() {
        // 将当前积累的字符串段加入结果
        if (!result.empty()) {
            result.push_back({TokenType::PLUS, "+", line_});
        }
        result.push_back({TokenType::STRING, segment, line_});
        segment.clear();
    };

    while (pos_ < source_.size() && peek() != '"') {
        char c = advance();
        if (c == '\\') {
            char next = peek();
            if (next == 'n')  { advance(); segment += '\n'; }
            else if (next == 't')  { advance(); segment += '\t'; }
            else if (next == '"')  { advance(); segment += '"'; }
            else if (next == '\\') { advance(); segment += '\\'; }
            else if (next == '(') {
                // 字符串插值：\(expr)
                advance(); // 跳过 (

                // 先把积累的字符串段输出
                flushSegment();
                if (!result.empty()) {
                    result.push_back({TokenType::PLUS, "+", line_});
                }

                // 包裹在 str(...) 中：str( <inner tokens> )
                result.push_back({TokenType::IDENTIFIER, "str", line_});
                result.push_back({TokenType::LPAREN, "(", line_});

                // 读取括号内的内容，重新 lex（支持嵌套）
                std::string inner;
                int depth = 1;
                while (pos_ < source_.size() && depth > 0) {
                    char ic = advance();
                    if (ic == '(') { depth++; inner += ic; }
                    else if (ic == ')') { depth--; if (depth > 0) inner += ic; }
                    else inner += ic;
                }

                // 递归 lex 内部表达式
                Lexer innerLexer(inner);
                auto innerTokens = innerLexer.tokenize();
                for (auto& t : innerTokens) {
                    if (t.type != TokenType::EOF_TOKEN)
                        result.push_back(t);
                }
                result.push_back({TokenType::RPAREN, ")", line_});
            } else {
                segment += c;
                segment += next;
                advance();
            }
        } else {
            segment += c;
        }
    }
    if (pos_ < source_.size()) advance(); // 跳过结尾 "

    // 最后一段
    if (result.empty()) {
        // 纯字符串，无插值
        result.push_back({TokenType::STRING, segment, startLine});
    } else {
        result.push_back({TokenType::PLUS, "+", line_});
        result.push_back({TokenType::STRING, segment, line_});
    }

    return result;
}

Token Lexer::readIdentifier() {
    std::string word;
    while (pos_ < source_.size() && (std::isalnum(peek()) || peek() == '_'))
        word += advance();
    auto it = keywords_.find(word);
    TokenType type = (it != keywords_.end()) ? it->second : TokenType::IDENTIFIER;
    return {type, word, line_};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (pos_ < source_.size()) {
        skipWhitespace();
        if (pos_ >= source_.size()) break;

        char c = peek();

        // 注释
        if (c == '/' && peek(1) == '/') { advance(); advance(); skipComment(); continue; }

        // 换行
        if (c == '\n') { tokens.push_back({TokenType::NEWLINE, "\\n", line_}); advance(); continue; }

        // 数字
        if (std::isdigit(c)) { tokens.push_back(readNumber()); continue; }

        // 字符串（含插值）
        if (c == '"') {
            auto strTokens = readStringWithInterp();
            for (auto& t : strTokens) tokens.push_back(t);
            continue;
        }

        // 标识符 / 关键字
        if (std::isalpha(c) || c == '_') { tokens.push_back(readIdentifier()); continue; }

        // 运算符和分隔符
        advance();
        switch (c) {
            case '+': { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::PLUS_ASSIGN  : TokenType::PLUS,  p ? "+=" : "+", line_}); break; }
            case '-':
                if (peek() == '>') { advance(); tokens.push_back({TokenType::ARROW, "->", line_}); }
                else { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::MINUS_ASSIGN : TokenType::MINUS, p ? "-=" : "-", line_}); }
                break;
            case '*': tokens.push_back({TokenType::STAR,    "*", line_}); break;
            case '/': tokens.push_back({TokenType::SLASH,   "/", line_}); break;
            case '%': tokens.push_back({TokenType::PERCENT, "%", line_}); break;
            case '=': { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::EQ     : TokenType::ASSIGN, p ? "==" : "=",  line_}); break; }
            case '!': { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::NEQ    : TokenType::NOT,    p ? "!=" : "!",  line_}); break; }
            case '<': { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::LEQ    : TokenType::LT,     p ? "<=" : "<",  line_}); break; }
            case '>': { bool p = peek()=='='; if(p) advance(); tokens.push_back({p ? TokenType::GEQ    : TokenType::GT,     p ? ">=" : ">",  line_}); break; }
            case '&': if (peek()=='&') { advance(); tokens.push_back({TokenType::AND, "&&", line_}); } break;
            case '|': if (peek()=='|') { advance(); tokens.push_back({TokenType::OR,  "||", line_}); } break;
            case '?': if (peek()=='?') { advance(); tokens.push_back({TokenType::QUESTION_QUESTION, "??", line_}); } break;
            case '(': tokens.push_back({TokenType::LPAREN, "(", line_}); break;
            case ')': tokens.push_back({TokenType::RPAREN, ")", line_}); break;
            case '{': tokens.push_back({TokenType::LBRACE, "{", line_}); break;
            case '}': tokens.push_back({TokenType::RBRACE, "}", line_}); break;
            case ',': tokens.push_back({TokenType::COMMA,  ",", line_}); break;
            case ':': tokens.push_back({TokenType::COLON,  ":", line_}); break;
            case '.':
                // .always / .never 枚举值
                if (pos_ < source_.size() && std::isalpha(peek())) {
                    std::string word;
                    while (pos_ < source_.size() && std::isalpha(peek()))
                        word += advance();
                    if      (word == "always") tokens.push_back({TokenType::DOT_ALWAYS, ".always", line_});
                    else if (word == "never")  tokens.push_back({TokenType::DOT_NEVER,  ".never",  line_});
                    else if (word == "block")  tokens.push_back({TokenType::DOT_BLOCK,  ".block",  line_});
                    else if (word == "drop")   tokens.push_back({TokenType::DOT_DROP,   ".drop",   line_});
                    else if (word == "error")  tokens.push_back({TokenType::DOT_ERROR,  ".error",  line_});
                    else {
                        tokens.push_back({TokenType::DOT, ".", line_});
                        // 把 word 作为标识符推回
                        auto it = keywords_.find(word);
                        TokenType t = (it != keywords_.end()) ? it->second : TokenType::IDENTIFIER;
                        tokens.push_back({t, word, line_});
                    }
                } else {
                    tokens.push_back({TokenType::DOT, ".", line_});
                }
                break;
            case '@': tokens.push_back({TokenType::AT, "@", line_}); break;
            case '[': tokens.push_back({TokenType::LBRACKET, "[", line_}); break;
            case ']': tokens.push_back({TokenType::RBRACKET, "]", line_}); break;
            default: break; // 忽略未知字符
        }
    }

    tokens.push_back({TokenType::EOF_TOKEN, "", line_});
    return tokens;
}
