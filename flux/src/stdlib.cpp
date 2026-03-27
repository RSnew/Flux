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
#include <regex>      // std::regex, std::regex_search, std::regex_replace

namespace fs = std::filesystem;

// POSIX socket（Http 模块使用）
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/utsname.h>  // uname()
#include <sys/wait.h>     // WEXITSTATUS

// OpenSSL（HTTPS 支持 + Crypto 模块）
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

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

// ── Value → JSON 可序列化 Value ──────────────────────────
// StructType → Map{fieldName: defaultType}，StructInst → Map{fieldName: value}
// 其他不可序列化类型 → String（toString）
static Value valueToSerializable(const Value& v) {
    if (v.type == Value::Type::StructType && v.structType) {
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
        for (auto& field : v.structType->fields) {
            // 默认表达式是 AST 节点，尝试提取字面量
            if (field.defaultExpr) {
                // 尝试 StringLiteral
                if (auto* sl = dynamic_cast<StringLit*>(field.defaultExpr.get()))
                    (*m)[field.name] = Value::Str(sl->value);
                else if (auto* nl = dynamic_cast<NumberLit*>(field.defaultExpr.get()))
                    (*m)[field.name] = Value::Num(nl->value);
                else if (auto* bl = dynamic_cast<BoolLit*>(field.defaultExpr.get()))
                    (*m)[field.name] = Value::Bool(bl->value);
                else
                    (*m)[field.name] = Value::Str("<expr>");
            } else {
                (*m)[field.name] = Value::Nil();
            }
        }
        return Value::MapOf(m);
    }
    if (v.type == Value::Type::StructInst && v.structInst) {
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
        for (auto& [k, val] : v.structInst->fields)
            (*m)[k] = valueToSerializable(val);
        return Value::MapOf(m);
    }
    // Interface, Function, Future 等 → 用 toString
    if (v.type == Value::Type::Interface || v.type == Value::Type::Function ||
        v.type == Value::Type::Future || v.type == Value::Type::Chan)
        return Value::Str(v.toString());
    return v;
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
            (*m)["input"]       = valueToSerializable(sp->input);
            (*m)["output"]      = valueToSerializable(sp->output);
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

// ═══════════════════════════════════════════════════════════
// Server 模块 — 动态路由 HTTP 开发服务器
// ═══════════════════════════════════════════════════════════

// 服务器状态（C++ 侧持有，通过 Addr 指针暴露给 Flux）
struct ServerRoute {
    std::string method;           // GET/POST/PUT/DELETE
    std::string pattern;          // e.g. "/api/users/:id"
    std::vector<std::string> paramNames;  // 从 pattern 提取的参数名
    std::string regexPattern;     // 转换后的正则/匹配模式
    std::shared_ptr<FuncVal> handler;     // Flux 回调函数
};

struct ServerState {
    int port = 8080;
    std::vector<ServerRoute> routes;
    Interpreter* interp = nullptr;  // 用于回调 Flux 函数
};

// 解析路径模式，提取参数名和生成匹配模式
static void parseRoutePattern(ServerRoute& route) {
    std::string pat = route.pattern;
    route.paramNames.clear();
    // 将 /users/:id/posts/:postId 拆分为段，记录参数段
    // 匹配时按段比较
    // 我们不用正则，直接按 '/' 分割对比
}

// 按 '/' 分割路径为段列表
static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    std::string seg;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '/') {
            if (!seg.empty()) { segments.push_back(seg); seg.clear(); }
        } else {
            seg += path[i];
        }
    }
    if (!seg.empty()) segments.push_back(seg);
    return segments;
}

// 解析查询参数 ?key=val&key2=val2
static std::shared_ptr<std::unordered_map<std::string, Value>>
parseQueryParams(const std::string& query) {
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    if (query.empty()) return m;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t eq = query.find('=', pos);
        size_t amp = query.find('&', pos);
        if (eq == std::string::npos || (amp != std::string::npos && amp < eq)) {
            // key without value
            std::string key = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
            if (!key.empty()) (*m)[key] = Value::Str("");
            pos = (amp == std::string::npos) ? query.size() : amp + 1;
        } else {
            std::string key = query.substr(pos, eq - pos);
            size_t valEnd = (amp == std::string::npos) ? query.size() : amp;
            std::string val = query.substr(eq + 1, valEnd - eq - 1);
            if (!key.empty()) (*m)[key] = Value::Str(val);
            pos = (amp == std::string::npos) ? query.size() : amp + 1;
        }
    }
    return m;
}

// URL decode (basic: %20 → space, + → space)
static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            char c1 = s[i+1], c2 = s[i+2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            out += (char)((hi << 4) | lo);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// 解析 HTTP 请求头
static std::shared_ptr<std::unordered_map<std::string, Value>>
parseHeaders(const std::string& raw) {
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    // 跳过第一行（请求行）
    size_t pos = raw.find("\r\n");
    if (pos == std::string::npos) return m;
    pos += 2;
    while (pos < raw.size()) {
        size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string::npos || lineEnd == pos) break;  // 空行 = 头结束
        std::string line = raw.substr(pos, lineEnd - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // trim leading whitespace
            while (!val.empty() && val[0] == ' ') val = val.substr(1);
            // lowercase the key for consistency
            for (auto& c : key) c = tolower(c);
            (*m)[key] = Value::Str(val);
        }
        pos = lineEnd + 2;
    }
    return m;
}

