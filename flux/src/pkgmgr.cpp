// pkgmgr.cpp — Flux 包管理器实现（Feature L）
#include "pkgmgr.h"
#include "toml.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>   // getpid()

namespace fs = std::filesystem;

// ── ANSI 颜色（复用 main.cpp 同一套）─────────────────────
#define OK  "\033[32m✓\033[0m "
#define ERR "\033[31m✗\033[0m "
#define DIM "\033[90m"
#define RST "\033[0m"
#define BLD "\033[1m"

// ── 目录工具 ──────────────────────────────────────────────

std::string registryDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.flux/packages";
}

std::string packagesDir(const std::string& dir) {
    return dir + "/flux_packages";
}

static std::string manifestPath(const std::string& dir) {
    return dir + "/flux.toml";
}

static std::string lockPath(const std::string& dir) {
    return dir + "/flux.lock";
}

// ── Manifest 读写 ─────────────────────────────────────────

Manifest loadManifest(const std::string& dir) {
    toml::Doc doc;
    try {
        doc = toml::loadFile(manifestPath(dir));
    } catch (...) {
        throw std::runtime_error(
            "flux.toml not found in '" + dir + "'\n"
            "  Run 'flux new <name>' to create a project, or 'cd' into a project directory.");
    }
    Manifest m;
    m.name     = toml::get(doc, "package", "name");
    m.version  = toml::get(doc, "package", "version",     "0.1.0");
    m.desc     = toml::get(doc, "package", "description");
    m.author   = toml::get(doc, "package", "author");
    m.fluxVer  = toml::get(doc, "package", "flux",        "1.0");
    m.mainFile = toml::get(doc, "package", "main",        "src/main.flux");

    auto& deps    = doc["dependencies"];
    for (auto& kv : deps) m.deps[kv.first] = kv.second;

    auto& scripts = doc["scripts"];
    for (auto& kv : scripts) m.scripts[kv.first] = kv.second;

    return m;
}

void saveManifest(const Manifest& m, const std::string& dir) {
    toml::Doc doc;
    doc["package"]["name"]    = m.name;
    doc["package"]["version"] = m.version;
    doc["package"]["flux"]    = m.fluxVer;
    if (!m.desc.empty())     doc["package"]["description"] = m.desc;
    if (!m.author.empty())   doc["package"]["author"]      = m.author;
    if (m.mainFile != "src/main.flux")
                             doc["package"]["main"]        = m.mainFile;
    for (auto& kv : m.deps)     doc["dependencies"][kv.first] = kv.second;
    for (auto& kv : m.scripts)  doc["scripts"][kv.first]      = kv.second;

    toml::saveFile(manifestPath(dir), doc,
                   {"package", "dependencies", "scripts"});
}

// ── flux new <name> ───────────────────────────────────────

void pkgNew(const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: flux new <project-name>\n"; return;
    }
    if (fs::exists(name)) {
        std::cerr << ERR << "Directory '" << name << "' already exists.\n"; return;
    }

    fs::create_directories(name + "/src");
    fs::create_directories(name + "/tests");
    fs::create_directories(name + "/flux_packages");

    // flux.toml
    Manifest m;
    m.name = name;
    m.scripts["run"]  = "src/main.flux";
    m.scripts["test"] = "tests/test.flux";
    saveManifest(m, name);

    // src/main.flux
    { std::ofstream f(name + "/src/main.flux");
      f << "// " << name << " — main entry point\n\n"
        << "print(\"Hello from " << name << "!\")\n"; }

    // tests/test.flux
    { std::ofstream f(name + "/tests/test.flux");
      f << "// " << name << " tests\n\n"
        << "fn assert(cond, msg) {\n"
        << "    if !cond { print(\"FAIL: \" + msg) }\n"
        << "    else     { print(\"PASS: \" + msg) }\n"
        << "}\n\n"
        << "assert(1 + 1 == 2, \"basic arithmetic\")\n"
        << "print(\"All tests passed\")\n"; }

    // .gitignore
    { std::ofstream f(name + "/.gitignore");
      f << "flux_packages/\nbuild/\nflux.lock\n"; }

    std::cout << BLD << OK << "Created project '" << name << "'" << RST << "\n\n"
              << DIM << "  cd " << name << " && flux build" << RST << "\n\n"
              << "Project structure:\n"
              << "  " << name << "/\n"
              << "  ├── flux.toml          # project manifest\n"
              << "  ├── src/main.flux      # entry point\n"
              << "  ├── tests/test.flux    # tests\n"
              << "  └── flux_packages/     # installed dependencies\n";
}

