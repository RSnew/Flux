// toml.h — Flux 极简 TOML 解析器（Feature L: 包管理器）
// 零依赖 header-only；支持：[节]、key = "字符串"、key = 数字、
// # 注释、key = { path = "..." } 内联表
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace toml {

// 一个 TOML 节（key → value 全部存为字符串）
using Section = std::unordered_map<std::string, std::string>;
// 整个文档（节名 → Section；根节名为 ""）
using Doc     = std::unordered_map<std::string, Section>;

// 内联表值的内部编码前缀（序列化为 key=val;key=val; 格式便于存储）
static const std::string TABLE_PREFIX = "__table__:";

inline std::string trim(std::string s) {
    auto l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return "";
    s.erase(0, l);
    auto r = s.find_last_not_of(" \t\r\n");
    if (r != std::string::npos) s.erase(r + 1);
    return s;
}

inline std::string stripQuotes(std::string s) {
    if (s.size() >= 2 &&
        ((s.front() == '"'  && s.back() == '"' ) ||
         (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

// 解析 { key = "v1", key2 = "v2" } → 编码为 __table__:key=v1;key2=v2;
inline std::string encodeInlineTable(const std::string& s) {
    auto open  = s.find('{');
    auto close = s.rfind('}');
    if (open == std::string::npos || close == std::string::npos) return s;
    std::string inner = s.substr(open + 1, close - open - 1);
    std::string encoded = TABLE_PREFIX;
    std::istringstream ss(inner);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(item.substr(0, eq));
        std::string val = stripQuotes(trim(item.substr(eq + 1)));
        encoded += key + "=" + val + ";";
    }
    return encoded;
}

// 解析 TOML 字符串内容 → Doc
inline Doc parse(const std::string& content) {
    Doc doc;
    doc[""] = {};                    // 根节始终存在
    std::string cur;                 // 当前节名（默认根节）
    std::istringstream ss(content);
    std::string line;

    while (std::getline(ss, line)) {
        // 去注释
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            // 节头
            cur = trim(line.substr(1, line.size() - 2));
            if (!doc.count(cur)) doc[cur] = {};
        } else if (auto eq = line.find('='); eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (!val.empty() && val.front() == '{')
                doc[cur][key] = encodeInlineTable(val);
            else
                doc[cur][key] = stripQuotes(val);
        }
    }
    return doc;
}

// 从文件加载
inline Doc loadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path);
    return parse(std::string(std::istreambuf_iterator<char>(f), {}));
}

// 将 Doc 写回文件（sectionOrder 控制节的顺序）
inline void saveFile(const std::string& path, const Doc& doc,
                     const std::vector<std::string>& sectionOrder = {}) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write: " + path);

    auto writeSection = [&](const std::string& sec) {
        auto it = doc.find(sec);
        if (it == doc.end() || it->second.empty()) return;
        if (!sec.empty()) f << "[" << sec << "]\n";
        for (auto& [k, v] : it->second) {
            if (v.substr(0, TABLE_PREFIX.size()) == TABLE_PREFIX) {
                // 还原为内联表
                f << k << " = { ";
                std::string inner = v.substr(TABLE_PREFIX.size());
                std::istringstream ss2(inner);
                std::string item;
                bool first = true;
                while (std::getline(ss2, item, ';')) {
                    if (item.empty()) continue;
                    auto eq2 = item.find('=');
                    if (eq2 == std::string::npos) continue;
                    if (!first) f << ", ";
                    f << item.substr(0, eq2) << " = \""
                      << item.substr(eq2 + 1) << "\"";
                    first = false;
                }
                f << " }\n";
            } else {
                f << k << " = \"" << v << "\"\n";
            }
        }
        f << "\n";
    };

    // 根节先写（一般为空）
    writeSection("");

    // 按指定顺序写各节
    if (!sectionOrder.empty()) {
        for (auto& s : sectionOrder) writeSection(s);
    } else {
        for (auto& [s, _] : doc) if (!s.empty()) writeSection(s);
    }
}

// 方便函数：读取某节某键，不存在返回 fallback
inline std::string get(const Doc& doc,
                       const std::string& section,
                       const std::string& key,
                       const std::string& fallback = "") {
    auto sit = doc.find(section);
    if (sit == doc.end()) return fallback;
    auto kit = sit->second.find(key);
    return kit == sit->second.end() ? fallback : kit->second;
}

// 判断某节是否为内联表值
inline bool isTable(const std::string& val) {
    return val.size() >= TABLE_PREFIX.size() &&
           val.substr(0, TABLE_PREFIX.size()) == TABLE_PREFIX;
}

// 从内联表编码中提取子键
inline std::string tableGet(const std::string& encoded,
                             const std::string& key,
                             const std::string& fallback = "") {
    if (!isTable(encoded)) return fallback;
    std::string inner = encoded.substr(TABLE_PREFIX.size());
    std::istringstream ss(inner);
    std::string item;
    while (std::getline(ss, item, ';')) {
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        if (item.substr(0, eq) == key) return item.substr(eq + 1);
    }
    return fallback;
}

} // namespace toml