// 路由匹配：返回是否匹配，以及提取的参数
static bool matchRoute(const ServerRoute& route, const std::string& path,
                       std::shared_ptr<std::unordered_map<std::string, Value>>& params) {
    auto patSegs = splitPath(route.pattern);
    auto pathSegs = splitPath(path);

    if (patSegs.size() != pathSegs.size()) return false;

    params = std::make_shared<std::unordered_map<std::string, Value>>();
    for (size_t i = 0; i < patSegs.size(); i++) {
        if (!patSegs[i].empty() && patSegs[i][0] == ':') {
            // 参数段
            std::string paramName = patSegs[i].substr(1);
            (*params)[paramName] = Value::Str(urlDecode(pathSegs[i]));
        } else if (patSegs[i] == "*") {
            // 通配符
            continue;
        } else if (patSegs[i] != pathSegs[i]) {
            return false;
        }
    }
    return true;
}

// 全局 ServerState 注册表（通过 Addr 指针管理生命周期）
static std::mutex g_serverMutex;
static std::vector<std::unique_ptr<ServerState>> g_servers;

static ServerState* getServerFromAddr(Value& v) {
    if (v.type != Value::Type::Addr || v.addr == 0)
        throw std::runtime_error("Server: invalid server handle");
    return reinterpret_cast<ServerState*>(v.addr);
}