// ── flux build [script] ───────────────────────────────────

BuildResult pkgBuild(const std::string& dir, const std::string& script) {
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) {
        return { false, e.what() };
    }

    // 确定入口文件
    std::string entryFile;
    if (!script.empty() && script != "run") {
        if (m.scripts.count(script))
            entryFile = dir + "/" + m.scripts[script];
        else
            entryFile = script;   // 直接当文件路径
    } else if (m.scripts.count("run")) {
        entryFile = dir + "/" + m.scripts["run"];
    } else {
        entryFile = dir + "/" + m.mainFile;
    }

    if (!fs::exists(entryFile))
        return { false, "Entry point not found: " + entryFile };

    // 收集 flux_packages/ 下所有 .flux 文件（作为前导代码）
    std::string preamble;
    auto pkgDir = packagesDir(dir);
    if (fs::exists(pkgDir)) {
        // 按包名排序，确定性
        std::vector<fs::path> pkgPaths;
        for (auto& e : fs::directory_iterator(pkgDir))
            if (fs::is_directory(e)) pkgPaths.push_back(e.path());
        std::sort(pkgPaths.begin(), pkgPaths.end());

        for (auto& pkg : pkgPaths) {
            // 读取包自身的 flux.toml，确定入口文件（默认 src/main.flux）
            std::string pkgMain = "src/main.flux";
            fs::path tomlPath = pkg / "flux.toml";
            if (fs::exists(tomlPath)) {
                try {
                    auto pkgDoc = toml::loadFile(tomlPath.string());
                    auto run = toml::get(pkgDoc, "scripts", "run");
                    if (!run.empty()) pkgMain = run;
                    else {
                        auto mf = toml::get(pkgDoc, "package", "main");
                        if (!mf.empty()) pkgMain = mf;
                    }
                } catch (...) {}
            }
            fs::path entryPath = pkg / pkgMain;
            if (!fs::exists(entryPath)) {
                // 回退：收集 src/ 目录下所有 .flux 文件
                for (auto& f : fs::recursive_directory_iterator(pkg / "src"))
                    if (f.path().extension() == ".flux") {
                        std::ifstream src(f.path());
                        preamble += "// ─── package: " + pkg.filename().string()
                                    + "/" + fs::relative(f.path(), pkg).string() + " ───\n";
                        preamble += std::string(std::istreambuf_iterator<char>(src), {});
                        preamble += "\n";
                    }
                continue;
            }
            std::ifstream src(entryPath);
            preamble += "// ─── package: " + pkg.filename().string()
                        + "/" + pkgMain + " ───\n";
            preamble += std::string(std::istreambuf_iterator<char>(src), {});
            preamble += "\n";
        }
    }

    // 读入口文件
    std::ifstream mainSrc(entryFile);
    if (!mainSrc)
        return { false, "Cannot read: " + entryFile };
    std::string mainContent(std::istreambuf_iterator<char>(mainSrc), {});

    return { true, "", preamble + mainContent, entryFile };
}

// ── flux add <pkg>[@version]  or  flux add <pkg> path=./lib ──

