// apc.h — Agent Permission Contract (APC) Security Layer
// Phase 1: Static capability checking for Flux modules
//
// Modules annotated with @requires declare their capabilities upfront.
// The APC checker verifies that all dangerous calls within a module
// are covered by its declared capabilities. Violations are reported
// before execution, preventing undeclared access to sensitive APIs.
#pragma once
#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <stdexcept>
#include <algorithm>

// ═══════════════════════════════════════════════════════════
// Danger level for operations
// ═══════════════════════════════════════════════════════════
enum class APCLevel { SILENT, WARN, CONFIRM, BLOCK };

// ═══════════════════════════════════════════════════════════
// A required capability for a stdlib/dangerous call
// ═══════════════════════════════════════════════════════════
struct RequiredCapability {
    std::string module;      // "File", "Http", "Net", etc.
    std::string permission;  // "read" / "write" / "allow"
    APCLevel    level;       // default danger level
};

// ═══════════════════════════════════════════════════════════
// APC Violation — thrown/collected when a module lacks permission
// ═══════════════════════════════════════════════════════════
struct APCViolation : std::runtime_error {
    std::string moduleName;
    std::string callModule;
    std::string callFunction;
    std::string requiredCapability;
    std::string declaredCapabilities;

    APCViolation(const std::string& modName,
                 const std::string& callMod,
                 const std::string& callFn,
                 const std::string& reqCap,
                 const std::string& declCaps)
        : std::runtime_error(
              "APC violation in module '" + modName + "': "
              "call to " + callMod + "." + callFn +
              " requires [" + reqCap + "] but module declares [" + declCaps + "]")
        , moduleName(modName)
        , callModule(callMod)
        , callFunction(callFn)
        , requiredCapability(reqCap)
        , declaredCapabilities(declCaps) {}
};

// ═══════════════════════════════════════════════════════════
// Intent Renderer — human-readable operation descriptions
// ═══════════════════════════════════════════════════════════
class IntentRenderer {
public:
    struct OperationSummary {
        APCLevel    level;
        std::string icon;        // emoji indicator
        std::string description; // human-readable
        std::string detail;      // path, URL, etc.
        std::string warning;     // additional warning if any
    };

    // Render intent for a dangerous call
    static OperationSummary render(const std::string& module, const std::string& fn,
                                    const std::vector<std::string>& argHints) {
        std::string detail = argHints.empty() ? "" : argHints[0];

        // File operations
        if (module == "File" && fn == "read")
            return { APCLevel::WARN,    "\xF0\x9F\x93\x96", "Read file contents",          detail, "" };
        if (module == "File" && fn == "write")
            return { APCLevel::CONFIRM, "\xE2\x9C\x8F\xEF\xB8\x8F",  "Write to file",               detail, "Data will be modified" };
        if (module == "File" && fn == "delete")
            return { APCLevel::CONFIRM, "\xF0\x9F\x97\x91\xEF\xB8\x8F",  "Permanently delete file",     detail, "This action cannot be undone" };
        if (module == "File" && fn == "append")
            return { APCLevel::CONFIRM, "\xE2\x9C\x8F\xEF\xB8\x8F",  "Append to file",              detail, "" };

        // Http operations
        if (module == "Http" && fn == "get")
            return { APCLevel::CONFIRM, "\xF0\x9F\x8C\x90", "Fetch data from external URL", detail, "" };
        if (module == "Http" && fn == "post")
            return { APCLevel::CONFIRM, "\xF0\x9F\x93\xA1", "Send data to external server", detail, "Data leaves the system" };
        if (module == "Http" && fn == "download")
            return { APCLevel::CONFIRM, "\xE2\xAC\x87\xEF\xB8\x8F",  "Download file from URL",      detail, "File will be written to disk" };

        // Env operations
        if (module == "Env" && fn == "get")
            return { APCLevel::CONFIRM, "\xF0\x9F\x94\x91", "Read environment variable",    detail, "May contain secrets" };
        if (module == "Env" && fn == "set")
            return { APCLevel::CONFIRM, "\xF0\x9F\x94\x91", "Modify environment variable",  detail, "Affects process environment" };

        // Network operations
        if (module == "Socket" && fn == "connect")
            return { APCLevel::CONFIRM, "\xF0\x9F\x94\x8C", "Open network socket",         detail, "" };
        if (module == "WS" && fn == "connect")
            return { APCLevel::CONFIRM, "\xF0\x9F\x94\x8C", "Open WebSocket connection",   detail, "" };

        // Asm — highest risk
        if (module == "Asm")
            return { APCLevel::BLOCK,   "\xE2\x9B\x94", "Execute machine code (high risk)", "", "Bypasses all safety guarantees" };

        // Concurrent
        if (module == "Concurrent" && fn == "spawn")
            return { APCLevel::WARN,    "\xF0\x9F\x94\x80", "Spawn background task",        "", "" };

        // Generic fallback
        return { APCLevel::WARN, "\xE2\x9A\xA0\xEF\xB8\x8F", module + "." + fn, detail, "" };
    }