// ═══════════════════════════════════════════════════════════
// Regex 模块 — C++ <regex> 包装
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeRegexModule() {
    return {
        // Regex.match(pattern, str) → Bool
        {"match", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.match(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                return Value::Bool(std::regex_search(args[1].string, re));
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.match: invalid pattern — ") + e.what());
            }
        }},

        // Regex.isMatch(pattern, str) → Bool  (alias for match)
        // Note: "test" is a reserved keyword in Flux, so we use "isMatch" instead.
        {"isMatch", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.isMatch(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                return Value::Bool(std::regex_search(args[1].string, re));
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.isMatch: invalid pattern — ") + e.what());
            }
        }},

        // Regex.find(pattern, str) → String | null
        {"find", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.find(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                std::smatch m;
                if (std::regex_search(args[1].string, m, re))
                    return Value::Str(m[0].str());
                return Value::Nil();
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.find: invalid pattern — ") + e.what());
            }
        }},

        // Regex.findAll(pattern, str) → Array[String]
        {"findAll", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.findAll(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                auto arr = std::make_shared<std::vector<Value>>();
                auto begin = std::sregex_iterator(args[1].string.begin(),
                                                   args[1].string.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it)
                    arr->push_back(Value::Str((*it)[0].str()));
                return Value::Arr(arr);
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.findAll: invalid pattern — ") + e.what());
            }
        }},

        // Regex.replace(pattern, str, replacement) → String
        {"replace", [](std::vector<Value> args) -> Value {
            if (args.size() < 3 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String
                                || args[2].type != Value::Type::String)
                throw std::runtime_error("Regex.replace(pattern, str, replacement) — requires 3 String args");
            try {
                std::regex re(args[0].string);
                return Value::Str(std::regex_replace(args[1].string, re, args[2].string));
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.replace: invalid pattern — ") + e.what());
            }
        }},

        // Regex.split(pattern, str) → Array[String]
        {"split", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.split(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                auto arr = std::make_shared<std::vector<Value>>();
                std::sregex_token_iterator begin(args[1].string.begin(),
                                                  args[1].string.end(), re, -1);
                std::sregex_token_iterator end;
                for (auto it = begin; it != end; ++it)
                    arr->push_back(Value::Str(it->str()));
                return Value::Arr(arr);
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.split: invalid pattern — ") + e.what());
            }
        }},

        // Regex.groups(pattern, str) → Array[String]  (capture groups, excluding group 0)
        {"groups", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Regex.groups(pattern, str) — requires 2 String args");
            try {
                std::regex re(args[0].string);
                std::smatch m;
                auto arr = std::make_shared<std::vector<Value>>();
                if (std::regex_search(args[1].string, m, re)) {
                    for (size_t i = 1; i < m.size(); ++i)
                        arr->push_back(Value::Str(m[i].str()));
                }
                return Value::Arr(arr);
            } catch (const std::regex_error& e) {
                throw std::runtime_error(std::string("Regex.groups: invalid pattern — ") + e.what());
            }
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// OS 模块 — 进程执行与系统交互
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeOSModule() {
    return {
        // OS.exec(command) → String — 执行命令，返回 stdout
        {"exec", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("OS.exec(command) — command must be a String");
            std::string cmd = args[0].string;
            std::string result;
            {
                GILRelease release;
                FILE* pipe = popen(cmd.c_str(), "r");
                if (!pipe)
                    throw std::runtime_error("OS.exec: failed to execute command");
                char buf[4096];
                while (fgets(buf, sizeof(buf), pipe))
                    result += buf;
                pclose(pipe);
            }
            // 去掉尾部换行
            while (!result.empty() && result.back() == '\n')
                result.pop_back();
            return Value::Str(result);
        }},

        // OS.execStatus(command) → Number — 执行命令，返回退出码
        {"execStatus", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("OS.execStatus(command) — command must be a String");
            std::string cmd = args[0].string;
            int exitCode;
            {
                GILRelease release;
                int status = system(cmd.c_str());
                #ifdef _WIN32
                exitCode = status;
                #else
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                #endif
            }
            return Value::Num((double)exitCode);
        }},

        // OS.pid() → Number — 当前进程 ID
        {"pid", [](std::vector<Value>) -> Value {
            return Value::Num((double)getpid());
        }},

        // OS.platform() → String — "darwin", "linux", etc.
        {"platform", [](std::vector<Value>) -> Value {
            struct utsname info;
            if (uname(&info) != 0)
                return Value::Str("unknown");
            std::string sys = info.sysname;
            std::transform(sys.begin(), sys.end(), sys.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            return Value::Str(sys);
        }},

        // OS.arch() → String — "arm64", "x86_64", etc.
        {"arch", [](std::vector<Value>) -> Value {
            struct utsname info;
            if (uname(&info) != 0)
                return Value::Str("unknown");
            return Value::Str(info.machine);
        }},

        // OS.hostname() → String
        {"hostname", [](std::vector<Value>) -> Value {
            char buf[256];
            if (gethostname(buf, sizeof(buf)) != 0)
                return Value::Str("unknown");
            return Value::Str(buf);
        }},

        // OS.cwd() → String — 当前工作目录
        {"cwd", [](std::vector<Value>) -> Value {
            char buf[4096];
            if (getcwd(buf, sizeof(buf)) == nullptr)
                throw std::runtime_error("OS.cwd: failed to get current directory");
            return Value::Str(buf);
        }},

        // OS.chdir(path) → Bool
        {"chdir", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("OS.chdir(path) — path must be a String");
            return Value::Bool(chdir(args[0].string.c_str()) == 0);
        }},

        // OS.args() → Array[String] — 命令行参数（占位）
        {"args", [](std::vector<Value>) -> Value {
            return Value::Array();
        }},

        // OS.exit(code) → Nil — 退出进程
        {"exit", [](std::vector<Value> args) -> Value {
            int code = 0;
            if (!args.empty() && args[0].type == Value::Type::Number)
                code = (int)args[0].number;
            std::exit(code);
            return Value::Nil();
        }},

        // OS.signal(sig, handler) → Nil — 信号处理占位
        {"signal", [](std::vector<Value>) -> Value {
            return Value::Nil();
        }},

        // OS.tmpdir() → String — 临时目录路径
        {"tmpdir", [](std::vector<Value>) -> Value {
            const char* tmp = std::getenv("TMPDIR");
            if (tmp) return Value::Str(tmp);
            tmp = std::getenv("TMP");
            if (tmp) return Value::Str(tmp);
            tmp = std::getenv("TEMP");
            if (tmp) return Value::Str(tmp);
            return Value::Str("/tmp");
        }},

        // OS.homedir() → String — 用户主目录
        {"homedir", [](std::vector<Value>) -> Value {
            const char* home = std::getenv("HOME");
            if (home) return Value::Str(home);
            #ifdef _WIN32
            const char* userprofile = std::getenv("USERPROFILE");
            if (userprofile) return Value::Str(userprofile);
            #endif
            return Value::Str("");
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Crypto 模块 — OpenSSL 加密 / 哈希 / 编码
// ═══════════════════════════════════════════════════════════

// 辅助：字节数组转十六进制字符串
static std::string bytesToHex(const unsigned char* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex[(data[i] >> 4) & 0x0F];
        result += hex[data[i] & 0x0F];
    }
    return result;
}

// 辅助：通用 EVP 摘要
static std::string evpDigest(const std::string& algo, const std::string& input) {
    const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
    if (!md)
        throw std::runtime_error("Crypto: unknown digest algorithm: " + algo);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("Crypto: EVP_MD_CTX_new failed");
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hashLen);
    EVP_MD_CTX_free(ctx);
    return bytesToHex(hash, hashLen);
}

static std::unordered_map<std::string, StdlibFn> makeCryptoModule() {
    return {
        // Crypto.sha256(str) → String (hex)
        {"sha256", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Crypto.sha256(str) — str must be a String");
            return Value::Str(evpDigest("sha256", args[0].string));
        }},

        // Crypto.sha512(str) → String (hex)
        {"sha512", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Crypto.sha512(str) — str must be a String");
            return Value::Str(evpDigest("sha512", args[0].string));
        }},

        // Crypto.md5(str) → String (hex)
        {"md5", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Crypto.md5(str) — str must be a String");
            return Value::Str(evpDigest("md5", args[0].string));
        }},

        // Crypto.hmac(algo, key, data) → String (hex)
        {"hmac", [](std::vector<Value> args) -> Value {
            if (args.size() < 3 ||
                args[0].type != Value::Type::String ||
                args[1].type != Value::Type::String ||
                args[2].type != Value::Type::String)
                throw std::runtime_error("Crypto.hmac(algo, key, data) — all args must be Strings");
            const std::string& algo = args[0].string;
            const std::string& key  = args[1].string;
            const std::string& data = args[2].string;
            const EVP_MD* md = EVP_get_digestbyname(algo.c_str());
            if (!md)
                throw std::runtime_error("Crypto.hmac: unknown algorithm: " + algo);
            unsigned char result[EVP_MAX_MD_SIZE];
            unsigned int resultLen = 0;
            HMAC(md,
                 key.data(), (int)key.size(),
                 (const unsigned char*)data.data(), data.size(),
                 result, &resultLen);
            return Value::Str(bytesToHex(result, resultLen));
        }},

        // Crypto.base64encode(str) → String
        {"base64encode", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Crypto.base64encode(str) — str must be a String");
            const std::string& input = args[0].string;
            size_t outLen = 4 * ((input.size() + 2) / 3) + 1;
            std::vector<unsigned char> out(outLen);
            int written = EVP_EncodeBlock(out.data(),
                                          (const unsigned char*)input.data(),
                                          (int)input.size());
            return Value::Str(std::string(out.begin(), out.begin() + written));
        }},

        // Crypto.base64decode(str) → String
        {"base64decode", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::String)
                throw std::runtime_error("Crypto.base64decode(str) — str must be a String");
            const std::string& input = args[0].string;
            size_t outLen = 3 * ((input.size() + 3) / 4) + 1;
            std::vector<unsigned char> out(outLen);
            int written = EVP_DecodeBlock(out.data(),
                                          (const unsigned char*)input.data(),
                                          (int)input.size());
            if (written < 0)
                throw std::runtime_error("Crypto.base64decode: invalid base64 input");
            // Trim padding bytes caused by '=' chars
            size_t padding = 0;
            if (input.size() >= 1 && input.back() == '=') ++padding;
            if (input.size() >= 2 && input[input.size() - 2] == '=') ++padding;
            return Value::Str(std::string(out.begin(), out.begin() + written - padding));
        }},

        // Crypto.uuid() → String (v4 UUID)
        {"uuid", [](std::vector<Value> /*args*/) -> Value {
            unsigned char bytes[16];
            if (RAND_bytes(bytes, 16) != 1)
                throw std::runtime_error("Crypto.uuid: RAND_bytes failed");
            bytes[6] = (bytes[6] & 0x0F) | 0x40;  // version 4
            bytes[8] = (bytes[8] & 0x3F) | 0x80;  // variant 1
            char buf[37];
            std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5],
                bytes[6], bytes[7],
                bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
            return Value::Str(std::string(buf));
        }},

        // Crypto.randomBytes(n) → String (hex-encoded)
        {"randomBytes", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Crypto.randomBytes(n) — n must be a Number");
            int n = (int)args[0].number;
            if (n <= 0 || n > 1024)
                throw std::runtime_error("Crypto.randomBytes(n) — n must be 1..1024");
            std::vector<unsigned char> bytes(n);
            if (RAND_bytes(bytes.data(), n) != 1)
                throw std::runtime_error("Crypto.randomBytes: RAND_bytes failed");
            return Value::Str(bytesToHex(bytes.data(), bytes.size()));
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// Socket 模块 — POSIX TCP/UDP 原始套接字
// ═══════════════════════════════════════════════════════════
static std::unordered_map<std::string, StdlibFn> makeSocketModule() {
    return {
        // ── TCP Client ──────────────────────────────────────

        // Socket.connect(host, port) → Number (socket fd)
        {"connect", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::String
                                || args[1].type != Value::Type::Number)
                throw std::runtime_error("Socket.connect(host, port) — host must be String, port must be Number");
            std::string host = args[0].string;
            int port = (int)args[1].number;

            struct sockaddr_storage addr{};
            socklen_t addrLen = 0;
            if (!dnsLookupCached(host, port, addr, addrLen))
                throw std::runtime_error("Socket.connect: DNS lookup failed for '" + host + "'");

            int fd;
            {
                GILRelease release;
                fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
                if (fd < 0) throw std::runtime_error("Socket.connect: socket() failed");
                if (::connect(fd, (struct sockaddr*)&addr, addrLen) < 0) {
                    ::close(fd);
                    throw std::runtime_error("Socket.connect: connect failed to " + host + ":" + std::to_string(port));
                }
            }
            return Value::Num((double)fd);
        }},

        // Socket.send(sock, data) → Number (bytes sent)
        {"send", [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || args[0].type != Value::Type::Number
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Socket.send(sock, data) — sock must be Number, data must be String");
            int fd = (int)args[0].number;
            const std::string& data = args[1].string;
            ssize_t sent;
            { GILRelease release; sent = ::send(fd, data.c_str(), data.size(), 0); }
            if (sent < 0) throw std::runtime_error("Socket.send: send() failed");
            return Value::Num((double)sent);
        }},

        // Socket.recv(sock, maxBytes) → String
        {"recv", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Socket.recv(sock, maxBytes) — sock must be Number");
            int fd = (int)args[0].number;
            int maxBytes = (args.size() > 1 && args[1].type == Value::Type::Number)
                           ? (int)args[1].number : 4096;
            if (maxBytes <= 0 || maxBytes > 1048576) maxBytes = 4096;

            std::vector<char> buf(maxBytes);
            ssize_t n;
            { GILRelease release; n = ::recv(fd, buf.data(), buf.size(), 0); }
            if (n < 0) throw std::runtime_error("Socket.recv: recv() failed");
            return Value::Str(std::string(buf.data(), (size_t)n));
        }},

        // Socket.close(sock) → Bool
        {"close", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Socket.close(sock) — sock must be Number");
            int fd = (int)args[0].number;
            int rc = ::close(fd);
            return Value::Bool(rc == 0);
        }},

        // ── TCP Server ──────────────────────────────────────

        // Socket.listen(port, backlog=10) → Number (server fd)
        {"listen", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Socket.listen(port, backlog=10) — port must be Number");
            int port = (int)args[0].number;
            int backlog = (args.size() > 1 && args[1].type == Value::Type::Number)
                          ? (int)args[1].number : 10;

            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) throw std::runtime_error("Socket.listen: socket() failed");

            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons((uint16_t)port);

            if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                ::close(fd);
                throw std::runtime_error("Socket.listen: bind() failed on port " + std::to_string(port));
            }
            if (::listen(fd, backlog) < 0) {
                ::close(fd);
                throw std::runtime_error("Socket.listen: listen() failed");
            }
            return Value::Num((double)fd);
        }},

        // Socket.accept(serverSock) → Number (client fd)
        {"accept", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Socket.accept(serverSock) — serverSock must be Number");
            int serverFd = (int)args[0].number;
            struct sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd;
            { GILRelease release; clientFd = ::accept(serverFd, (struct sockaddr*)&clientAddr, &clientLen); }
            if (clientFd < 0) throw std::runtime_error("Socket.accept: accept() failed");
            return Value::Num((double)clientFd);
        }},

        // ── UDP ─────────────────────────────────────────────

        // Socket.udpNew() → Number (udp socket fd)
        {"udpNew", [](std::vector<Value>) -> Value {
            int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) throw std::runtime_error("Socket.udpNew: socket() failed");
            return Value::Num((double)fd);
        }},

        // Socket.sendTo(sock, host, port, data) → Number (bytes sent)
        {"sendTo", [](std::vector<Value> args) -> Value {
            if (args.size() < 4 || args[0].type != Value::Type::Number
                                || args[1].type != Value::Type::String
                                || args[2].type != Value::Type::Number
                                || args[3].type != Value::Type::String)
                throw std::runtime_error("Socket.sendTo(sock, host, port, data)");
            int fd = (int)args[0].number;
            std::string host = args[1].string;
            int port = (int)args[2].number;
            const std::string& data = args[3].string;

            struct sockaddr_storage addr{};
            socklen_t addrLen = 0;
            if (!dnsLookupCached(host, port, addr, addrLen))
                throw std::runtime_error("Socket.sendTo: DNS lookup failed for '" + host + "'");

            ssize_t sent;
            { GILRelease release; sent = ::sendto(fd, data.c_str(), data.size(), 0, (struct sockaddr*)&addr, addrLen); }
            if (sent < 0) throw std::runtime_error("Socket.sendTo: sendto() failed");
            return Value::Num((double)sent);
        }},

        // Socket.recvFrom(sock, maxBytes) → Map { data, host, port }
        {"recvFrom", [](std::vector<Value> args) -> Value {
            if (args.empty() || args[0].type != Value::Type::Number)
                throw std::runtime_error("Socket.recvFrom(sock, maxBytes)");
            int fd = (int)args[0].number;
            int maxBytes = (args.size() > 1 && args[1].type == Value::Type::Number)
                           ? (int)args[1].number : 4096;
            if (maxBytes <= 0 || maxBytes > 1048576) maxBytes = 4096;

            std::vector<char> buf(maxBytes);
            struct sockaddr_in srcAddr{};
            socklen_t srcLen = sizeof(srcAddr);
            ssize_t n;
            { GILRelease release; n = ::recvfrom(fd, buf.data(), buf.size(), 0, (struct sockaddr*)&srcAddr, &srcLen); }
            if (n < 0) throw std::runtime_error("Socket.recvFrom: recvfrom() failed");

            auto m = std::make_shared<std::unordered_map<std::string, Value>>();
            (*m)["data"] = Value::Str(std::string(buf.data(), (size_t)n));

            char hostBuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &srcAddr.sin_addr, hostBuf, sizeof(hostBuf));
            (*m)["host"] = Value::Str(std::string(hostBuf));
            (*m)["port"] = Value::Num((double)ntohs(srcAddr.sin_port));

            return Value::MapOf(m);
        }},

        // ── Utility ─────────────────────────────────────────

        // Socket.setOption(sock, option, value) → Bool
        {"setOption", [](std::vector<Value> args) -> Value {
            if (args.size() < 3 || args[0].type != Value::Type::Number
                                || args[1].type != Value::Type::String)
                throw std::runtime_error("Socket.setOption(sock, option, value)");
            int fd = (int)args[0].number;
            std::string option = args[1].string;

            int optName = 0;
            int level = SOL_SOCKET;
            if (option == "SO_REUSEADDR")       optName = SO_REUSEADDR;
            else if (option == "SO_REUSEPORT")  optName = SO_REUSEPORT;
            else if (option == "SO_KEEPALIVE")  optName = SO_KEEPALIVE;
            else if (option == "SO_RCVBUF")     optName = SO_RCVBUF;
            else if (option == "SO_SNDBUF")     optName = SO_SNDBUF;
            else if (option == "SO_BROADCAST")  optName = SO_BROADCAST;
            else throw std::runtime_error("Socket.setOption: unknown option '" + option + "'");

            int val = 0;
            if (args[2].type == Value::Type::Number)   val = (int)args[2].number;
            else if (args[2].type == Value::Type::Bool) val = args[2].boolean ? 1 : 0;
            else throw std::runtime_error("Socket.setOption: value must be Number or Bool");

            int rc = ::setsockopt(fd, level, optName, &val, sizeof(val));
            return Value::Bool(rc == 0);
        }},
    };
}