void pkgAdd(const std::string& pkgSpec, const std::string& dir) {
    if (pkgSpec.empty()) {
        std::cerr << "Usage: flux add <package>[@version]  or  flux add <name> path=<local/path>\n";
        return;
    }
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; return; }

    std::string name, spec;

    // flux add mylib path=../mylib
    if (pkgSpec.find("path=") != std::string::npos) {
        auto sp = pkgSpec.find(' ');
        name = toml::trim(pkgSpec.substr(0, sp));
        spec = toml::TABLE_PREFIX + pkgSpec.substr(sp + 1) + ";";
    } else {
        // flux add mylib[@version]
        auto at = pkgSpec.find('@');
        if (at != std::string::npos) {
            name = pkgSpec.substr(0, at);
            spec = pkgSpec.substr(at + 1);
        } else {
            name = pkgSpec;
            spec = "*";   // 匹配任意版本
            // 尝试从注册表找最新版本
            auto reg = registryDir();
            if (fs::exists(reg)) {
                std::string latest;
                for (auto& e : fs::directory_iterator(reg)) {
                    auto fn = e.path().filename().string();
                    if (fn == name || fn.substr(0, name.size() + 1) == name + "-") {
                        auto dash = fn.rfind('-');
                        if (dash != std::string::npos && fn.substr(0, dash) == name)
                            latest = fn.substr(dash + 1);
                    }
                }
                if (!latest.empty()) spec = latest;
            }
        }
    }

    bool updated = m.deps.count(name) > 0;
    m.deps[name] = spec;
    saveManifest(m, dir);

    if (toml::isTable(spec)) {
        std::string path = toml::tableGet(spec, "path");
        std::cout << OK << (updated ? "Updated" : "Added") << " " << name
                  << " (local: " << path << ")\n";
    } else {
        std::cout << OK << (updated ? "Updated" : "Added") << " "
                  << name << "@" << spec << "\n";
    }
    std::cout << DIM << "  Run 'flux install' to install." << RST << "\n";
}

// ── flux remove <pkg> ─────────────────────────────────────

void pkgRemove(const std::string& name, const std::string& dir) {
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; return; }

    if (!m.deps.count(name)) {
        std::cerr << ERR << "'" << name << "' not in dependencies.\n"; return;
    }
    m.deps.erase(name);
    saveManifest(m, dir);

    auto installed = packagesDir(dir) + "/" + name;
    if (fs::exists(installed)) fs::remove_all(installed);

    std::cout << OK << "Removed " << name << "\n";
}

// ── flux install ──────────────────────────────────────────

