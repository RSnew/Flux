#pragma once
#include "ast.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>

// ══════════════════════════════════════════════════════════════
// DocGenerator — 从 Flux AST 提取文档信息，输出 markdown / JSON / context
// ══════════════════════════════════════════════════════════════

struct DocParam {
    std::string name;
    std::string type;   // 可为空
};

struct DocFunction {
    std::string            name;
    std::vector<DocParam>  params;
    std::string            returnType;
    std::string            doc;          // 前置注释
    bool                   hasPreconditions  = false;
    bool                   hasPostconditions = false;
    int                    line = 0;
};

struct DocPersistentField {
    std::string name;
};

struct DocModule {
    std::string                   name;
    std::vector<DocPersistentField> persistent;
    std::vector<DocFunction>      functions;
    // annotations
    bool                          supervised = false;
    std::string                   restartPolicy;  // "always" / "never"
    int                           maxRetries = 3;
    bool                          concurrent = false;
    std::string                   poolName;
    int                           poolQueue = 100;
    std::string                   poolOverflow;
    std::string                   doc;
    int                           line = 0;
};

struct DocStructField {
    std::string name;
};

struct DocStructMethod {
    std::string            name;
    std::vector<DocParam>  params;
    std::string            returnType;
};

struct DocStruct {
    std::string                   name;
    std::string                   interfaceName;
    std::vector<DocStructField>   fields;
    std::vector<DocStructMethod>  methods;
    std::string                   doc;
    int                           line = 0;
};

struct DocInterface {
    std::string                   name;
    std::vector<DocStructMethod>  methods;
    std::string                   doc;
    int                           line = 0;
};

struct DocSpecifyField {
    std::string key;
    std::string value;   // 字符串化的值
};

struct DocSpecify {
    std::string                    name;
    std::string                    intent;
    std::string                    inputDesc;
    std::string                    outputDesc;
    std::vector<std::string>       constraints;
    std::vector<std::string>       examples;
    std::vector<DocSpecifyField>   rawFields;
    std::string                    doc;
    int                            line = 0;
};

struct DocVariable {
    std::string name;
    std::string type;
    bool        isConst = false;   // conf 声明
    std::string doc;
    int         line = 0;
};

struct DocEnumVariant {
    std::string name;
};

struct DocEnum {
    std::string                  name;
    std::vector<DocEnumVariant>  variants;
    std::string                  doc;
    int                          line = 0;
};

struct DocFile {
    std::string                filepath;
    std::vector<DocFunction>   functions;
    std::vector<DocModule>     modules;
    std::vector<DocStruct>     structs;
    std::vector<DocInterface>  interfaces;
    std::vector<DocSpecify>    specifies;
    std::vector<DocVariable>   variables;
    std::vector<DocEnum>       enums;
};

// ── JSON 辅助：转义字符串 ──────────────────────────────────
static inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// ── 从源文件提取某行之前的连续注释行 ─────────────────────
static inline std::vector<std::string> splitLines(const std::string& src) {
    std::vector<std::string> lines;
    std::istringstream iss(src);
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);
    return lines;
}

static inline std::string extractComment(const std::vector<std::string>& srcLines, int declLine) {
    // declLine 是 1-based 行号
    if (declLine <= 0 || declLine > (int)srcLines.size()) return "";
    std::vector<std::string> commentLines;
    for (int i = declLine - 2; i >= 0; i--) {
        std::string trimmed = srcLines[i];
        // 去除前导空白
        size_t start = trimmed.find_first_not_of(" \t");
        if (start == std::string::npos) break;  // 空行终止
        trimmed = trimmed.substr(start);
        if (trimmed.substr(0, 2) == "//") {
            // 去除 "// " 前缀
            std::string content = trimmed.substr(2);
            if (!content.empty() && content[0] == ' ') content = content.substr(1);
            commentLines.push_back(content);
        } else {
            break;
        }
    }
    // 反转（从上到下）
    std::string result;
    for (int i = (int)commentLines.size() - 1; i >= 0; i--) {
        if (!result.empty()) result += " ";
        result += commentLines[i];
    }
    return result;
}