// ═══════════════════════════════════════════════════════════
// WS 模块 — WebSocket 客户端 (RFC 6455)
// ═══════════════════════════════════════════════════════════

struct WSConnection {
    int         fd = -1;
    SSL*        ssl = nullptr;
    bool        useTLS = false;
    bool        open = false;
    std::mutex  mu;

    int rawSend(const void* data, size_t len) {
        if (useTLS && ssl) return SSL_write(ssl, data, (int)len);
        return (int)::send(fd, data, len, 0);
    }
    int rawRecv(void* buf, size_t len) {
        if (useTLS && ssl) return SSL_read(ssl, buf, (int)len);
        return (int)::recv(fd, buf, len, 0);
    }
    bool readExact(void* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = rawRecv((char*)buf + got, n - got);
            if (r <= 0) return false;
            got += (size_t)r;
        }
        return true;
    }
    void cleanup() {
        open = false;
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
};

static std::mutex g_wsMutex;
static std::vector<std::unique_ptr<WSConnection>> g_wsConns;

static WSConnection* getWS(Value& v) {
    if (v.type != Value::Type::Addr || v.addr == 0)
        throw std::runtime_error("WS: invalid WebSocket handle");
    return reinterpret_cast<WSConnection*>(v.addr);
}

static bool wsSendFrame(WSConnection* ws, uint8_t opcode, const std::string& payload) {
    std::lock_guard<std::mutex> lk(ws->mu);
    if (!ws->open) return false;
    size_t plen = payload.size();
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode);
    if (plen <= 125) {
        frame.push_back(0x80 | (uint8_t)plen);
    } else if (plen <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)(plen >> 8));
        frame.push_back((uint8_t)(plen & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((uint8_t)((plen >> (i * 8)) & 0xFF));
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(std::rand() & 0xFF);
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < plen; i++)
        frame.push_back((uint8_t)payload[i] ^ mask[i % 4]);
    int sent = ws->rawSend(frame.data(), frame.size());
    return sent == (int)frame.size();
}