void pkgInstall(const std::string& dir) {
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; return; }

    if (m.deps.empty()) {
        std::cout << "No dependencies.\n"; return;
    }

    auto reg    = registryDir();
    auto pkgDir = packagesDir(dir);
    fs::create_directories(pkgDir);

    int installed = 0, skipped = 0, failed = 0;

    for (auto& [name, spec] : m.deps) {
        auto dest = pkgDir + "/" + name;

        // ── 本地路径依赖 ─────────────────────────────────
        if (toml::isTable(spec)) {
            std::string path = toml::tableGet(spec, "path");
            if (path.empty()) {
                std::cerr << ERR << name << ": missing 'path' in inline table\n";
                failed++; continue;
            }
            if (!fs::exists(path)) {
                std::cerr << ERR << name << ": local path not found: " << path << "\n";
                failed++; continue;
            }
            if (fs::exists(dest)) {
                std::cout << DIM << "  " << name << " (local, already linked)" << RST << "\n";
                skipped++; continue;
            }
            // 创建符号链接（或复制）
            try {
                fs::create_symlink(fs::absolute(path), dest);
                std::cout << OK << name << " → " << path << "\n";
                installed++;
            } catch (...) {
                // 若符号链接失败，回退为复制
                fs::create_directories(dest);
                for (auto& entry : fs::recursive_directory_iterator(path)) {
                    auto rel = fs::relative(entry.path(), path);
                    if (fs::is_directory(entry)) fs::create_directories(dest / rel);
                    else fs::copy_file(entry.path(), dest / rel,
                                       fs::copy_options::overwrite_existing);
                }
                std::cout << OK << name << " (copied from " << path << ")\n";
                installed++;
            }
            continue;
        }

        // ── 版本依赖：已安装则跳过 ───────────────────────
        if (fs::exists(dest)) {
            std::cout << DIM << "  " << name << "@" << spec
                      << " (already installed)" << RST << "\n";
            skipped++; continue;
        }

        // ── 从本地注册表安装 ─────────────────────────────
        // 查找策略：name-version > name（任意版本）
        fs::path src;
        if (fs::exists(reg)) {
            fs::path exact = fs::path(reg) / (name + "-" + spec);
            fs::path any   = fs::path(reg) / name;
            // 也支持匹配任意已发布版本
            if (fs::exists(exact)) {
                src = exact;
            } else {
                // 遍历注册表，找 name-* 目录
                for (auto& e : fs::directory_iterator(reg)) {
                    auto fn = e.path().filename().string();
                    if (fn == name || fn.substr(0, name.size() + 1) == name + "-") {
                        src = e.path(); break;
                    }
                }
            }
        }

        if (src.empty() || !fs::exists(src)) {
            std::cerr << ERR << name << "@" << spec << ": not in local registry\n"
                      << "  cd to the package directory and run 'flux publish'\n"
                      << "  Registry: " << reg << "\n";
            failed++; continue;
        }

        // 复制到 flux_packages/
        fs::create_directories(dest);
        for (auto& entry : fs::recursive_directory_iterator(src)) {
            auto rel = fs::relative(entry.path(), src);
            if (fs::is_directory(entry)) fs::create_directories(dest / rel);
            else {
                fs::create_directories(fs::path(dest) / rel.parent_path());
                fs::copy_file(entry.path(), dest / rel,
                              fs::copy_options::overwrite_existing);
            }
        }
        std::cout << OK << name << "@" << spec << "\n";
        installed++;
    }

    // 写 flux.lock
    { std::ofstream lock(lockPath(dir));
      lock << "# flux.lock — auto-generated, do not edit\n\n";
      for (auto& [name, spec] : m.deps) {
          lock << "[" << name << "]\n";
          if (toml::isTable(spec))
              lock << "source = \"local\"\npath = \""
                   << toml::tableGet(spec, "path") << "\"\n\n";
          else
              lock << "version = \"" << spec << "\"\n"
                   << "installed = \""
                   << (fs::exists(pkgDir + "/" + name) ? "true" : "false")
                   << "\"\n\n";
      }
    }

    std::cout << "\n" << installed << " installed";
    if (skipped)  std::cout << ", " << skipped  << " up-to-date";
    if (failed)   std::cout << ", " << failed   << " failed";
    std::cout << "\n";
}

// ── flux publish — 发布到本地 registry ───────────────────

void pkgPublish(const std::string& dir) {
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; return; }

    if (m.name.empty()) {
        std::cerr << ERR << "Package name not set in flux.toml\n"; return;
    }

    auto reg  = registryDir();
    auto dest = reg + "/" + m.name + "-" + m.version;
    fs::create_directories(reg);

    if (fs::exists(dest)) {
        std::cout << "Warning: " << m.name << "@" << m.version
                  << " already in registry. Overwriting.\n";
        fs::remove_all(dest);
    }

    static const std::vector<std::string> skip =
        {"flux_packages", ".git", "build", "flux.lock", ".gitignore"};

    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        auto rel   = fs::relative(entry.path(), dir);
        auto first = rel.begin()->string();
        if (std::find(skip.begin(), skip.end(), first) != skip.end()) continue;

        if (fs::is_directory(entry)) {
            fs::create_directories(dest / rel);
        } else {
            fs::create_directories(fs::path(dest / rel).parent_path());
            fs::copy_file(entry.path(), dest / rel,
                          fs::copy_options::overwrite_existing);
        }
    }

    std::cout << OK << "Published " << m.name << "@" << m.version << "\n"
              << DIM << "  Registry: " << dest << RST << "\n";
}

// ── flux search [query] ───────────────────────────────────