    // Print formatted summary to stderr
    static void display(const OperationSummary& summary) {
        const char* levelStr = "";
        switch (summary.level) {
            case APCLevel::SILENT:  levelStr = "SILENT";  break;
            case APCLevel::WARN:    levelStr = "WARN";    break;
            case APCLevel::CONFIRM: levelStr = "CONFIRM"; break;
            case APCLevel::BLOCK:   levelStr = "BLOCK";   break;
        }
        std::cerr << "  " << summary.icon << "  [" << levelStr << "] "
                  << summary.description;
        if (!summary.detail.empty())
            std::cerr << ": " << summary.detail;
        std::cerr << "\n";
        if (!summary.warning.empty())
            std::cerr << "     " << summary.warning << "\n";
    }

    // Print all operations
    static void displayAll(const std::vector<OperationSummary>& ops) {
        if (ops.empty()) return;
        std::cerr << "\n\033[1mAPC Intent Summary:\033[0m\n";
        for (auto& op : ops) display(op);
        std::cerr << "\n";
    }
};

// ═══════════════════════════════════════════════════════════
// APCChecker — static analysis pass for APC enforcement
// ═══════════════════════════════════════════════════════════
class APCChecker {
public:
    // Check entire program for APC violations.
    // Returns list of violations (empty = all clear).
    std::vector<APCViolation> check(Program* prog) {
        violations_.clear();
        intents_.clear();

        for (auto& stmt : prog->statements) {
            auto* mod = dynamic_cast<ModuleDecl*>(stmt.get());
            if (!mod) continue;

            // Only check modules that have @requires declarations
            if (mod->capabilities.empty()) {
                if (strict_) {
                    // In strict mode, modules without @requires are rejected
                    violations_.push_back(APCViolation(
                        mod->name, "", "", "any",
                        "module '" + mod->name + "' has no @requires declarations (strict mode)"));
                }
                continue;
            }

            auto caps = collectCapabilities(mod->capabilities);
            for (auto& bodyNode : mod->body) {
                walkNode(bodyNode.get(), caps, mod->name);
            }
        }

        // Display intent summary if there are any
        if (!intents_.empty()) {
            IntentRenderer::displayAll(intents_);
        }

        return violations_;
    }

    // Enable/disable strict mode
    void setStrict(bool s) { strict_ = s; }

    // Access collected intents (for testing/display)
    const std::vector<IntentRenderer::OperationSummary>& intents() const { return intents_; }

private:
    bool strict_ = false;
    std::vector<APCViolation> violations_;
    std::vector<IntentRenderer::OperationSummary> intents_;

    // ── Hardcoded danger map — LLM cannot modify this ──────────
    // Key: "Module.function"
    // Value: RequiredCapability describing what is needed
    struct DangerEntry {
        std::string capModule;     // capability module required
        std::string capPermission; // capability permission required
        APCLevel    level;
    };

    static const std::unordered_map<std::string, DangerEntry>& dangerMap() {
        static const std::unordered_map<std::string, DangerEntry> map = {
            // File operations
            {"File.read",     {"File",   "read",    APCLevel::WARN}},
            {"File.write",    {"File",   "write",   APCLevel::CONFIRM}},
            {"File.delete",   {"File",   "write",   APCLevel::CONFIRM}},
            {"File.append",   {"File",   "write",   APCLevel::CONFIRM}},
            {"File.exists",   {"File",   "read",    APCLevel::SILENT}},

            // Http operations
            {"Http.get",      {"Http",   "read",    APCLevel::CONFIRM}},
            {"Http.post",     {"Http",   "write",   APCLevel::CONFIRM}},
            {"Http.download", {"Http",   "write",   APCLevel::CONFIRM}},

            // Env operations
            {"Env.get",       {"Env",    "read",    APCLevel::CONFIRM}},
            {"Env.set",       {"Env",    "write",   APCLevel::CONFIRM}},

            // Network operations
            {"Socket.connect",{"Net",    "connect", APCLevel::CONFIRM}},
            {"WS.connect",    {"Net",    "connect", APCLevel::CONFIRM}},
        };
        return map;
    }