static std::pair<uint8_t, std::string> wsRecvFrame(WSConnection* ws) {
    uint8_t hdr[2];
    if (!ws->readExact(hdr, 2)) return {0, ""};
    uint8_t opcode = hdr[0] & 0x0F;
    bool    masked = (hdr[1] & 0x80) != 0;
    uint64_t plen  = hdr[1] & 0x7F;
    if (plen == 126) {
        uint8_t ext[2];
        if (!ws->readExact(ext, 2)) return {0, ""};
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (!ws->readExact(ext, 8)) return {0, ""};
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    uint8_t mask[4] = {};
    if (masked && !ws->readExact(mask, 4)) return {0, ""};
    if (plen > 16 * 1024 * 1024) return {0, ""};
    std::string payload(plen, '\0');
    if (plen > 0) {
        if (!ws->readExact(&payload[0], plen)) return {0, ""};
        if (masked) {
            for (size_t i = 0; i < plen; i++)
                payload[i] ^= mask[i % 4];
        }
    }
    return {opcode, std::move(payload)};
}

static std::unordered_map<std::string, StdlibFn> makeWebSocketModule() {
    std::unordered_map<std::string, StdlibFn> m;

    m["connect"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type != Value::Type::String)
            throw std::runtime_error("WS.connect(url) — url must be a String");
        std::string url = args[0].string;
        bool useTLS = false;
        std::string u = url;
        if (u.size() >= 6 && u.substr(0, 6) == "wss://") {
            useTLS = true; u = u.substr(6);
        } else if (u.size() >= 5 && u.substr(0, 5) == "ws://") {
            u = u.substr(5);
        } else {
            throw std::runtime_error("WS.connect: url must start with ws:// or wss://");
        }
        int port = useTLS ? 443 : 80;
        size_t sl = u.find('/');
        std::string host = (sl == std::string::npos) ? u : u.substr(0, sl);
        std::string path = (sl == std::string::npos) ? "/" : u.substr(sl);
        size_t co = host.find(':');
        if (co != std::string::npos) {
            port = std::stoi(host.substr(co + 1));
            host = host.substr(0, co);
        }

        struct sockaddr_storage saddr{};
        socklen_t addrLen = 0;
        if (!dnsLookupCached(host, port, saddr, addrLen))
            throw std::runtime_error("WS.connect: DNS lookup failed for '" + host + "'");
        int sock = ::socket(saddr.ss_family, SOCK_STREAM, 0);
        if (sock < 0) throw std::runtime_error("WS.connect: socket() failed");
        if (::connect(sock, (struct sockaddr*)&saddr, addrLen) < 0) {
            ::close(sock);
            throw std::runtime_error("WS.connect: TCP connect failed to " + host);
        }

        SSL* ssl = nullptr;
        if (useTLS) {
            SSL_CTX* ctx = getGlobalSSLCtx();
            if (!ctx) { ::close(sock); throw std::runtime_error("WS.connect: SSL_CTX not available"); }
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, sock);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl); ::close(sock);
                throw std::runtime_error("WS.connect: TLS handshake failed with " + host);
            }
        }

        auto conn = std::make_unique<WSConnection>();
        conn->fd = sock; conn->ssl = ssl; conn->useTLS = useTLS; conn->open = true;

        // Sec-WebSocket-Key: 16 random bytes → base64 (OpenSSL)
        unsigned char keyBytes[16];
        for (int i = 0; i < 16; i++) keyBytes[i] = (unsigned char)(std::rand() & 0xFF);
        unsigned char keyB64[32];
        int keyLen = EVP_EncodeBlock(keyB64, keyBytes, 16);
        std::string wsKey((char*)keyB64, keyLen);

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wsKey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        int sent = conn->rawSend(req.c_str(), req.size());
        if (sent <= 0) {
            conn->cleanup();
            throw std::runtime_error("WS.connect: failed to send upgrade request");
        }

        std::string response;
        char buf[1];
        while (response.find("\r\n\r\n") == std::string::npos) {
            int r = conn->rawRecv(buf, 1);
            if (r <= 0) {
                conn->cleanup();
                throw std::runtime_error("WS.connect: connection closed during handshake");
            }
            response += buf[0];
            if (response.size() > 4096) {
                conn->cleanup();
                throw std::runtime_error("WS.connect: handshake response too large");
            }
        }
        if (response.find("101") == std::string::npos) {
            conn->cleanup();
            throw std::runtime_error("WS.connect: server did not return 101\n" + response.substr(0, 200));
        }

        WSConnection* ptr = conn.get();
        { std::lock_guard<std::mutex> lk(g_wsMutex); g_wsConns.push_back(std::move(conn)); }
        return Value::AddrVal(reinterpret_cast<uintptr_t>(ptr));
    };

    m["send"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("WS.send(ws, message) — requires 2 args");
        auto* ws = getWS(args[0]);
        std::string msg = args[1].toString();
        bool ok;
        { GILRelease release; ok = wsSendFrame(ws, 0x01, msg); }
        return Value::Bool(ok);
    };

    m["recv"] = [](std::vector<Value> args) -> Value {
        if (args.empty())
            throw std::runtime_error("WS.recv(ws) — requires ws handle");
        auto* ws = getWS(args[0]);
        std::pair<uint8_t, std::string> frame;
        { GILRelease release; frame = wsRecvFrame(ws); }
        uint8_t opcode = frame.first;
        if (opcode == 0x08 || (opcode == 0 && frame.second.empty())) {
            ws->open = false;
            return Value::Nil();
        }
        if (opcode == 0x09) {
            wsSendFrame(ws, 0x0A, frame.second);
            { GILRelease release; frame = wsRecvFrame(ws); }
            if (frame.first == 0x08 || (frame.first == 0 && frame.second.empty())) {
                ws->open = false; return Value::Nil();
            }
        }
        return Value::Str(std::move(frame.second));
    };

    m["close"] = [](std::vector<Value> args) -> Value {
        if (args.empty())
            throw std::runtime_error("WS.close(ws) — requires ws handle");
        auto* ws = getWS(args[0]);
        if (!ws->open) return Value::Bool(false);
        wsSendFrame(ws, 0x08, "");
        ws->cleanup();
        return Value::Bool(true);
    };

    m["isOpen"] = [](std::vector<Value> args) -> Value {
        if (args.empty())
            throw std::runtime_error("WS.isOpen(ws) — requires ws handle");
        auto* ws = getWS(args[0]);
        return Value::Bool(ws->open);
    };

    return m;
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
    registerStdlibModule("Regex",   makeRegexModule());
    registerStdlibModule("OS",      makeOSModule());
    registerStdlibModule("Crypto",  makeCryptoModule());
    registerStdlibModule("Socket",  makeSocketModule());
    registerStdlibModule("WS",      makeWebSocketModule());
    registerStdlibModule("log",     makeLogModule());

    // ── Server 模块（动态路由 HTTP 服务器）────────────────────
    Interpreter* self = this;
    std::unordered_map<std::string, StdlibFn> serverMod;

    // Server.new(port) → Addr (server handle)
    serverMod["new"] = [self](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type != Value::Type::Number)
            throw std::runtime_error("Server.new(port) — port must be a Number");
        auto state = std::make_unique<ServerState>();
        state->port = (int)args[0].number;
        state->interp = self;
        ServerState* ptr = state.get();
        {
            std::lock_guard<std::mutex> lk(g_serverMutex);
            g_servers.push_back(std::move(state));
        }
        return Value::AddrVal(reinterpret_cast<uintptr_t>(ptr));
    };

    // Server.get(app, path, handler)
    serverMod["get"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("Server.get(app, path, handler)");
        auto* srv = getServerFromAddr(args[0]);
        ServerRoute route;
        route.method = "GET";
        route.pattern = args[1].toString();
        if (args[2].type != Value::Type::Function || !args[2].func)
            throw std::runtime_error("Server.get: handler must be a function");
        route.handler = args[2].func;
        srv->routes.push_back(std::move(route));
        return Value::Nil();
    };

    // Server.post(app, path, handler)
    serverMod["post"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("Server.post(app, path, handler)");
        auto* srv = getServerFromAddr(args[0]);
        ServerRoute route;
        route.method = "POST";
        route.pattern = args[1].toString();
        if (args[2].type != Value::Type::Function || !args[2].func)
            throw std::runtime_error("Server.post: handler must be a function");
        route.handler = args[2].func;
        srv->routes.push_back(std::move(route));
        return Value::Nil();
    };

    // Server.put(app, path, handler)
    serverMod["put"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("Server.put(app, path, handler)");
        auto* srv = getServerFromAddr(args[0]);
        ServerRoute route;
        route.method = "PUT";
        route.pattern = args[1].toString();
        if (args[2].type != Value::Type::Function || !args[2].func)
            throw std::runtime_error("Server.put: handler must be a function");
        route.handler = args[2].func;
        srv->routes.push_back(std::move(route));
        return Value::Nil();
    };

    // Server.delete(app, path, handler)
    serverMod["delete"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("Server.delete(app, path, handler)");
        auto* srv = getServerFromAddr(args[0]);
        ServerRoute route;
        route.method = "DELETE";
        route.pattern = args[1].toString();
        if (args[2].type != Value::Type::Function || !args[2].func)
            throw std::runtime_error("Server.delete: handler must be a function");
        route.handler = args[2].func;
        srv->routes.push_back(std::move(route));
        return Value::Nil();
    };

    // Server.listen(app) → blocks, serving requests
    serverMod["listen"] = [self](std::vector<Value> args) -> Value {
        if (args.empty())
            throw std::runtime_error("Server.listen(app)");
        auto* srv = getServerFromAddr(args[0]);

        int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0) throw std::runtime_error("Server.listen: socket() failed");

        int opt = 1;
        setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(srv->port);

        if (::bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(serverFd);
            throw std::runtime_error("Server.listen: bind() failed on port " + std::to_string(srv->port));
        }
        if (::listen(serverFd, 128) < 0) {
            ::close(serverFd);
            throw std::runtime_error("Server.listen: listen() failed");
        }

        std::cout << "\033[32m[Server]\033[0m Flux development server running on http://localhost:"
                  << srv->port << "\n";
        std::cout << "\033[90m  Routes:\n";
        for (auto& r : srv->routes) {
            std::cout << "    " << r.method << " " << r.pattern << "\n";
        }
        std::cout << "\033[0m" << std::flush;

        while (true) {
            int clientFd;
            {
                GILRelease release;
                clientFd = ::accept(serverFd, nullptr, nullptr);
            }
            if (clientFd < 0) continue;

            // Read full request (handle large bodies)
            std::string rawReq;
            {
                char buf[8192];
                ssize_t n = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
                if (n <= 0) { ::close(clientFd); continue; }
                buf[n] = '\0';
                rawReq = std::string(buf, n);

                // Check Content-Length for body
                std::string clHeader = "content-length:";
                size_t clPos = rawReq.find(clHeader);
                if (clPos == std::string::npos) {
                    // try case-insensitive
                    std::string lowerReq = rawReq;
                    for (auto& c : lowerReq) c = tolower(c);
                    clPos = lowerReq.find(clHeader);
                }
                if (clPos != std::string::npos) {
                    size_t valStart = clPos + clHeader.size();
                    while (valStart < rawReq.size() && rawReq[valStart] == ' ') valStart++;
                    size_t valEnd = rawReq.find("\r\n", valStart);
                    if (valEnd == std::string::npos) valEnd = rawReq.size();
                    int contentLen = std::stoi(rawReq.substr(valStart, valEnd - valStart));

                    // Find end of headers
                    size_t headerEnd = rawReq.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        size_t bodyStart = headerEnd + 4;
                        int bodyRead = (int)rawReq.size() - (int)bodyStart;
                        while (bodyRead < contentLen) {
                            ssize_t more = ::recv(clientFd, buf, sizeof(buf) - 1, 0);
                            if (more <= 0) break;
                            rawReq.append(buf, more);
                            bodyRead += more;
                        }
                    }
                }
            }

            // Parse request line: "GET /path?query HTTP/1.1"
            std::string method, fullPath, path, queryStr, body;
            {
                size_t sp1 = rawReq.find(' ');
                size_t sp2 = rawReq.find(' ', sp1 + 1);
                if (sp1 != std::string::npos && sp2 != std::string::npos) {
                    method   = rawReq.substr(0, sp1);
                    fullPath = rawReq.substr(sp1 + 1, sp2 - sp1 - 1);
                }
                // Split path and query
                size_t qPos = fullPath.find('?');
                if (qPos != std::string::npos) {
                    path     = fullPath.substr(0, qPos);
                    queryStr = fullPath.substr(qPos + 1);
                } else {
                    path = fullPath;
                }
                // Extract body (after \r\n\r\n)
                size_t headerEnd = rawReq.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    body = rawReq.substr(headerEnd + 4);
                }
            }

            // Route matching
            std::string responseBody = "";
            int status = 404;
            std::string contentType = "text/plain";
            bool matched = false;

            for (auto& route : srv->routes) {
                if (route.method != method) continue;
                std::shared_ptr<std::unordered_map<std::string, Value>> params;
                if (matchRoute(route, path, params)) {
                    // Build req Map
                    auto reqMap = std::make_shared<std::unordered_map<std::string, Value>>();
                    (*reqMap)["method"]  = Value::Str(method);
                    (*reqMap)["path"]    = Value::Str(path);
                    (*reqMap)["body"]    = Value::Str(body);
                    (*reqMap)["params"]  = Value::MapOf(params);
                    (*reqMap)["query"]   = Value::MapOf(parseQueryParams(queryStr));
                    (*reqMap)["headers"] = Value::MapOf(parseHeaders(rawReq));

                    Value reqVal = Value::MapOf(reqMap);

                    try {
                        Value result = self->callFuncVal(route.handler, {reqVal});
                        responseBody = result.toString();
                        status = 200;
                        // Auto-detect JSON content type
                        if (!responseBody.empty() &&
                            (responseBody[0] == '{' || responseBody[0] == '['))
                            contentType = "application/json";
                    } catch (const std::exception& e) {
                        responseBody = std::string("{\"error\":\"") + e.what() + "\"}";
                        status = 500;
                        contentType = "application/json";
                        std::cerr << "\033[31m[Server] Error handling "
                                  << method << " " << path << ": "
                                  << e.what() << "\033[0m\n";
                    }
                    matched = true;
                    break;
                }
            }

            if (!matched) {
                responseBody = "{\"error\":\"Not Found\",\"path\":\"" + path + "\"}";
                contentType = "application/json";
            }

            // Send response
            std::string statusText = (status == 200) ? "OK"
                : (status == 404) ? "Not Found"
                : "Internal Server Error";
            std::string response =
                "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Content-Length: " + std::to_string(responseBody.size()) + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                + responseBody;
            ::send(clientFd, response.c_str(), response.size(), 0);
            ::close(clientFd);

            // Log request
            std::cout << "\033[90m  " << method << " " << path
                      << " → " << status << "\033[0m\n";
        }
        ::close(serverFd);
        return Value::Nil();
    };

    registerStdlibModule("Server", std::move(serverMod));
}
