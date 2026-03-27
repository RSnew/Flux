// taint.h — Cross-module taint tracking for APC Phase 2
// Prevents Confused Deputy attacks by tracking sensitive data flow
// from taint sources (File.read, Env.get) to taint sinks (Http.post, Socket.send)
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <fnmatch.h>

// ═══════════════════════════════════════════════════════════
// TaintInfo — metadata about a tainted value's origin
// ═══════════════════════════════════════════════════════════
struct TaintInfo {
    bool        isSensitive = false;
    std::string sourceModule;      // "File", "Env", "Http"
    std::string sourceCall;        // "read", "get"
    std::string sourceCapability;  // "File.read"
    std::string sourcePath;        // actual path for Intent Render
};

// ═══════════════════════════════════════════════════════════
// TaintViolation — thrown when tainted data reaches a sink
// ═══════════════════════════════════════════════════════════
struct TaintViolation : std::runtime_error {
    std::string sinkModule;
    std::string sinkFunction;
    std::string taintSource;
    std::string taintPath;

    TaintViolation(const std::string& sinkMod,
                   const std::string& sinkFn,
                   const std::string& source,
                   const std::string& path)
        : std::runtime_error(
              "Taint violation: data from " + source +
              (path.empty() ? "" : " (path: " + path + ")") +
              " cannot flow to " + sinkMod + "." + sinkFn +
              " — possible Confused Deputy attack")
        , sinkModule(sinkMod)
        , sinkFunction(sinkFn)
        , taintSource(source)
        , taintPath(path) {}
};

// ═══════════════════════════════════════════════════════════
// Protected paths — hardcoded, LLM cannot modify
// ═══════════════════════════════════════════════════════════
static inline const std::vector<std::string>& protectedPaths() {
    static const std::vector<std::string> paths = {
        "~/.ssh/*",
        "/Users/*/.ssh/*",
        "~/.aws/*",
        "/Users/*/.aws/*",
        "~/.gnupg/*",
        "/Users/*/.gnupg/*",
        "/etc/*",
        "*.env",
        "*/.env",
        "*/secrets/*",
        "*credentials*",
        "*password*",
        "*secret*",
        "*token*",
        "*key*",
    };
    return paths;
}

// Check if a path matches any protected pattern
static inline bool isProtectedPath(const std::string& path) {
    for (auto& pattern : protectedPaths()) {
        if (fnmatch(pattern.c_str(), path.c_str(), FNM_PATHNAME) == 0)
            return true;
        // Also try without FNM_PATHNAME for glob-like matching
        if (fnmatch(pattern.c_str(), path.c_str(), 0) == 0)
            return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════
// TaintTracker — tracks taint across variables and returns
// ═══════════════════════════════════════════════════════════
class TaintTracker {
public:
    void enable() { enabled_ = true; }
    bool isEnabled() const { return enabled_; }

    // ── Mark a variable as tainted ──────────────────────────
    void markTainted(const std::string& varKey, TaintInfo info) {
        if (!enabled_) return;
        taintedVars_[varKey] = std::move(info);
    }

    // ── Check if a variable is tainted ──────────────────────
    TaintInfo* getTaint(const std::string& varKey) {
        if (!enabled_) return nullptr;
        auto it = taintedVars_.find(varKey);
        if (it != taintedVars_.end()) return &it->second;
        return nullptr;
    }

    // ── Remove taint from a variable (e.g., after @sanitize) ──
    void clearTaint(const std::string& varKey) {
        taintedVars_.erase(varKey);
    }

    // ── Set taint on last return value ──────────────────────
    void setReturnTaint(TaintInfo info) {
        if (!enabled_) return;
        lastReturnTaint_ = std::move(info);
    }

    TaintInfo* getReturnTaint() {
        if (!enabled_) return nullptr;
        if (lastReturnTaint_.has_value()) return &lastReturnTaint_.value();
        return nullptr;
    }

    void clearReturnTaint() {
        lastReturnTaint_.reset();
    }

    // ── Check taint sink — returns true if violation found ──
    // Checks if any of the given argument variable names are tainted
    // and the target is a taint sink
    bool checkSink(const std::string& sinkModule, const std::string& sinkFn,
                   const std::vector<std::string>& argVarNames) {
        if (!enabled_) return false;

        // Only check known sinks
        if (!isSink(sinkModule, sinkFn)) return false;

        for (auto& varName : argVarNames) {
            TaintInfo* taint = getTaint(varName);
            if (taint && taint->isSensitive) {
                // Emit detailed violation info to stderr
                std::cerr << "\033[1;31m"
                          << "\n[APC Taint] BLOCKED: Confused Deputy attack detected!\n"
                          << "  Source: " << taint->sourceCapability
                          << (taint->sourcePath.empty() ? "" : " (" + taint->sourcePath + ")")
                          << "\n"
                          << "  Sink:   " << sinkModule << "." << sinkFn << "\n"
                          << "  Via:    variable '" << varName << "'\n"
                          << "\033[0m";

                throw TaintViolation(sinkModule, sinkFn,
                                     taint->sourceCapability, taint->sourcePath);
            }
        }
        return false;
    }

    // ── Check if a stdlib call is a taint source ────────────
    // Returns TaintInfo if the call produces tainted data, nullopt otherwise
    static std::optional<TaintInfo> checkSource(const std::string& module,
                                                 const std::string& fn,
                                                 const std::vector<std::string>& argHints) {
        // File.read(path) — tainted if path matches protected paths
        if (module == "File" && (fn == "read" || fn == "lines")) {
            std::string path = argHints.empty() ? "" : argHints[0];
            if (!path.empty() && isProtectedPath(path)) {
                return TaintInfo{true, module, fn, module + "." + fn, path};
            }
            return std::nullopt;
        }

        // Env.get(key) — ALWAYS tainted (env vars may contain secrets)
        if (module == "Env" && fn == "get") {
            std::string key = argHints.empty() ? "<unknown>" : argHints[0];
            return TaintInfo{true, module, fn, "Env.get", key};
        }

        // Http.get(url) — tainted (external data)
        if (module == "Http" && fn == "get") {
            std::string url = argHints.empty() ? "" : argHints[0];
            return TaintInfo{true, module, fn, "Http.get", url};
        }

        return std::nullopt;
    }

    // ── Taint sinks — where tainted data is checked ────────
    static bool isSink(const std::string& module, const std::string& fn) {
        if (module == "Http" && (fn == "post" || fn == "put")) return true;
        if (module == "Socket" && fn == "send") return true;
        if (module == "WS" && fn == "send") return true;
        return false;
    }

private:
    // Map of varKey → TaintInfo
    // varKey format: "varName" for globals, "moduleName::varName" for module vars
    std::unordered_map<std::string, TaintInfo> taintedVars_;

    // Taint on the last return value from a stdlib call
    std::optional<TaintInfo> lastReturnTaint_;

    bool enabled_ = false;
};
