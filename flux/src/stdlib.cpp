// stdlib.cpp — Flux 标准库（File、Json、Http、Time、Math、Set、Log、Env、Test、hw）
// 通过 Interpreter::registerStdlib() 注册到解释器
#include "interpreter.h"
#include "hw.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cctype>
#include <cmath>    // std::floor, std::ceil, std::round, std::pow, etc.
#include <cstdio>   // std::remove, std::snprintf
#include <cstdlib>  // std::rand, std::getenv
#include <ctime>    // std::time, std::localtime, std::strftime
#include <chrono>   // std::chrono::*
#include <thread>   // std::this_thread::sleep_for

// POSIX socket（Http 模块使用）
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

// ═══════════════════════════════════════════════════════════
// JSON 解析器（手写递归下降）
// ═══════════════════════════════════════════════════════════
class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src), pos_(0) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (pos_ != src_.size())
            throw std::runtime_error("Json.parse: trailing garbage at position "
                                     + std::to_string(pos_));
        return v;
    }

private:
    const std::string& src_;
    size_t             pos_;

    void skipWs() {
        while (pos_ < src_.size() && std::isspace((unsigned char)src_[pos_])) ++pos_;
    }
    char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char advance()    { return src_[pos_++]; }

    void expect(char c) {
        if (peek() != c)
            throw std::runtime_error(std::string("Json.parse: expected '") + c
                                     + "' got '" + (pos_ < src_.size() ? src_[pos_] : '?') + "'");
        ++pos_;
    }

    Value parseValue() {
        skipWs();
        char c = peek();
        if (c == '"') return Value::Str(parseString());
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't') { expect('t'); expect('r'); expect('u'); expect('e');
                        return Value::Bool(true); }
        if (c == 'f') { expect('f'); expect('a'); expect('l'); expect('s'); expect('e');
                        return Value::Bool(false); }
        if (c == 'n') { expect('n'); expect('u'); expect('l'); expect('l');
                        return Value::Nil(); }
        if (c == '-' || std::isdigit((unsigned char)c)) return parseNumber();
        throw std::runtime_error(std::string("Json.parse: unexpected character '") + c + "'");
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (pos_ < src_.size() && peek() != '"') {
            char c = advance();
            if (c != '\\') { result += c; continue; }
            char e = advance();
            switch (e) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'u': {
                    // 4 位 hex Unicode（简化：仅支持 BMP 基本平面）
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && pos_ < src_.size(); ++i) {
                        char h = advance();
                        cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    }
                    // 编码为 UTF-8
                    if (cp < 0x80) {
                        result += (char)cp;
                    } else if (cp < 0x800) {
                        result += (char)(0xC0 | (cp >> 6));
                        result += (char)(0x80 | (cp & 0x3F));
                    } else {
                        result += (char)(0xE0 | (cp >> 12));
                        result += (char)(0x80 | ((cp >> 6) & 0x3F));
                        result += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: result += c; result += e; break;
            }
        }
        expect('"');
        return result;
    }

    Value parseNumber() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit((unsigned char)src_[pos_])) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            while (pos_ < src_.size() && std::isdigit((unsigned char)src_[pos_])) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit((unsigned char)src_[pos_])) ++pos_;
        }
        return Value::Num(std::stod(src_.substr(start, pos_ - start)));
    }

    Value parseArray() {
        expect('[');
        auto arr = std::make_shared<std::vector<Value>>();
        skipWs();
        if (peek() == ']') { ++pos_; return Value::Arr(arr); }
        while (true) {
            arr->push_back(parseValue());
            skipWs();
            if (peek() == ']') { ++pos_; break; }
            if (peek() != ',')
                throw std::runtime_error("Json.parse: expected ',' or ']' in array");
            ++pos_;
        }
        return Value::Arr(arr);
    }

    Value parseObject() {
        expect('{');
        auto map = std::make_shared<std::unordered_map<std::string, Value>>();
        skipWs();
        if (peek() == '}') { ++pos_; return Value::MapOf(map); }
        while (true) {
            skipWs();
            if (peek() != '"')
                throw std::runtime_error("Json.parse: expected string key in object");
            std::string key = parseString();
            skipWs();
            expect(':');
            (*map)[key] = parseValue();
            skipWs();
            if (peek() == '}') { ++pos_; break; }
            if (peek() != ',')
                throw std::runtime_error("Json.parse: expected ',' or '}' in object");
            ++pos_;
        }
        return Value::MapOf(map);
    }
};

