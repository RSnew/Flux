// stdlib.cpp — Flux 标准库（File、Json、Http、Time、Math、Set、Log、Env、Test、hw）
// 通过 Interpreter::registerStdlib() 注册到解释器
#include "interpreter.h"
#include "hw.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cctype>
#include <cmath>    // std::floor, std::ceil, std::round, std::pow, etc.
#include <cstdio>   // std::remove, std::snprintf
#include <cstdlib>  // std::rand, std::getenv, std::setenv
#include <ctime>    // std::time, std::localtime, std::strftime
#include <chrono>   // std::chrono::*
#include <thread>   // std::this_thread::sleep_for
#include <mutex>    // std::mutex, std::once_flag
#include <algorithm> // std::sort, std::transform
#include <filesystem> // File operations (C++17)

namespace fs = std::filesystem;

// POSIX socket（Http 模块使用）
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

// OpenSSL（HTTPS 支持）
#include <openssl/ssl.h>
#include <openssl/err.h>

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

// RAII 清理助手
struct SockGuard {
    int fd; SSL* ssl;
    SockGuard(int f, SSL* s = nullptr) : fd(f), ssl(s) {}
    ~SockGuard() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (fd >= 0) ::close(fd);
    }
};

// ── 全局 SSL_CTX 缓存（避免每次请求重建 + 重新加载 CA）──
static SSL_CTX* getGlobalSSLCtx() {
    static SSL_CTX* ctx = nullptr;
    static std::once_flag flag;
    std::call_once(flag, []() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx) {
            SSL_CTX_set_default_verify_paths(ctx);
            // 启用 SSL 会话缓存，复用 TLS 会话可跳过完整握手
            SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
        }
    });
    return ctx;
}

// ── DNS 缓存（同一 host 不重复解析）──────────────────────
static std::mutex dnsCacheMutex;
static std::unordered_map<std::string, struct sockaddr_storage> dnsCache;

static bool dnsLookupCached(const std::string& host, int port,
                            struct sockaddr_storage& out, socklen_t& outLen) {
    std::string key = host + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lk(dnsCacheMutex);
        auto it = dnsCache.find(key);
        if (it != dnsCache.end()) {
            out = it->second;
            outLen = (out.ss_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                 : sizeof(struct sockaddr_in);
            return true;
        }
    }
    // 未命中 → 实际解析
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (err || !res) return false;
    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    outLen = res->ai_addrlen;
    freeaddrinfo(res);
    {
        std::lock_guard<std::mutex> lk(dnsCacheMutex);
        dnsCache[key] = out;
    }
    return true;
}

static std::string httpDoRequest(const std::string& method,
                                  const std::string& url,
                                  const std::string& body,
                                  const std::string& contentType) {
    auto p = parseUrl(url);

    // ── DNS 解析（带缓存）+ TCP 连接 ─────────────────────
    struct sockaddr_storage addr{};
    socklen_t addrLen = 0;
    if (!dnsLookupCached(p.host, p.port, addr, addrLen))
        throw std::runtime_error("Http." + method + ": DNS lookup failed for '" + p.host + "'");

    int sock = ::socket(addr.ss_family, SOCK_STREAM, 0);
    if (sock < 0) throw std::runtime_error("Http: socket() failed");

    if (::connect(sock, (struct sockaddr*)&addr, addrLen) < 0) {
        ::close(sock);
        throw std::runtime_error("Http." + method + ": connect failed to " + p.host);
    }

    // ── 构建 HTTP/1.1 请求 ──────────────────────────────
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

    std::string response;
    char buf[8192];

    if (p.https) {
        // ── HTTPS: 复用全局 SSL_CTX ─────────────────────
        SSL_CTX* ctx = getGlobalSSLCtx();
        if (!ctx) {
            ::close(sock);
            throw std::runtime_error("Http: SSL_CTX not available");
        }

        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, p.host.c_str());

        SockGuard guard(sock, ssl);

        if (SSL_connect(ssl) <= 0)
            throw std::runtime_error("Http." + method + ": TLS handshake failed with " + p.host);

        if (SSL_write(ssl, req.c_str(), (int)req.size()) <= 0)
            throw std::runtime_error("Http: SSL_write() failed");

        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0)
            response.append(buf, (size_t)n);

        // SSL_CTX 不释放 — 全局复用
    } else {
        // ── HTTP: 纯 TCP ────────────────────────────────
        SockGuard guard(sock, nullptr);

        if (::send(sock, req.c_str(), req.size(), 0) < 0)
            throw std::runtime_error("Http: send() failed");

        ssize_t n;
        while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
            response.append(buf, (size_t)n);
    }

    // ── 去掉 HTTP 头，返回 body ─────────────────────────
    size_t header_end = response.find("\r\n\r\n");
    if (header_end != std::string::npos)
        return response.substr(header_end + 4);
    return response;
}