    // ── Capability set — collected from @requires declarations ──
    struct CapabilitySet {
        // key: "Module.permission" or "Module.*" for broad access
        std::unordered_set<std::string> entries;
        // module → (permission, scope) pairs
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> byModule;

        bool satisfies(const std::string& reqModule, const std::string& reqPermission) const {
            // Check explicit grant: Module.permission
            if (entries.count(reqModule + "." + reqPermission)) return true;

            // Check broad grants
            if (entries.count(reqModule + ".all")) return true;
            if (entries.count(reqModule + ".allow")) return true;

            // "write" implies "read" (superset)
            if (reqPermission == "read" && entries.count(reqModule + ".write")) return true;

            // "none" blocks everything — if the only entry is "none", deny
            if (entries.count(reqModule + ".none")) return false;

            return false;
        }

        std::string toString() const {
            std::string result;
            for (auto& e : entries) {
                if (!result.empty()) result += ", ";
                result += e;
            }
            return result.empty() ? "(none)" : result;
        }
    };

    CapabilitySet collectCapabilities(const std::vector<CapabilityDecl>& decls) {
        CapabilitySet cs;
        for (auto& d : decls) {
            std::string key = d.module + "." + d.permission;
            cs.entries.insert(key);
            cs.byModule[d.module].push_back({d.permission, d.scope});
        }
        return cs;
    }