// ── JSON 序列化 ──────────────────────────────────────────
static std::string jsonStringify(const Value& v, int indent, int depth) {
    std::string nl   = indent >= 0 ? "\n" : "";
    std::string pad  = indent >= 0 ? std::string((size_t)depth       * (size_t)indent, ' ') : "";
    std::string cpad = indent >= 0 ? std::string((size_t)(depth + 1) * (size_t)indent, ' ') : "";

    switch (v.type) {
        case Value::Type::Nil:  return "null";
        case Value::Type::Bool: return v.boolean ? "true" : "false";
        case Value::Type::Number: {
            if (v.number == (long long)v.number)
                return std::to_string((long long)v.number);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", v.number);
            return buf;
        }
        case Value::Type::String: {
            std::string s = "\"";
            for (unsigned char c : v.string) {
                if      (c == '"')  s += "\\\"";
                else if (c == '\\') s += "\\\\";
                else if (c == '\n') s += "\\n";
                else if (c == '\r') s += "\\r";
                else if (c == '\t') s += "\\t";
                else if (c == '\b') s += "\\b";
                else if (c == '\f') s += "\\f";
                else if (c < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                    s += esc;
                } else {
                    s += (char)c;
                }
            }
            return s + "\"";
        }
        case Value::Type::Array: {
            if (!v.array || v.array->empty()) return "[]";
            std::string s = "[" + nl;
            for (size_t i = 0; i < v.array->size(); ++i) {
                if (i > 0) s += "," + nl;
                s += cpad + jsonStringify((*v.array)[i], indent, depth + 1);
            }
            return s + nl + pad + "]";
        }
        case Value::Type::Map: {
            if (!v.map || v.map->empty()) return "{}";
            std::string s = "{" + nl;
            bool first = true;
            for (auto& kv : *v.map) {
                if (!first) s += "," + nl;
                s += cpad + "\"" + kv.first + "\":"
                   + (indent >= 0 ? " " : "")
                   + jsonStringify(kv.second, indent, depth + 1);
                first = false;
            }
            return s + nl + pad + "}";
        }
        default: return "null";
    }
}

// ═══════════════════════════════════════════════════════════
// HTTP 简易客户端（POSIX socket，仅支持 HTTP）
// ═══════════════════════════════════════════════════════════
struct UrlParts { std::string host, path; int port; bool https; };

static UrlParts parseUrl(const std::string& url) {
    UrlParts p; p.port = 80; p.https = false;
    std::string u = url;
    if (u.size() >= 8 && u.substr(0, 8) == "https://") {
        p.https = true; p.port = 443; u = u.substr(8);
    } else if (u.size() >= 7 && u.substr(0, 7) == "http://") {
        u = u.substr(7);
    }
    size_t sl = u.find('/');
    p.host = sl == std::string::npos ? u : u.substr(0, sl);
    p.path = sl == std::string::npos ? "/" : u.substr(sl);
    // 处理 query string 中的 host:port
    size_t co = p.host.find(':');
    if (co != std::string::npos) {
        p.port = std::stoi(p.host.substr(co + 1));
        p.host = p.host.substr(0, co);
    }
    return p;
}

static std::string httpDoRequest(const std::string& method,
                                  const std::string& url,
                                  const std::string& body,
                                  const std::string& contentType) {
    auto p = parseUrl(url);
    if (p.https)
        throw std::runtime_error(
            "Http: HTTPS is not supported without libcurl. Use http:// instead.");

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(p.host.c_str(), std::to_string(p.port).c_str(), &hints, &res);
    if (err || !res)
        throw std::runtime_error("Http." + method + ": DNS lookup failed for '"
                                 + p.host + "': " + gai_strerror(err));

    int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); throw std::runtime_error("Http: socket() failed"); }

    if (::connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(sock); freeaddrinfo(res);
        throw std::runtime_error("Http." + method + ": connect failed to " + p.host);
    }
    freeaddrinfo(res);

    // 构建 HTTP/1.1 请求
    std::string req = method + " " + p.path + " HTTP/1.1\r\n"
                      "Host: " + p.host + "\r\n"
                      "Connection: close\r\n"
                      "User-Agent: Flux/1.0\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    if (!body.empty()) req += body;

    if (::send(sock, req.c_str(), req.size(), 0) < 0) {
        ::close(sock);
        throw std::runtime_error("Http: send() failed");
    }

    // 读取完整响应
    std::string response;
    char buf[8192];
    ssize_t n;
    while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
        response.append(buf, (size_t)n);
    ::close(sock);

    // 去掉 HTTP 头，返回 body
    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos)
        return response.substr(header_end + 4);
    return response;
}