// ── 二进制下载到文件 ──────────────────────────────────────
// 返回写入的字节数
static size_t httpDownloadToFile(const std::string& url, const std::string& path) {
    // 复用 httpDoRequest 获取原始响应（body 是 std::string，可存二进制）
    std::string data = httpDoRequest("GET", url, "", "");

    // 以二进制模式写入文件
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Http.download: cannot open '" + path + "' for writing");
    out.write(data.data(), (std::streamsize)data.size());
    out.close();
    return data.size();
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

        // File.rename(oldPath, newPath) -> Bool
        {"rename", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                || args[1].type != Value::Type::String)
                throw std::runtime_error("File.rename(old, new) — both must be Strings");
            std::error_code ec;
            fs::rename(args[0].string, args[1].string, ec);
            return Value::Bool(!ec);
        }},

        // File.copy(src, dst) -> Bool
        {"copy", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                || args[1].type != Value::Type::String)
                throw std::runtime_error("File.copy(src, dst) — both must be Strings");
            std::error_code ec;
            fs::copy_file(args[0].string, args[1].string,
                          fs::copy_options::overwrite_existing, ec);
            return Value::Bool(!ec);
        }},

        // File.size(path) -> Number (bytes)
        {"size", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.size(path) — path must be a String");
            std::error_code ec;
            auto sz = fs::file_size(args[0].string, ec);
            if (ec) return Value::Num(-1);
            return Value::Num((double)sz);
        }},

        // File.isDir(path) -> Bool
        {"isDir", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.isDir(path) — path must be a String");
            return Value::Bool(fs::is_directory(args[0].string));
        }},

        // File.mkdir(path) -> Bool (recursive)
        {"mkdir", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.mkdir(path) — path must be a String");
            std::error_code ec;
            fs::create_directories(args[0].string, ec);
            return Value::Bool(!ec);
        }},

        // File.listDir(path) -> Array[String]
        {"listDir", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.listDir(path) — path must be a String");
            auto arr = std::make_shared<std::vector<Value>>();
            std::error_code ec;
            for (auto& entry : fs::directory_iterator(args[0].string, ec)) {
                arr->push_back(Value::Str(entry.path().filename().string()));
            }
            return Value::Arr(arr);
        }},

        // File.ext(path) -> String (file extension)
        {"ext", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.ext(path) — path must be a String");
            return Value::Str(fs::path(args[0].string).extension().string());
        }},

        // File.basename(path) -> String (filename without directory)
        {"basename", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.basename(path) — path must be a String");
            return Value::Str(fs::path(args[0].string).filename().string());
        }},

        // File.dirname(path) -> String (directory part)
        {"dirname", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("File.dirname(path) — path must be a String");
            return Value::Str(fs::path(args[0].string).parent_path().string());
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
        // Http.get(url) -> String（释放 GIL 以允许并发请求）
        {"get", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.get(url) — url must be a String");
            std::string url = args[0].string;
            std::string result;
            { GILRelease release; result = httpDoRequest("GET", url, "", ""); }
            return Value::Str(result);
        }},

        // Http.post(url, body, contentType="application/json") -> String
        {"post", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.post(url, body) — url must be a String");
            std::string url  = args[0].string;
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string ct   = args.size() > 2 ? args[2].toString() : "application/json";
            std::string result;
            { GILRelease release; result = httpDoRequest("POST", url, body, ct); }
            return Value::Str(result);
        }},

        // Http.put(url, body, contentType="application/json") -> String
        {"put", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.put(url, body) — url must be a String");
            std::string url  = args[0].string;
            std::string body = args.size() > 1 ? args[1].toString() : "";
            std::string ct   = args.size() > 2 ? args[2].toString() : "application/json";
            std::string result;
            { GILRelease release; result = httpDoRequest("PUT", url, body, ct); }
            return Value::Str(result);
        }},

        // Http.delete(url) -> String
        {"delete", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Http.delete(url) — url must be a String");
            std::string url = args[0].string;
            std::string result;
            { GILRelease release; result = httpDoRequest("DELETE", url, "", ""); }
            return Value::Str(result);
        }},

        // Http.download(url, path) -> Number (写入字节数)
        // 二进制安全：直接将响应体写入文件，支持图片、视频等
        {"download", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                               || args[1].type != Value::Type::String)
                throw std::runtime_error("Http.download(url, path) — url and path must be Strings");
            std::string url  = args[0].string;
            std::string path = args[1].string;
            size_t bytes;
            { GILRelease release; bytes = httpDownloadToFile(url, path); }
            return Value::Num((double)bytes);
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

        // Time.millis() → Number  (current time in milliseconds)
        {"millis", [](std::vector<Value>) -> Value {
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
            return Value::Num((double)ms);
        }},

        // Time.measure(start_ns) → Number  (elapsed ms since start_ns from Time.clock())
        {"measure", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Time.measure(start_ns) — start_ns from Time.clock()");
            using namespace std::chrono;
            auto now_ns = (double)duration_cast<nanoseconds>(
                high_resolution_clock::now().time_since_epoch()).count();
            return Value::Num((now_ns - args[0].number) / 1e6);  // ms
        }},

        // Time.date() → Map { year, month, day, hour, minute, second, weekday }
        {"date", [](std::vector<Value> args) -> Value {
            std::time_t t;
            if (!args.empty() && args[0].type == Value::Type::Number)
                t = (std::time_t)args[0].number;
            else
                t = std::time(nullptr);
            struct tm* tm_info = std::localtime(&t);
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            (*map)["year"]    = Value::Num(tm_info->tm_year + 1900);
            (*map)["month"]   = Value::Num(tm_info->tm_mon + 1);
            (*map)["day"]     = Value::Num(tm_info->tm_mday);
            (*map)["hour"]    = Value::Num(tm_info->tm_hour);
            (*map)["minute"]  = Value::Num(tm_info->tm_min);
            (*map)["second"]  = Value::Num(tm_info->tm_sec);
            (*map)["weekday"] = Value::Num(tm_info->tm_wday);
            return Value::MapOf(map);
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
// log 模块 — 内置结构化日志
// ═══════════════════════════════════════════════════════════
// 格式: [LEVEL] HH:MM:SS  {消息}
static std::string logTimestamp() {
    auto now = std::time(nullptr);
    struct tm* tm_info = std::localtime(&now);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

static Value logAtLevel(const std::string& level, const std::string& color,
                        const std::vector<Value>& args) {
    std::string msg;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) msg += " ";
        msg += args[i].toString();
    }
    std::cout << color << "[" << level << "] " << logTimestamp()
              << "  " << msg << "\033[0m\n";
    return Value::Nil();
}

static std::unordered_map<std::string, StdlibFn> makeLogModule() {
    return {
        {"info", [](std::vector<Value> args) -> Value {
            return logAtLevel("INFO", "\033[32m", args);
        }},
        {"warn", [](std::vector<Value> args) -> Value {
            return logAtLevel("WARN", "\033[33m", args);
        }},
        {"error", [](std::vector<Value> args) -> Value {
            return logAtLevel("ERROR", "\033[31m", args);
        }},
        {"debug", [](std::vector<Value> args) -> Value {
            return logAtLevel("DEBUG", "\033[90m", args);
        }},
    };
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
        {"sqrt", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::sqrt(args[0].number));
        }},
        {"tan", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::tan(args[0].number));
        }},
        {"atan", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::atan(args[0].number));
        }},
        {"atan2", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("Math.atan2(y, x) — requires 2 args");
            return Value::Num(std::atan2(args[0].number, args[1].number));
        }},
        {"log2", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::log2(args[0].number));
        }},
        {"log10", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            return Value::Num(std::log10(args[0].number));
        }},
        {"exp", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(1);
            return Value::Num(std::exp(args[0].number));
        }},
        {"clamp", [](std::vector<Value> args) -> Value {
            if (args.size() < 3) throw std::runtime_error("Math.clamp(val, lo, hi) — requires 3 args");
            double v = args[0].number, lo = args[1].number, hi = args[2].number;
            return Value::Num(v < lo ? lo : (v > hi ? hi : v));
        }},
        {"sign", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Num(0);
            double v = args[0].number;
            return Value::Num(v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0));
        }},
        {"random", [](std::vector<Value>) -> Value {
            return Value::Num((double)std::rand() / RAND_MAX);
        }},
        {"PI", [](std::vector<Value>) -> Value {
            return Value::Num(3.14159265358979323846);
        }},
        {"E", [](std::vector<Value>) -> Value {
            return Value::Num(2.71828182845904523536);
        }},
        {"INF", [](std::vector<Value>) -> Value {
            return Value::Num(std::numeric_limits<double>::infinity());
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
        // Set.diff(a, b) → Map (elements in a but not in b)
        {"diff", [](std::vector<Value> args) -> Value {
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            if (args.size() >= 2 && args[0].type == Value::Type::Map && args[0].map) {
                for (auto& kv : *args[0].map) {
                    bool inB = args[1].type == Value::Type::Map && args[1].map
                               && args[1].map->count(kv.first);
                    if (!inB) (*map)[kv.first] = Value::Bool(true);
                }
            }
            return Value::MapOf(map);
        }},
        // Set.equals(a, b) → Bool
        {"equals", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value::Bool(false);
            auto& a = args[0]; auto& b = args[1];
            if (a.type != Value::Type::Map || b.type != Value::Type::Map) return Value::Bool(false);
            if (!a.map || !b.map) return Value::Bool(a.map == b.map);
            if (a.map->size() != b.map->size()) return Value::Bool(false);
            for (auto& kv : *a.map)
                if (!b.map->count(kv.first)) return Value::Bool(false);
            return Value::Bool(true);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Env 模块 — 环境变量
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeEnvModule() {
    return {
        // Env.get(name) -> String | null
        {"get", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Env.get(name) — name must be a String");
            const char* val = std::getenv(args[0].string.c_str());
            return val ? Value::Str(val) : Value::Nil();
        }},
        // Env.set(name, value) -> Bool
        {"set", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String)
                throw std::runtime_error("Env.set(name, value) — name must be a String");
            return Value::Bool(setenv(args[0].string.c_str(),
                                      args[1].toString().c_str(), 1) == 0);
        }},
        // Env.has(name) -> Bool
        {"has", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Env.has(name) — name must be a String");
            return Value::Bool(std::getenv(args[0].string.c_str()) != nullptr);
        }},
        // Env.unset(name) -> Bool
        {"unset", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Env.unset(name) — name must be a String");
            return Value::Bool(unsetenv(args[0].string.c_str()) == 0);
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
// String 模块 — 字符串工具函数
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeStringModule() {
    return {
        // String.replace(str, old, new) → String
        {"replace", [](std::vector<Value> args) -> Value {
            if (args.size() < 3) throw std::runtime_error("String.replace(str, old, new)");
            std::string s = args[0].toString();
            std::string from = args[1].toString();
            std::string to = args[2].toString();
            if (from.empty()) return Value::Str(s);
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.length(), to);
                pos += to.length();
            }
            return Value::Str(s);
        }},
        // String.startsWith(str, prefix) → Bool
        {"startsWith", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value::Bool(false);
            std::string s = args[0].toString(), p = args[1].toString();
            return Value::Bool(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
        }},
        // String.endsWith(str, suffix) → Bool
        {"endsWith", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value::Bool(false);
            std::string s = args[0].toString(), p = args[1].toString();
            return Value::Bool(s.size() >= p.size()
                && s.compare(s.size() - p.size(), p.size(), p) == 0);
        }},
        // String.repeat(str, n) → String
        {"repeat", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("String.repeat(str, n)");
            std::string s = args[0].toString();
            int n = (int)args[1].number;
            std::string r;
            r.reserve(s.size() * n);
            for (int i = 0; i < n; i++) r += s;
            return Value::Str(r);
        }},
        // String.indexOf(str, sub) → Number (-1 if not found)
        {"indexOf", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value::Num(-1);
            size_t pos = args[0].toString().find(args[1].toString());
            return Value::Num(pos == std::string::npos ? -1.0 : (double)pos);
        }},
        // String.slice(str, start, end?) → String
        {"slice", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Str("");
            std::string s = args[0].toString();
            int start = args.size() > 1 ? (int)args[1].number : 0;
            int end = args.size() > 2 ? (int)args[2].number : (int)s.size();
            if (start < 0) start = std::max(0, (int)s.size() + start);
            if (end < 0) end = std::max(0, (int)s.size() + end);
            if (start >= (int)s.size() || start >= end) return Value::Str("");
            return Value::Str(s.substr(start, end - start));
        }},
        // String.charAt(str, index) → String (single char)
        {"charAt", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) return Value::Str("");
            std::string s = args[0].toString();
            int idx = (int)args[1].number;
            if (idx < 0 || idx >= (int)s.size()) return Value::Str("");
            return Value::Str(std::string(1, s[idx]));
        }},
        // String.padLeft(str, len, fill=" ") → String
        {"padLeft", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("String.padLeft(str, len, fill?)");
            std::string s = args[0].toString();
            int len = (int)args[1].number;
            std::string fill = args.size() > 2 ? args[2].toString() : " ";
            if (fill.empty()) fill = " ";
            while ((int)s.size() < len) s = fill + s;
            return Value::Str(s);
        }},
        // String.padRight(str, len, fill=" ") → String
        {"padRight", [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw std::runtime_error("String.padRight(str, len, fill?)");
            std::string s = args[0].toString();
            int len = (int)args[1].number;
            std::string fill = args.size() > 2 ? args[2].toString() : " ";
            if (fill.empty()) fill = " ";
            while ((int)s.size() < len) s += fill;
            return Value::Str(s);
        }},
        // String.chars(str) → Array[String] (split into individual characters)
        {"chars", [](std::vector<Value> args) -> Value {
            auto arr = std::make_shared<std::vector<Value>>();
            if (!args.empty()) {
                std::string s = args[0].toString();
                for (char c : s) arr->push_back(Value::Str(std::string(1, c)));
            }
            return Value::Arr(arr);
        }},
        // String.code(str) → Number (ASCII/Unicode code of first char)
        {"code", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].toString().empty()) return Value::Num(0);
            return Value::Num((double)(unsigned char)args[0].toString()[0]);
        }},
        // String.fromCode(n) → String (char from ASCII/Unicode code)
        {"fromCode", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Str("");
            return Value::Str(std::string(1, (char)(int)args[0].number));
        }},
        // String.reverse(str) → String
        {"reverse", [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value::Str("");
            std::string s = args[0].toString();
            std::reverse(s.begin(), s.end());
            return Value::Str(s);
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

// ═══════════════════════════════════════════════════════════
// Specify 模块 — 规格声明类型的内省与工具
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeSpecifyModule() {
    return {
        // Specify.describe(val) → 人类可读的规格描述（字符串）
        {"describe", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.describe(val) — val must be a Specify type");
            auto& sp = args[0].specify;
            std::string desc;
            desc += "Specify: " + sp->name + "\n";
            desc += "  Intent: " + sp->intent + "\n";
            if (sp->input.type != Value::Type::Nil)
                desc += "  Input: " + sp->input.toString() + "\n";
            if (sp->output.type != Value::Type::Nil)
                desc += "  Output: " + sp->output.toString() + "\n";
            if (sp->constraints.type == Value::Type::Array && sp->constraints.array) {
                desc += "  Constraints:\n";
                for (auto& c : *sp->constraints.array)
                    desc += "    - " + c.toString() + "\n";
            }
            if (sp->examples.type == Value::Type::Array && sp->examples.array) {
                desc += "  Examples: " + std::to_string(sp->examples.array->size()) + " defined\n";
            }
            return Value::Str(desc);
        }},
        // Specify.schema(val) → JSON schema 格式的结构化输出（Map）
        {"schema", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.schema(val) — val must be a Specify type");
            auto& sp = args[0].specify;
            auto m = std::make_shared<std::unordered_map<std::string, Value>>();
            (*m)["name"]        = Value::Str(sp->name);
            (*m)["intent"]      = Value::Str(sp->intent);
            (*m)["input"]       = sp->input;
            (*m)["output"]      = sp->output;
            (*m)["constraints"] = sp->constraints;
            (*m)["examples"]    = sp->examples;
            return Value::MapOf(m);
        }},
        // Specify.intent(val) → 获取意图字符串
        {"intent", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.intent(val) — val must be a Specify type");
            return Value::Str(args[0].specify->intent);
        }},
        // Specify.constraints(val) → 获取约束列表
        {"constraints", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.constraints(val) — val must be a Specify type");
            return args[0].specify->constraints;
        }},
        // Specify.examples(val) → 获取示例列表
        {"examples", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.examples(val) — val must be a Specify type");
            return args[0].specify->examples;
        }},
        // Specify.validate(val, inputMap) → 检查输入是否满足约束
        {"validate", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::Specify || !args[0].specify)
                throw std::runtime_error("Specify.validate(val, input) — requires Specify type and input");
            // 基本验证：检查 input 是否包含 schema 要求的字段
            auto& sp = args[0].specify;
            if (sp->input.type == Value::Type::Map && sp->input.map &&
                args[1].type == Value::Type::Map && args[1].map) {
                for (auto& kv : *sp->input.map) {
                    if (args[1].map->find(kv.first) == args[1].map->end())
                        return Value::Bool(false);
                }
            }
            return Value::Bool(true);
        }},
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
    // Log 模块已移除，使用 specify 替代
    registerStdlibModule("Env",    makeEnvModule());
    registerStdlibModule("Test",   makeTestModule());
    registerStdlibModule("String", makeStringModule());
    registerStdlibModule("hw",     makeHwModule());
    registerStdlibModule("Specify", makeSpecifyModule());
}