// ── AST 节点字符串化（简易）─────────────────────────────
static inline std::string nodeToString(ASTNode* node) {
    if (!node) return "";
    if (auto* s = dynamic_cast<StringLit*>(node)) return s->value;
    if (auto* n = dynamic_cast<NumberLit*>(node)) {
        if (n->value == (long long)n->value) return std::to_string((long long)n->value);
        return std::to_string(n->value);
    }
    if (auto* b = dynamic_cast<BoolLit*>(node)) return b->value ? "true" : "false";
    if (dynamic_cast<NilLit*>(node)) return "null";
    if (auto* id = dynamic_cast<Identifier*>(node)) return id->name;
    if (auto* arr = dynamic_cast<ArrayLit*>(node)) {
        std::string out = "[";
        for (size_t i = 0; i < arr->elements.size(); i++) {
            if (i > 0) out += ", ";
            auto* elem = arr->elements[i].get();
            // 字符串值加引号
            if (auto* s2 = dynamic_cast<StringLit*>(elem))
                out += "\"" + s2->value + "\"";
            else
                out += nodeToString(elem);
        }
        out += "]";
        return out;
    }
    if (auto* call = dynamic_cast<CallExpr*>(node)) {
        std::string out = call->name + "(";
        for (size_t i = 0; i < call->args.size(); i++) {
            if (i > 0) out += ", ";
            auto* arg = call->args[i].get();
            if (auto* s3 = dynamic_cast<StringLit*>(arg))
                out += "\"" + s3->value + "\"";
            else
                out += nodeToString(arg);
        }
        out += ")";
        return out;
    }
    return "<expr>";
}

// ── 从 ArrayLit 节点提取字符串列表 ─────────────────────
static inline std::vector<std::string> extractStringArray(ASTNode* node) {
    std::vector<std::string> result;
    if (auto* arr = dynamic_cast<ArrayLit*>(node)) {
        for (auto& elem : arr->elements) {
            if (auto* s = dynamic_cast<StringLit*>(elem.get()))
                result.push_back(s->value);
            else
                result.push_back(nodeToString(elem.get()));
        }
    }
    return result;
}

// ══════════════════════════════════════════════════════════════
// DocGenerator 类
// ══════════════════════════════════════════════════════════════
class DocGenerator {
public:
    // 从 AST + 源文件提取文档
    DocFile extract(Program* program, const std::string& filepath, const std::string& source) {
        DocFile doc;
        doc.filepath = filepath;
        auto srcLines = splitLines(source);

        // 用于获取声明行号：遍历 tokens 获取首个 token 行号
        // 由于 AST 没有直接存行号，我们通过源文件行匹配
        // 策略：重新 lex 获取 token 行号映射，或者用名称在源文件中搜索
        // 这里采用简单方法：在源文件中搜索声明名

        for (auto& stmt : program->statements) {
            // ── FnDecl ──
            if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
                doc.functions.push_back(extractFunction(fn, srcLines));
            }
            // ── ProfiledFnDecl（包装的 FnDecl）──
            if (auto* pfn = dynamic_cast<ProfiledFnDecl*>(stmt.get())) {
                if (auto* fn = dynamic_cast<FnDecl*>(pfn->fnDecl.get())) {
                    auto df = extractFunction(fn, srcLines);
                    df.doc = df.doc.empty() ? "@profile annotated" : df.doc + " (@profile)";
                    doc.functions.push_back(std::move(df));
                }
            }
            // ── ModuleDecl ──
            if (auto* md = dynamic_cast<ModuleDecl*>(stmt.get())) {
                doc.modules.push_back(extractModule(md, srcLines));
            }
            // ── VarDecl（含 struct / interface / specify）──
            if (auto* vd = dynamic_cast<VarDecl*>(stmt.get())) {
                if (vd->isInterface) {
                    // interface 声明
                    if (auto* ilit = dynamic_cast<InterfaceLit*>(vd->initializer.get())) {
                        DocInterface di;
                        di.name = vd->name;
                        di.line = findLineOf(srcLines, "var " + vd->name);
                        di.doc = extractComment(srcLines, di.line);
                        for (auto& m : ilit->methods) {
                            DocStructMethod dm;
                            dm.name = m.name;
                            for (auto& p : m.params)
                                dm.params.push_back({p.name, p.type});
                            dm.returnType = m.returnType;
                            di.methods.push_back(std::move(dm));
                        }
                        doc.interfaces.push_back(std::move(di));
                    }
                } else if (auto* slit = dynamic_cast<StructLit*>(vd->initializer.get())) {
                    // struct 声明
                    DocStruct ds;
                    ds.name = vd->name;
                    ds.interfaceName = slit->interfaceName;
                    ds.line = findLineOf(srcLines, "var " + vd->name);
                    ds.doc = extractComment(srcLines, ds.line);
                    for (auto& f : slit->fields)
                        ds.fields.push_back({f.name});
                    for (auto& m : slit->methods) {
                        DocStructMethod dm;
                        dm.name = m.name;
                        for (auto& p : m.params)
                            dm.params.push_back({p.name, p.type});
                        dm.returnType = m.returnType;
                        ds.methods.push_back(std::move(dm));
                    }
                    doc.structs.push_back(std::move(ds));
                } else {
                    // 普通变量
                    DocVariable dv;
                    dv.name = vd->name;
                    dv.type = vd->typeAnnotation.empty() ? "Any" : vd->typeAnnotation;
                    dv.isConst = false;
                    dv.line = findLineOf(srcLines, "var " + vd->name);
                    dv.doc = extractComment(srcLines, dv.line);
                    doc.variables.push_back(std::move(dv));
                }
            }
            // ── ConfDecl（常量）──
            if (auto* cd = dynamic_cast<ConfDecl*>(stmt.get())) {
                DocVariable dv;
                dv.name = cd->name;
                dv.type = cd->typeAnnotation.empty() ? "Any" : cd->typeAnnotation;
                dv.isConst = true;
                dv.line = findLineOf(srcLines, "conf " + cd->name);
                dv.doc = extractComment(srcLines, dv.line);
                doc.variables.push_back(std::move(dv));
            }
            // ── SpecifyDecl ──
            if (auto* sd = dynamic_cast<SpecifyDecl*>(stmt.get())) {
                doc.specifies.push_back(extractSpecify(sd, srcLines));
            }
            // ── EnumDecl ──
            if (auto* ed = dynamic_cast<EnumDecl*>(stmt.get())) {
                DocEnum de;
                de.name = ed->name;
                de.line = findLineOf(srcLines, "enum " + ed->name);
                de.doc = extractComment(srcLines, de.line);
                for (auto& v : ed->variants)
                    de.variants.push_back({v.name});
                doc.enums.push_back(std::move(de));
            }
        }