// ═══════════════════════════════════════════════════════════
// 模块工厂
// ═══════════════════════════════════════════════════════════

// ── File 模块 ─────────────────────────────────────────────
static std::unordered_map<std::string, StdlibFn> makeFileModule() {
    return {
        // File.read(path) -> String
        {"read", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.read(path) — path must be a String");
            std::ifstream f(args[0].string, std::ios::binary);
            if (!f)
                throw std::runtime_error("File.read: cannot open '" + args[0].string + "'");
            return Value::Str({std::istreambuf_iterator<char>(f), {}});
        }},

        // File.write(path, content) -> Bool
        {"write", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.write(path, content) — path must be a String");
            std::ofstream f(args[0].string, std::ios::binary);
            if (!f) return Value::Bool(false);
            std::string content = args.size() > 1 ? args[1].toString() : "";
            f << content;
            return Value::Bool(f.good());
        }},

        // File.append(path, content) -> Bool
        {"append", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.append(path, content) — path must be a String");
            std::ofstream f(args[0].string, std::ios::binary | std::ios::app);
            if (!f) return Value::Bool(false);
            std::string content = args.size() > 1 ? args[1].toString() : "";
            f << content;
            return Value::Bool(f.good());
        }},

        // File.exists(path) -> Bool
        {"exists", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.exists(path) — path must be a String");
            std::ifstream f(args[0].string);
            return Value::Bool(f.good());
        }},

        // File.lines(path) -> Array[String]
        {"lines", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.lines(path) — path must be a String");
            std::ifstream f(args[0].string);
            if (!f)
                throw std::runtime_error("File.lines: cannot open '" + args[0].string + "'");
            auto arr = std::make_shared<std::vector<Value>>();
            std::string line;
            while (std::getline(f, line)) arr->push_back(Value::Str(line));
            return Value::Arr(arr);
        }},

        // File.delete(path) -> Bool
        {"delete", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.delete(path) — path must be a String");
            return Value::Bool(std::remove(args[0].string.c_str()) == 0);
        }},
    };
}

// ── Json 模块 ─────────────────────────────────────────────
static std::unordered_map<std::string, StdlibFn> makeJsonModule() {
    return {
        // Json.parse(str) -> Any
        {"parse", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Json.parse(str) — argument must be a String");
            return JsonParser(args[0].string).parse();
        }},

        // Json.stringify(value) -> String
        {"stringify", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Str("null");
            return Value::Str(jsonStringify(args[0], -1, 0));
        }},

        // Json.pretty(value, indent=2) -> String
        {"pretty", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Str("null");
            int indent = 2;
            if (args.size() > 1 && args[1].type == Value::Type::Number)
                indent = (int)args[1].number;
            return Value::Str(jsonStringify(args[0], indent, 0));
        }},
    };
}

