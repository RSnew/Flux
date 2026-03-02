// pkgmgr.h — Flux 包管理器（Feature L）
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// ── flux.toml 解析结果 ────────────────────────────────────
struct Manifest {
    std::string name;
    std::string version   = "0.1.0";
    std::string desc;
    std::string author;
    std::string fluxVer   = "1.0";
    std::string mainFile  = "src/main.flux";  // 默认入口

    // [dependencies]  name → version_spec
    // version_spec 可以是 "1.0.0" 或内联表编码 "__table__:path=../lib;"
    std::unordered_map<std::string, std::string> deps;

    // [scripts]  name → file (相对路径)
    std::unordered_map<std::string, std::string> scripts;
};

// ── flux build 的返回值（main.cpp 负责执行）─────────────
struct BuildResult {
    bool        ok = false;
    std::string error;
    std::string source;    // 预加载包 + 主文件 的合并源码
    std::string entryFile; // 原始入口文件路径（用于报错显示）
};

// ── 目录工具 ──────────────────────────────────────────────
std::string registryDir();                          // ~/.flux/packages
std::string packagesDir(const std::string& dir);   // <dir>/flux_packages

// ── Manifest 读写 ─────────────────────────────────────────
Manifest loadManifest(const std::string& dir = ".");
void     saveManifest(const Manifest& m, const std::string& dir = ".");

// ── 包管理器命令 ──────────────────────────────────────────
// flux new <name>
void pkgNew(const std::string& name);

// flux build [script] — 返回合并源码供 main.cpp 执行
BuildResult pkgBuild(const std::string& dir = ".",
                     const std::string& script = "run");

// flux add <pkg>[@version]  or  flux add <pkg> path=../lib
void pkgAdd(const std::string& pkgSpec,
            const std::string& dir = ".");

// flux remove <pkg>
void pkgRemove(const std::string& name,
               const std::string& dir = ".");

// flux install — 安装 flux.toml 中所有依赖
void pkgInstall(const std::string& dir = ".");

// flux publish — 发布到本地 registry
void pkgPublish(const std::string& dir = ".");

// flux search [query]
void pkgSearch(const std::string& query = "");

// flux info <pkg>
void pkgInfo(const std::string& name);

// flux list — 列出当前项目依赖
void pkgList(const std::string& dir = ".");