        return doc;
    }

    // ── 输出格式化：Markdown ─────────────────────────────────
    std::string toMarkdown(const DocFile& doc) {
        std::ostringstream out;
        out << "# Documentation: " << doc.filepath << "\n\n";

        if (!doc.functions.empty()) {
            out << "## Functions\n\n";
            for (auto& fn : doc.functions) {
                out << "### `" << fn.name << "(";
                for (size_t i = 0; i < fn.params.size(); i++) {
                    if (i > 0) out << ", ";
                    out << fn.params[i].name;
                    if (!fn.params[i].type.empty()) out << ": " << fn.params[i].type;
                }
                out << ")";
                if (!fn.returnType.empty()) out << " -> " << fn.returnType;
                out << "`\n\n";
                if (!fn.doc.empty()) out << fn.doc << "\n\n";
                if (fn.hasPreconditions) out << "- **Requires**: preconditions defined\n";
                if (fn.hasPostconditions) out << "- **Ensures**: postconditions defined\n";
                if (fn.hasPreconditions || fn.hasPostconditions) out << "\n";
            }
        }

        if (!doc.modules.empty()) {
            out << "## Modules\n\n";
            for (auto& mod : doc.modules) {
                out << "### `" << mod.name << "`\n\n";
                if (!mod.doc.empty()) out << mod.doc << "\n\n";
                if (mod.supervised) {
                    out << "- **Supervised**: restart=" << mod.restartPolicy
                        << ", maxRetries=" << mod.maxRetries << "\n";
                }
                if (mod.concurrent) {
                    out << "- **Concurrent**: pool=" << mod.poolName
                        << ", queue=" << mod.poolQueue
                        << ", overflow=" << mod.poolOverflow << "\n";
                }
                if (!mod.persistent.empty()) {
                    out << "- **Persistent state**: ";
                    for (size_t i = 0; i < mod.persistent.size(); i++) {
                        if (i > 0) out << ", ";
                        out << mod.persistent[i].name;
                    }
                    out << "\n";
                }
                out << "\n";
                if (!mod.functions.empty()) {
                    out << "**Functions:**\n\n";
                    for (auto& fn : mod.functions) {
                        out << "- `" << fn.name << "(";
                        for (size_t i = 0; i < fn.params.size(); i++) {
                            if (i > 0) out << ", ";
                            out << fn.params[i].name;
                            if (!fn.params[i].type.empty()) out << ": " << fn.params[i].type;
                        }
                        out << ")";
                        if (!fn.returnType.empty()) out << " -> " << fn.returnType;
                        out << "`";
                        if (!fn.doc.empty()) out << " - " << fn.doc;
                        out << "\n";
                    }
                    out << "\n";
                }
            }
        }

        if (!doc.structs.empty()) {
            out << "## Structs\n\n";
            for (auto& s : doc.structs) {
                out << "### `" << s.name << "`";
                if (!s.interfaceName.empty()) out << " (implements " << s.interfaceName << ")";
                out << "\n\n";
                if (!s.doc.empty()) out << s.doc << "\n\n";
                if (!s.fields.empty()) {
                    out << "**Fields:** ";
                    for (size_t i = 0; i < s.fields.size(); i++) {
                        if (i > 0) out << ", ";
                        out << s.fields[i].name;
                    }
                    out << "\n\n";
                }
                if (!s.methods.empty()) {
                    out << "**Methods:**\n\n";
                    for (auto& m : s.methods) {
                        out << "- `" << m.name << "(";
                        for (size_t i = 0; i < m.params.size(); i++) {
                            if (i > 0) out << ", ";
                            out << m.params[i].name;
                            if (!m.params[i].type.empty()) out << ": " << m.params[i].type;
                        }
                        out << ")";
                        if (!m.returnType.empty()) out << " -> " << m.returnType;
                        out << "`\n";
                    }
                    out << "\n";
                }
            }
        }

        if (!doc.interfaces.empty()) {
            out << "## Interfaces\n\n";
            for (auto& iface : doc.interfaces) {
                out << "### `" << iface.name << "`\n\n";
                if (!iface.doc.empty()) out << iface.doc << "\n\n";
                if (!iface.methods.empty()) {
                    out << "**Methods:**\n\n";
                    for (auto& m : iface.methods) {
                        out << "- `" << m.name << "(";
                        for (size_t i = 0; i < m.params.size(); i++) {
                            if (i > 0) out << ", ";
                            out << m.params[i].name;
                            if (!m.params[i].type.empty()) out << ": " << m.params[i].type;
                        }
                        out << ")";
                        if (!m.returnType.empty()) out << " -> " << m.returnType;
                        out << "`\n";
                    }
                    out << "\n";
                }
            }
        }

        if (!doc.specifies.empty()) {
            out << "## Contracts (specify)\n\n";
            for (auto& sp : doc.specifies) {
                out << "### `" << sp.name << "`\n\n";
                if (!sp.doc.empty()) out << sp.doc << "\n\n";
                if (!sp.intent.empty()) out << "- **Intent**: " << sp.intent << "\n";
                if (!sp.inputDesc.empty()) out << "- **Input**: " << sp.inputDesc << "\n";
                if (!sp.outputDesc.empty()) out << "- **Output**: " << sp.outputDesc << "\n";
                if (!sp.constraints.empty()) {
                    out << "- **Constraints**:\n";
                    for (auto& c : sp.constraints)
                        out << "  - " << c << "\n";
                }
                if (!sp.examples.empty()) {
                    out << "- **Examples**:\n";
                    for (auto& e : sp.examples)
                        out << "  - " << e << "\n";
                }
                out << "\n";
            }
        }

        if (!doc.variables.empty()) {
            out << "## Variables\n\n";
            for (auto& v : doc.variables) {
                out << "- `" << (v.isConst ? "conf " : "var ") << v.name;
                if (v.type != "Any") out << ": " << v.type;
                out << "`";
                if (!v.doc.empty()) out << " - " << v.doc;
                out << "\n";
            }
            out << "\n";
        }

        if (!doc.enums.empty()) {
            out << "## Enums\n\n";
            for (auto& e : doc.enums) {
                out << "### `" << e.name << "`\n\n";
                if (!e.doc.empty()) out << e.doc << "\n\n";
                out << "**Values:** ";
                for (size_t i = 0; i < e.variants.size(); i++) {
                    if (i > 0) out << ", ";
                    out << e.variants[i].name;
                }
                out << "\n\n";
            }
        }

        return out.str();
    }

    // ── 输出格式化：JSON ─────────────────────────────────────
    std::string toJson(const DocFile& doc) {
        std::ostringstream out;
        out << "{\n";
        out << "  \"file\": \"" << jsonEscape(doc.filepath) << "\",\n";

        // functions
        out << "  \"functions\": [\n";
        for (size_t i = 0; i < doc.functions.size(); i++) {
            auto& fn = doc.functions[i];
            out << "    {\"name\": \"" << jsonEscape(fn.name) << "\", \"params\": [";
            for (size_t j = 0; j < fn.params.size(); j++) {
                out << "{\"name\": \"" << jsonEscape(fn.params[j].name) << "\"";
                if (!fn.params[j].type.empty())
                    out << ", \"type\": \"" << jsonEscape(fn.params[j].type) << "\"";
                out << "}";
                if (j + 1 < fn.params.size()) out << ", ";
            }
            out << "], \"returnType\": \"" << jsonEscape(fn.returnType) << "\"";
            out << ", \"doc\": \"" << jsonEscape(fn.doc) << "\"";
            if (fn.hasPreconditions) out << ", \"requires\": true";
            if (fn.hasPostconditions) out << ", \"ensures\": true";
            out << "}";
            if (i + 1 < doc.functions.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // modules
        out << "  \"modules\": [\n";
        for (size_t i = 0; i < doc.modules.size(); i++) {
            auto& mod = doc.modules[i];
            out << "    {\"name\": \"" << jsonEscape(mod.name) << "\"";
            // persistent
            out << ", \"persistent\": [";
            for (size_t j = 0; j < mod.persistent.size(); j++) {
                out << "\"" << jsonEscape(mod.persistent[j].name) << "\"";
                if (j + 1 < mod.persistent.size()) out << ", ";
            }
            out << "]";
            // supervised
            if (mod.supervised) {
                out << ", \"supervised\": {\"restart\": \"" << jsonEscape(mod.restartPolicy)
                    << "\", \"maxRetries\": " << mod.maxRetries << "}";
            }
            // concurrent
            if (mod.concurrent) {
                out << ", \"concurrent\": {\"pool\": \"" << jsonEscape(mod.poolName)
                    << "\", \"queue\": " << mod.poolQueue
                    << ", \"overflow\": \"" << jsonEscape(mod.poolOverflow) << "\"}";
            }
            // functions
            out << ", \"functions\": [";
            for (size_t j = 0; j < mod.functions.size(); j++) {
                auto& fn = mod.functions[j];
                out << "{\"name\": \"" << jsonEscape(fn.name) << "\", \"params\": [";
                for (size_t k = 0; k < fn.params.size(); k++) {
                    out << "{\"name\": \"" << jsonEscape(fn.params[k].name) << "\"";
                    if (!fn.params[k].type.empty())
                        out << ", \"type\": \"" << jsonEscape(fn.params[k].type) << "\"";
                    out << "}";
                    if (k + 1 < fn.params.size()) out << ", ";
                }
                out << "], \"returnType\": \"" << jsonEscape(fn.returnType) << "\"";
                out << ", \"doc\": \"" << jsonEscape(fn.doc) << "\"}";
                if (j + 1 < mod.functions.size()) out << ", ";
            }
            out << "]";
            out << ", \"doc\": \"" << jsonEscape(mod.doc) << "\"";
            out << "}";
            if (i + 1 < doc.modules.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // structs
        out << "  \"structs\": [\n";
        for (size_t i = 0; i < doc.structs.size(); i++) {
            auto& s = doc.structs[i];
            out << "    {\"name\": \"" << jsonEscape(s.name) << "\"";
            if (!s.interfaceName.empty())
                out << ", \"implements\": \"" << jsonEscape(s.interfaceName) << "\"";
            out << ", \"fields\": [";
            for (size_t j = 0; j < s.fields.size(); j++) {
                out << "\"" << jsonEscape(s.fields[j].name) << "\"";
                if (j + 1 < s.fields.size()) out << ", ";
            }
            out << "], \"methods\": [";
            for (size_t j = 0; j < s.methods.size(); j++) {
                auto& m = s.methods[j];
                out << "{\"name\": \"" << jsonEscape(m.name) << "\", \"params\": [";
                for (size_t k = 0; k < m.params.size(); k++) {
                    out << "{\"name\": \"" << jsonEscape(m.params[k].name) << "\"";
                    if (!m.params[k].type.empty())
                        out << ", \"type\": \"" << jsonEscape(m.params[k].type) << "\"";
                    out << "}";
                    if (k + 1 < m.params.size()) out << ", ";
                }
                out << "], \"returnType\": \"" << jsonEscape(m.returnType) << "\"}";
                if (j + 1 < s.methods.size()) out << ", ";
            }
            out << "]";
            out << ", \"doc\": \"" << jsonEscape(s.doc) << "\"";
            out << "}";
            if (i + 1 < doc.structs.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // interfaces
        out << "  \"interfaces\": [\n";
        for (size_t i = 0; i < doc.interfaces.size(); i++) {
            auto& iface = doc.interfaces[i];
            out << "    {\"name\": \"" << jsonEscape(iface.name) << "\", \"methods\": [";
            for (size_t j = 0; j < iface.methods.size(); j++) {
                auto& m = iface.methods[j];
                out << "{\"name\": \"" << jsonEscape(m.name) << "\", \"params\": [";
                for (size_t k = 0; k < m.params.size(); k++) {
                    out << "{\"name\": \"" << jsonEscape(m.params[k].name) << "\"";
                    if (!m.params[k].type.empty())
                        out << ", \"type\": \"" << jsonEscape(m.params[k].type) << "\"";
                    out << "}";
                    if (k + 1 < m.params.size()) out << ", ";
                }
                out << "], \"returnType\": \"" << jsonEscape(m.returnType) << "\"}";
                if (j + 1 < iface.methods.size()) out << ", ";
            }
            out << "]";
            out << ", \"doc\": \"" << jsonEscape(iface.doc) << "\"";
            out << "}";
            if (i + 1 < doc.interfaces.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // enums
        out << "  \"enums\": [\n";
        for (size_t i = 0; i < doc.enums.size(); i++) {
            auto& e = doc.enums[i];
            out << "    {\"name\": \"" << jsonEscape(e.name) << "\", \"variants\": [";
            for (size_t j = 0; j < e.variants.size(); j++) {
                out << "\"" << jsonEscape(e.variants[j].name) << "\"";
                if (j + 1 < e.variants.size()) out << ", ";
            }
            out << "]";
            out << ", \"doc\": \"" << jsonEscape(e.doc) << "\"";
            out << "}";
            if (i + 1 < doc.enums.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // specifies
        out << "  \"specifies\": [\n";
        for (size_t i = 0; i < doc.specifies.size(); i++) {
            auto& sp = doc.specifies[i];
            out << "    {\"name\": \"" << jsonEscape(sp.name) << "\"";
            out << ", \"intent\": \"" << jsonEscape(sp.intent) << "\"";
            out << ", \"input\": \"" << jsonEscape(sp.inputDesc) << "\"";
            out << ", \"output\": \"" << jsonEscape(sp.outputDesc) << "\"";
            out << ", \"constraints\": [";
            for (size_t j = 0; j < sp.constraints.size(); j++) {
                out << "\"" << jsonEscape(sp.constraints[j]) << "\"";
                if (j + 1 < sp.constraints.size()) out << ", ";
            }
            out << "]";
            out << ", \"examples\": [";
            for (size_t j = 0; j < sp.examples.size(); j++) {
                out << "\"" << jsonEscape(sp.examples[j]) << "\"";
                if (j + 1 < sp.examples.size()) out << ", ";
            }
            out << "]";
            out << "}";
            if (i + 1 < doc.specifies.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // variables
        out << "  \"variables\": [\n";
        for (size_t i = 0; i < doc.variables.size(); i++) {
            auto& v = doc.variables[i];
            out << "    {\"name\": \"" << jsonEscape(v.name) << "\"";
            out << ", \"type\": \"" << jsonEscape(v.type) << "\"";
            out << ", \"const\": " << (v.isConst ? "true" : "false");
            out << ", \"doc\": \"" << jsonEscape(v.doc) << "\"";
            out << "}";
            if (i + 1 < doc.variables.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n";

        out << "}\n";
        return out.str();
    }

    // ── 输出格式化：Context（LLM 系统提示优化）───────────────
    std::string toContext(const DocFile& doc) {
        std::ostringstream out;

        // 提取文件名
        std::string filename = doc.filepath;
        auto slash = filename.rfind('/');
        if (slash != std::string::npos) filename = filename.substr(slash + 1);

        out << "# API Reference: " << filename << "\n";

        if (!doc.functions.empty()) {
            out << "\n## Functions\n";
            for (auto& fn : doc.functions) {
                out << "- " << fn.name << "(";
                for (size_t i = 0; i < fn.params.size(); i++) {
                    if (i > 0) out << ", ";
                    out << fn.params[i].name;
                    if (!fn.params[i].type.empty()) out << ": " << fn.params[i].type;
                }
                out << ")";
                if (!fn.returnType.empty()) out << " -> " << fn.returnType;
                if (!fn.doc.empty()) out << ": \"" << fn.doc << "\"";
                if (fn.hasPreconditions) out << " [requires]";
                if (fn.hasPostconditions) out << " [ensures]";
                out << "\n";
            }
        }

        if (!doc.modules.empty()) {
            out << "\n## Modules\n";
            for (auto& mod : doc.modules) {
                out << "- " << mod.name;
                if (!mod.persistent.empty()) {
                    out << " [persistent: ";
                    for (size_t i = 0; i < mod.persistent.size(); i++) {
                        if (i > 0) out << ", ";
                        out << mod.persistent[i].name;
                    }
                    out << "]";
                }
                if (mod.supervised) {
                    out << " [supervised: restart=" << mod.restartPolicy
                        << ", maxRetries=" << mod.maxRetries << "]";
                }
                if (mod.concurrent) {
                    out << " [concurrent: pool=" << mod.poolName
                        << ", queue=" << mod.poolQueue
                        << ", overflow=" << mod.poolOverflow << "]";
                }
                out << "\n";
                for (auto& fn : mod.functions) {
                    out << "  - " << fn.name << "(";
                    for (size_t i = 0; i < fn.params.size(); i++) {
                        if (i > 0) out << ", ";
                        out << fn.params[i].name;
                        if (!fn.params[i].type.empty()) out << ": " << fn.params[i].type;
                    }
                    out << ")";
                    if (!fn.returnType.empty()) out << " -> " << fn.returnType;
                    if (!fn.doc.empty()) out << ": \"" << fn.doc << "\"";
                    out << "\n";
                }
            }
        }

        if (!doc.structs.empty()) {
            out << "\n## Structs\n";
            for (auto& s : doc.structs) {
                out << "- " << s.name;
                if (!s.interfaceName.empty()) out << " (implements " << s.interfaceName << ")";
                if (!s.fields.empty()) {
                    out << " {";
                    for (size_t i = 0; i < s.fields.size(); i++) {
                        if (i > 0) out << ", ";
                        out << s.fields[i].name;
                    }
                    out << "}";
                }
                out << "\n";
                for (auto& m : s.methods) {
                    out << "  - " << m.name << "(";
                    for (size_t i = 0; i < m.params.size(); i++) {
                        if (i > 0) out << ", ";
                        out << m.params[i].name;
                    }
                    out << ")";
                    if (!m.returnType.empty()) out << " -> " << m.returnType;
                    out << "\n";
                }
            }
        }

        if (!doc.interfaces.empty()) {
            out << "\n## Interfaces\n";
            for (auto& iface : doc.interfaces) {
                out << "- " << iface.name << "\n";
                for (auto& m : iface.methods) {
                    out << "  - " << m.name << "(";
                    for (size_t i = 0; i < m.params.size(); i++) {
                        if (i > 0) out << ", ";
                        out << m.params[i].name;
                    }
                    out << ")";
                    if (!m.returnType.empty()) out << " -> " << m.returnType;
                    out << "\n";
                }
            }
        }

        if (!doc.specifies.empty()) {
            out << "\n## Contracts (specify)\n";
            for (auto& sp : doc.specifies) {
                out << "- " << sp.name << ":";
                if (!sp.intent.empty()) out << " intent=\"" << sp.intent << "\"";
                if (!sp.inputDesc.empty()) out << ", input=" << sp.inputDesc;
                if (!sp.outputDesc.empty()) out << ", output=" << sp.outputDesc;
                if (!sp.constraints.empty()) {
                    out << ", constraints=[";
                    for (size_t i = 0; i < sp.constraints.size(); i++) {
                        if (i > 0) out << ", ";
                        out << "\"" << sp.constraints[i] << "\"";
                    }
                    out << "]";
                }
                if (!sp.examples.empty()) {
                    out << ", examples=[";
                    for (size_t i = 0; i < sp.examples.size(); i++) {
                        if (i > 0) out << ", ";
                        out << "\"" << sp.examples[i] << "\"";
                    }
                    out << "]";
                }
                out << "\n";
            }
        }

        if (!doc.enums.empty()) {
            out << "\n## Enums\n";
            for (auto& e : doc.enums) {
                out << "- " << e.name << " {";
                for (size_t i = 0; i < e.variants.size(); i++) {
                    if (i > 0) out << ", ";
                    out << e.variants[i].name;
                }
                out << "}\n";
            }
        }

        if (!doc.variables.empty()) {
            out << "\n## Variables\n";
            for (auto& v : doc.variables) {
                out << "- " << (v.isConst ? "conf " : "") << v.name;
                if (v.type != "Any") out << ": " << v.type;
                if (!v.doc.empty()) out << " — " << v.doc;
                out << "\n";
            }
        }

        return out.str();
    }

private:
    // 在源文件中查找某个前缀首次出现的行号（1-based）
    int findLineOf(const std::vector<std::string>& srcLines, const std::string& prefix) {
        for (size_t i = 0; i < srcLines.size(); i++) {
            std::string trimmed = srcLines[i];
            size_t start = trimmed.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            trimmed = trimmed.substr(start);
            if (trimmed.find(prefix) == 0) return (int)(i + 1);
        }
        return 0;
    }

    DocFunction extractFunction(FnDecl* fn, const std::vector<std::string>& srcLines) {
        DocFunction df;
        df.name = fn->name;
        for (auto& p : fn->params)
            df.params.push_back({p.name, p.type});
        df.returnType = fn->returnType;
        df.hasPreconditions = !fn->preconditions_.empty();
        df.hasPostconditions = !fn->postconditions_.empty();
        df.line = findLineOf(srcLines, "func " + fn->name);
        df.doc = extractComment(srcLines, df.line);
        return df;
    }

    DocModule extractModule(ModuleDecl* md, const std::vector<std::string>& srcLines) {
        DocModule dm;
        dm.name = md->name;
        dm.line = findLineOf(srcLines, "module " + md->name);
        dm.doc = extractComment(srcLines, dm.line);

        // supervised
        if (md->restartPolicy != RestartPolicy::None) {
            dm.supervised = true;
            dm.restartPolicy = (md->restartPolicy == RestartPolicy::Always) ? "always" : "never";
            dm.maxRetries = md->maxRetries;
        }
        // concurrent
        if (!md->poolName.empty()) {
            dm.concurrent = true;
            dm.poolName = md->poolName;
            dm.poolQueue = md->poolQueue;
            dm.poolOverflow = md->poolOverflow;
        }

        // body 成员
        for (auto& item : md->body) {
            if (auto* fn = dynamic_cast<FnDecl*>(item.get())) {
                dm.functions.push_back(extractFunction(fn, srcLines));
            }
            if (auto* pb = dynamic_cast<PersistentBlock*>(item.get())) {
                for (auto& f : pb->fields)
                    dm.persistent.push_back({f.name});
            }
        }

        return dm;
    }

    DocSpecify extractSpecify(SpecifyDecl* sd, const std::vector<std::string>& srcLines) {
        DocSpecify ds;
        ds.name = sd->name;
        ds.line = findLineOf(srcLines, "var " + sd->name);
        ds.doc = extractComment(srcLines, ds.line);

        for (auto& f : sd->fields) {
            ds.rawFields.push_back({f.key, nodeToString(f.value.get())});

            if (f.key == "intent") {
                if (auto* s = dynamic_cast<StringLit*>(f.value.get()))
                    ds.intent = s->value;
                else
                    ds.intent = nodeToString(f.value.get());
            } else if (f.key == "output") {
                if (auto* s = dynamic_cast<StringLit*>(f.value.get()))
                    ds.outputDesc = s->value;
                else
                    ds.outputDesc = nodeToString(f.value.get());
            } else if (f.key == "input") {
                ds.inputDesc = nodeToString(f.value.get());
            } else if (f.key == "constraints") {
                ds.constraints = extractStringArray(f.value.get());
            } else if (f.key == "examples") {
                ds.examples = extractStringArray(f.value.get());
            }
        }

        return ds;
    }
};