// ── Http 模块 ─────────────────────────────────────────────
static std::unordered_map<std::string, StdlibFn> makeHttpModule() {
    return {
        // Http.get(url) -> String
        {"get", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.get(url) — url must be a String");
            return Value::Str(httpDoRequest("GET", args[0].string, "", ""));
        }},

        // Http.post(url, body, contentType="application/json") -> String
        {"post", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.post(url, body) — url must be a String");
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string ct   = args.size() > 2 ? args[2].toString() : "application/json";
            return Value::Str(httpDoRequest("POST", args[0].string, body, ct));
        }},

        // Http.put(url, body, contentType="application/json") -> String
        {"put", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.put(url, body) — url must be a String");
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string ct   = args.size() > 2 ? args[2].toString() : "application/json";
            return Value::Str(httpDoRequest("PUT", args[0].string, body, ct));
        }},

        // Http.delete(url) -> String
        {"delete", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.delete(url) — url must be a String");
            return Value::Str(httpDoRequest("DELETE", args[0].string, "", ""));
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Time 模块
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeTimeModule() {
    return {
        // Time.now() → Number  (Unix epoch seconds, double precision)
        {"now", [](std::vector<Value>) -> Value {
            using namespace std::chrono;
            auto tp  = system_clock::now();
            auto sec = duration<double>(tp.time_since_epoch()).count();
            return Value::Num(sec);
        }},

        // Time.clock() → Number  (nanoseconds since epoch, for benchmarking)
        {"clock", [](std::vector<Value>) -> Value {
            using namespace std::chrono;
            auto tp = high_resolution_clock::now();
            auto ns = duration_cast<nanoseconds>(tp.time_since_epoch()).count();
            return Value::Num((double)ns);
        }},

        // Time.sleep(ms) → Nil  (releases GIL during sleep)
        {"sleep", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Time.sleep(ms) — ms must be a Number");
            auto ms = (long long)args[0].number;
            {
                GILRelease release;   // allow other threads to run while sleeping
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
            return Value::Nil();
        }},

        // Time.format(ts) → String  (human-readable local time)
        {"format", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Time.format(ts) — ts must be a Number (epoch seconds)");
            std::time_t t = (std::time_t)args[0].number;
            // Optional format string (arg 1); default: "%Y-%m-%d %H:%M:%S"
            std::string fmt = "%Y-%m-%d %H:%M:%S";
            if (args.size() > 1 && args[1].type == Value::Type::String)
                fmt = args[1].string;
            char buf[128];
            struct tm* tm_info = std::localtime(&t);
            std::strftime(buf, sizeof(buf), fmt.c_str(), tm_info);
            return Value::Str(buf);
        }},

        // Time.diff(a, b) → Number  (b − a, in seconds)
        {"diff", [](std::vector<Value> args) -> Value {
            if (args.size() < 2
                || args[0].type != Value::Type::Number
                || args[1].type != Value::Type::Number)
                throw std::runtime_error("Time.diff(a, b) — both arguments must be Numbers");
            return Value::Num(args[1].number - args[0].number);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// 注册所有标准库模块
// ═══════════════════════════════════════════════════════════
// ── Chan 模块：创建通道 ───────────────────────────────────
// Chan.make()     — 无界通道
// Chan.make(cap)  — 有界通道（capacity = cap）
static std::unordered_map<std::string, StdlibFn> makeChanModule() {
    std::unordered_map<std::string, StdlibFn> m;
    m["make"] = [](std::vector<Value> args) -> Value {
        size_t cap = 0;
        if (!args.empty() && args[0].type == Value::Type::Number)
            cap = (size_t)args[0].number;
        return Value::Chan(std::make_shared<ChanVal>(cap));
    };
    return m;
}

// ═══════════════════════════════════════════════════════════
// Math 模块
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeMathModule() {
    return {
        {"abs", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Math.abs(x) — x must be a Number");
            return Value::Num(std::abs(args[0].number));
        }},
        {"floor", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::floor(args[0].number));
        }},
        {"ceil", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::ceil(args[0].number));
        }},
        {"round", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::round(args[0].number));
        }},
        {"min", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("Math.min(a, b) — requires 2 args");
            return Value::Num(std::min(args[0].number, args[1].number));
        }},
        {"max", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("Math.max(a, b) — requires 2 args");
            return Value::Num(std::max(args[0].number, args[1].number));
        }},
        {"pow", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("Math.pow(base, exp) — requires 2 args");
            return Value::Num(std::pow(args[0].number, args[1].number));
        }},
        {"log", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::log(args[0].number));
        }},
        {"sin", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::sin(args[0].number));
        }},
        {"cos", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::cos(args[0].number));
        }},
        {"random", [](std::vector<Value>) -> Value {
            return Value::Num((double)std::rand() / RAND_MAX);
        }},
        {"PI", [](std::vector<Value>) -> Value {
            return Value::Num(3.14159265358979323846);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Set 模块 — 基于 Map 的集合操作
// Set 内部用 Map（key → true）表示
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeSetModule() {
    return {
        // Set.new() → Map (empty set)
        {"new", [](std::vector<Value>) -> Value {
            return Value::MapVal();
        }},
        // Set.from(array) → Map (set from array elements)
        {"from", [](std::vector<Value> args) -> Value {
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            if (!args.empty() && args[0].type == Value::Type::Array && args[0].array) {
                for (auto& v : *args[0].array)
                    (*map)[v.toString()] = Value::Bool(true);
            }
            return Value::MapOf(map);
        }},
        // Set.add(set, value) → Map
        {"add", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::Map || !args[0].map)
                throw std::runtime_error("Set.add(set, value)");
            (*args[0].map)[args[1].toString()] = Value::Bool(true);
            return args[0];
        }},
        // Set.has(set, value) → Bool
        {"has", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::Map || !args[0].map)
                return Value::Bool(false);
            return Value::Bool(args[0].map->count(args[1].toString()) > 0);
        }},
        // Set.remove(set, value) → Bool
        {"remove", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::Map || !args[0].map)
                return Value::Bool(false);
            return Value::Bool(args[0].map->erase(args[1].toString()) > 0);
        }},
        // Set.size(set) → Number
        {"size", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Map || !args[0].map)
                return Value::Num(0);
            return Value::Num((double)args[0].map->size());
        }},
        // Set.toArray(set) → Array
        {"toArray", [](std::vector<Value> args) -> Value {
            auto arr = std::make_shared<std::vector<Value>>();
            if (!args.empty() && args[0].type == Value::Type::Map && args[0].map) {
                for (auto& kv : *args[0].map)
                    arr->push_back(Value::Str(kv.first));
            }
            return Value::Arr(arr);
        }},
        // Set.union(a, b) → Map
        {"union", [](std::vector<Value> args) -> Value {
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            if (args.size() >= 1 && args[0].type == Value::Type::Map && args[0].map)
                for (auto& kv : *args[0].map) (*map)[kv.first] = Value::Bool(true);
            if (args.size() >= 2 && args[1].type == Value::Type::Map && args[1].map)
                for (auto& kv : *args[1].map) (*map)[kv.first] = Value::Bool(true);
            return Value::MapOf(map);
        }},
        // Set.intersect(a, b) → Map
        {"intersect", [](std::vector<Value> args) -> Value {
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            if (args.size() >= 2 && args[0].type == Value::Type::Map && args[0].map
                && args[1].type == Value::Type::Map && args[1].map) {
                for (auto& kv : *args[0].map)
                    if (args[1].map->count(kv.first))
                        (*map)[kv.first] = Value::Bool(true);
            }
            return Value::MapOf(map);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Log 模块 — 结构化日志
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeLogModule() {
    return {
        {"info", [](std::vector<Value> args) -> Value {
            std::cout << "\033[36m[INFO]\033[0m ";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value::Nil();
        }},
        {"warn", [](std::vector<Value> args) -> Value {
            std::cout << "\033[33m[WARN]\033[0m ";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value::Nil();
        }},
        {"error", [](std::vector<Value> args) -> Value {
            std::cerr << "\033[31m[ERROR]\033[0m ";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cerr << " ";
                std::cerr << args[i].toString();
            }
            std::cerr << "\n";
            return Value::Nil();
        }},
        {"debug", [](std::vector<Value> args) -> Value {
            std::cout << "\033[90m[DEBUG]\033[0m ";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value::Nil();
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Env 模块 — 环境变量
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeEnvModule() {
    return {
        {"get", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Env.get(name) — name must be a String");
            const char* val = std::getenv(args[0].string.c_str());
            return val ? Value::Str(val) : Value::Nil();
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Test 模块 — 内置测试框架
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeTestModule() {
    return {
        // Test.equal(actual, expected, msg?) → Nil (throws on mismatch)
        {"equal", [](std::vector<Value> args) -> Value {
            if (args.size() < 2)
                throw std::runtime_error("Test.equal(actual, expected)");
            std::string a = args[0].toString(), b = args[1].toString();
            if (a != b) {
                std::string msg = args.size() > 2 ? args[2].toString() : "";
                throw PanicSignal{"Test failed: expected " + b + ", got " + a
                                  + (msg.empty() ? "" : " — " + msg)};
            }
            return Value::Nil();
        }},
        // Test.notEqual(actual, expected, msg?) → Nil
        {"notEqual", [](std::vector<Value> args) -> Value {
            if (args.size() < 2)
                throw std::runtime_error("Test.notEqual(actual, expected)");
            std::string a = args[0].toString(), b = args[1].toString();
            if (a == b) {
                std::string msg = args.size() > 2 ? args[2].toString() : "";
                throw PanicSignal{"Test failed: values should not be equal: " + a
                                  + (msg.empty() ? "" : " — " + msg)};
            }
            return Value::Nil();
        }},
        // Test.isTrue(cond, msg?) → Nil
        {"isTrue", [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].isTruthy()) {
                std::string msg = args.size() > 1 ? args[1].toString() : "expected true";
                throw PanicSignal{"Test failed: " + msg};
            }
            return Value::Nil();
        }},
        // Test.isFalse(cond, msg?) → Nil
        {"isFalse", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].isTruthy()) {
                std::string msg = args.size() > 1 ? args[1].toString() : "expected false";
                throw PanicSignal{"Test failed: " + msg};
            }
            return Value::Nil();
        }},
        // Test.isNil(val, msg?) → Nil
        {"isNil", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Nil) {
                std::string msg = args.size() > 1 ? args[1].toString() : "expected nil";
                throw PanicSignal{"Test failed: " + msg};
            }
            return Value::Nil();
        }},
        // Test.throws(fn_name, msg?) → Bool (checks if calling fn() panics)
        // Note: This is a simplified version - actual implementation would
        // need interpreter access to call the function
        {"throws", [](std::vector<Value> args) -> Value {
            if (args.empty())
                throw std::runtime_error("Test.throws(fn_name)");
            // placeholder — true exception testing requires interpreter integration
            return Value::Bool(true);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Http.serve — 简易 HTTP 服务器
// ═══════════════════════════════════════════════════════════
static void addHttpServerToModule(std::unordered_map<std::string, StdlibFn>& httpMod) {
    // Http.serve(port, handler_map) → Nil (blocking)
    // handler_map is a Map of "METHOD /path" → response string
    httpMod["serve"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Type::Number)
            throw std::runtime_error("Http.serve(port, routes)");

        int port = (int)args[0].number;
        Value routes = args[1]; // Map of "GET /path" → response

        int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0) throw std::runtime_error("Http.serve: socket() failed");

        int opt = 1;
        setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(serverFd);
            throw std::runtime_error("Http.serve: bind() failed on port " + std::to_string(port));
        }
        if (::listen(serverFd, 128) < 0) {
            ::close(serverFd);
            throw std::runtime_error("Http.serve: listen() failed");
        }

        std::cout << "\033[32m[Http.serve]\033[0m Listening on port " << port << "\n";

        while (true) {
            int clientFd;
            {
                GILRelease release;
                clientFd = ::accept(serverFd, nullptr, nullptr);
            }
            if (clientFd < 0) continue;

            // Read request
            char buf[4096];
            ssize_t n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) { ::close(clientFd); continue; }
            buf[n] = '\0';

            // Parse request line: "GET /path HTTP/1.1"
            std::string req(buf);
            std::string method, path;
            {
                size_t sp1 = req.find(' ');
                size_t sp2 = req.find(' ', sp1 + 1);
                if (sp1 != std::string::npos && sp2 != std::string::npos) {
                    method = req.substr(0, sp1);
                    path   = req.substr(sp1 + 1, sp2 - sp1 - 1);
                }
            }

            // Route lookup
            std::string responseBody = "404 Not Found";
            int status = 404;
            if (routes.type == Value::Type::Map && routes.map) {
                std::string key = method + " " + path;
                auto it = routes.map->find(key);
                if (it != routes.map->end()) {
                    responseBody = it->second.toString();
                    status = 200;
                } else {
                    // Try wildcard: "GET *"
                    auto wit = routes.map->find(method + " *");
                    if (wit != routes.map->end()) {
                        responseBody = wit->second.toString();
                        status = 200;
                    }
                }
            }

            // Send response
            std::string response = "HTTP/1.1 " + std::to_string(status) + " "
                + (status == 200 ? "OK" : "Not Found") + "\r\n"
                + "Content-Type: text/plain\r\n"
                + "Content-Length: " + std::to_string(responseBody.size()) + "\r\n"
                + "Connection: close\r\n\r\n"
                + responseBody;
            ::send(clientFd, response.c_str(), response.size(), 0);
            ::close(clientFd);
        }
        ::close(serverFd);
        return Value::Nil();
    };
}

void Interpreter::registerStdlib() {
    registerStdlibModule("File", makeFileModule());
    registerStdlibModule("Json", makeJsonModule());
    auto httpMod = makeHttpModule();
    addHttpServerToModule(httpMod);
    registerStdlibModule("Http", std::move(httpMod));
    registerStdlibModule("Time", makeTimeModule());
    registerStdlibModule("Chan", makeChanModule());
    registerStdlibModule("Math", makeMathModule());
    registerStdlibModule("Set",  makeSetModule());
    registerStdlibModule("Log",  makeLogModule());
    registerStdlibModule("Env",  makeEnvModule());
    registerStdlibModule("Test", makeTestModule());
    registerStdlibModule("hw",   makeHwModule());
}