    // ── AST walking ──────────────────────────────────────────
    void walkNode(ASTNode* node, const CapabilitySet& caps, const std::string& moduleName) {
        if (!node) return;

        // ModuleCall: the primary target for APC checking
        if (auto* mc = dynamic_cast<ModuleCall*>(node)) {
            checkCall(mc->module, mc->fn, caps, moduleName);
            for (auto& arg : mc->args) walkNode(arg.get(), caps, moduleName);
            return;
        }

        // AsmBlock: always requires Asm.allow
        if (dynamic_cast<AsmBlock*>(node)) {
            checkAsmBlock(caps, moduleName);
            return;
        }

        // SpawnStmt: requires Concurrent.spawn
        if (auto* sp = dynamic_cast<SpawnStmt*>(node)) {
            checkSpawn(caps, moduleName);
            for (auto& s : sp->body) walkNode(s.get(), caps, moduleName);
            return;
        }

        // ── Recurse into compound nodes ─────────────────────
        if (auto* fn = dynamic_cast<FnDecl*>(node)) {
            for (auto& s : fn->body) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* ifS = dynamic_cast<IfStmt*>(node)) {
            walkNode(ifS->condition.get(), caps, moduleName);
            for (auto& s : ifS->thenBlock) walkNode(s.get(), caps, moduleName);
            for (auto& s : ifS->elseBlock) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* whS = dynamic_cast<WhileStmt*>(node)) {
            walkNode(whS->condition.get(), caps, moduleName);
            for (auto& s : whS->body) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* forS = dynamic_cast<ForIn*>(node)) {
            walkNode(forS->iterable.get(), caps, moduleName);
            for (auto& s : forS->body) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* es = dynamic_cast<ExprStmt*>(node)) {
            walkNode(es->expr.get(), caps, moduleName);
            return;
        }
        if (auto* vd = dynamic_cast<VarDecl*>(node)) {
            walkNode(vd->initializer.get(), caps, moduleName);
            return;
        }
        if (auto* ret = dynamic_cast<ReturnStmt*>(node)) {
            walkNode(ret->value.get(), caps, moduleName);
            return;
        }
        if (auto* assign = dynamic_cast<Assign*>(node)) {
            walkNode(assign->value.get(), caps, moduleName);
            return;
        }
        if (auto* bin = dynamic_cast<BinaryExpr*>(node)) {
            walkNode(bin->left.get(), caps, moduleName);
            walkNode(bin->right.get(), caps, moduleName);
            return;
        }
        if (auto* un = dynamic_cast<UnaryExpr*>(node)) {
            walkNode(un->operand.get(), caps, moduleName);
            return;
        }
        if (auto* call = dynamic_cast<CallExpr*>(node)) {
            for (auto& a : call->args) walkNode(a.get(), caps, moduleName);
            return;
        }
        if (auto* ac = dynamic_cast<AsyncCall*>(node)) {
            checkCall(ac->module, ac->fn, caps, moduleName);
            for (auto& a : ac->args) walkNode(a.get(), caps, moduleName);
            return;
        }
        if (auto* ae = dynamic_cast<AsyncExpr*>(node)) {
            walkNode(ae->call.get(), caps, moduleName);
            return;
        }
        if (auto* aw = dynamic_cast<AwaitExpr*>(node)) {
            walkNode(aw->expr.get(), caps, moduleName);
            return;
        }
        if (auto* mc = dynamic_cast<MethodCall*>(node)) {
            walkNode(mc->object.get(), caps, moduleName);
            for (auto& a : mc->args) walkNode(a.get(), caps, moduleName);
            return;
        }
        if (auto* ia = dynamic_cast<IndexAssign*>(node)) {
            walkNode(ia->object.get(), caps, moduleName);
            walkNode(ia->index.get(), caps, moduleName);
            walkNode(ia->value.get(), caps, moduleName);
            return;
        }
        if (auto* ie = dynamic_cast<IndexExpr*>(node)) {
            walkNode(ie->object.get(), caps, moduleName);
            walkNode(ie->index.get(), caps, moduleName);
            return;
        }
        if (auto* fa = dynamic_cast<FieldAssign*>(node)) {
            walkNode(fa->object.get(), caps, moduleName);
            walkNode(fa->value.get(), caps, moduleName);
            return;
        }
        if (auto* sa = dynamic_cast<StateAssign*>(node)) {
            walkNode(sa->value.get(), caps, moduleName);
            return;
        }
        if (auto* pf = dynamic_cast<ProfiledFnDecl*>(node)) {
            walkNode(pf->fnDecl.get(), caps, moduleName);
            return;
        }
        if (auto* def = dynamic_cast<DefaultStmt*>(node)) {
            for (auto& s : def->body) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* dd = dynamic_cast<DefaultDecl*>(node)) {
            for (auto& s : dd->body) walkNode(s.get(), caps, moduleName);
            return;
        }
        if (auto* al = dynamic_cast<ArrayLit*>(node)) {
            for (auto& e : al->elements) walkNode(e.get(), caps, moduleName);
            return;
        }

        // Leaf nodes (NumberLit, StringLit, BoolLit, NilLit, Identifier,
        // StateAccess, etc.) — no action needed
    }

    void checkCall(const std::string& callModule, const std::string& callFn,
                   const CapabilitySet& caps, const std::string& moduleName) {
        std::string key = callModule + "." + callFn;
        auto& dmap = dangerMap();
        auto it = dmap.find(key);
        if (it == dmap.end()) return;  // Not a dangerous call

        const auto& entry = it->second;

        // Collect intent
        intents_.push_back(IntentRenderer::render(callModule, callFn, {}));

        // Check if capability is satisfied
        if (!caps.satisfies(entry.capModule, entry.capPermission)) {
            std::string reqStr = entry.capModule + "." + entry.capPermission;
            violations_.push_back(APCViolation(
                moduleName, callModule, callFn, reqStr, caps.toString()));
        }
    }

    void checkAsmBlock(const CapabilitySet& caps, const std::string& moduleName) {
        intents_.push_back(IntentRenderer::render("Asm", "execute", {}));
        if (!caps.satisfies("Asm", "allow")) {
            violations_.push_back(APCViolation(
                moduleName, "Asm", "execute", "Asm.allow", caps.toString()));
        }
    }

    void checkSpawn(const CapabilitySet& caps, const std::string& moduleName) {
        intents_.push_back(IntentRenderer::render("Concurrent", "spawn", {}));
        if (!caps.satisfies("Concurrent", "spawn")) {
            violations_.push_back(APCViolation(
                moduleName, "Concurrent", "spawn", "Concurrent.spawn", caps.toString()));
        }
    }
};