void pkgSearch(const std::string& query) {
    auto reg = registryDir();
    if (!fs::exists(reg)) {
        std::cout << "Local registry is empty.\n"
                  << "Use 'flux publish' from a package directory to add packages.\n";
        return;
    }

    std::cout << BLD << "Local registry packages:" << RST << "\n\n";
    bool found = false;
    // 按名排序
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(reg))
        if (fs::is_directory(e)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());

    for (auto& entry : entries) {
        auto fn = entry.filename().string();
        if (!query.empty() && fn.find(query) == std::string::npos) continue;

        std::string desc;
        auto mpath = entry / "flux.toml";
        if (fs::exists(mpath)) {
            try {
                auto doc = toml::loadFile(mpath.string());
                desc = toml::get(doc, "package", "description");
            } catch (...) {}
        }

        // 提取 name 和 version（格式：name-version）
        auto dash = fn.rfind('-');
        std::string dispName    = (dash != std::string::npos) ? fn.substr(0, dash) : fn;
        std::string dispVersion = (dash != std::string::npos) ? fn.substr(dash + 1) : "?";

        std::cout << "  " << BLD << dispName << RST
                  << " " << DIM << dispVersion << RST;
        if (!desc.empty()) std::cout << "  — " << desc;
        std::cout << "\n";
        found = true;
    }
    if (!found) {
        std::cout << "  (no packages" << (query.empty() ? "" : " matching '" + query + "'") << ")\n";
    }
}

// ── flux info <pkg> ───────────────────────────────────────

void pkgInfo(const std::string& name) {
    auto reg = registryDir();
    fs::path pkgPath;
    if (fs::exists(reg)) {
        for (auto& e : fs::directory_iterator(reg)) {
            auto fn = e.path().filename().string();
            if (fn == name || fn.substr(0, name.size() + 1) == name + "-") {
                pkgPath = e.path(); break;
            }
        }
    }
    if (pkgPath.empty()) {
        std::cerr << ERR << "Package '" << name << "' not in local registry.\n"; return;
    }

    try {
        auto doc = toml::loadFile((pkgPath / "flux.toml").string());
        std::cout << BLD << "Package:     " << RST << toml::get(doc, "package", "name")    << "\n"
                  << BLD << "Version:     " << RST << toml::get(doc, "package", "version") << "\n";
        auto desc   = toml::get(doc, "package", "description");
        auto author = toml::get(doc, "package", "author");
        auto fluxv  = toml::get(doc, "package", "flux");
        if (!desc.empty())   std::cout << BLD << "Description: " << RST << desc   << "\n";
        if (!author.empty()) std::cout << BLD << "Author:      " << RST << author << "\n";
        if (!fluxv.empty())  std::cout << BLD << "Requires:    " << RST << "Flux >= " << fluxv << "\n";

        // 列出 .flux 文件
        std::cout << BLD << "Files:\n" << RST;
        for (auto& f : fs::recursive_directory_iterator(pkgPath)) {
            if (f.path().extension() == ".flux")
                std::cout << "  " << fs::relative(f.path(), pkgPath).string() << "\n";
        }
    } catch (std::exception& e) {
        std::cerr << ERR << "Error: " << e.what() << "\n";
    }
}

// ── flux list ─────────────────────────────────────────────

void pkgList(const std::string& dir) {
    Manifest m;
    try { m = loadManifest(dir); }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; return; }

    std::cout << BLD << m.name << " v" << m.version << RST << "\n\n";

    if (m.deps.empty()) { std::cout << "No dependencies.\n"; return; }

    auto pkgDir = packagesDir(dir);
    std::cout << "Dependencies:\n";
    for (auto& [name, spec] : m.deps) {
        bool inst = fs::exists(pkgDir + "/" + name);
        std::cout << "  " << (inst ? OK : ERR) << name;
        if (toml::isTable(spec))
            std::cout << DIM << " (local: " << toml::tableGet(spec, "path") << ")" << RST;
        else
            std::cout << DIM << " @" << spec << RST;
        if (!inst) std::cout << "  \033[33m(not installed)\033[0m";
        std::cout << "\n";
    }

    // Scripts
    if (!m.scripts.empty()) {
        std::cout << "\nScripts:\n";
        for (auto& [name, file] : m.scripts)
            std::cout << "  " << BLD << name << RST << " → " << DIM << file << RST << "\n";
    }
}
