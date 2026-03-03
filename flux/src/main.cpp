#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "interpreter.h"
#include "compiler.h"
#include "vm.h"
#include "formatter.h"
#include "watcher.h"
#include "pkgmgr.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

// ── ANSI 颜色 ─────────────────────────────────────────────
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_CYAN   "\033[36m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_RED    "\033[31m"
#define CLR_GRAY   "\033[90m"

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void printBanner(const std::string& file) {
    std::cout << CLR_BOLD CLR_CYAN
              << "╔══════════════════════════════════════╗\n"
              << "║       Flux Language Runtime          ║\n"
              << "║       Hot Reload: ON  🔥             ║\n"
              << "╚══════════════════════════════════════╝\n"
              << CLR_RESET;
    std::cout << CLR_GRAY << "  Watching: " << file << CLR_RESET << "\n\n";
}

// ── 编译并执行（热更新时复用解释器状态）──────────────────
static bool g_useVM = false;   // --vm 标志

static void runSource(const std::string& source, Interpreter& interp, bool isReload) {
    try {
        Lexer  lexer(source);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        // ── 类型检查（在执行前）──────────────────────────
        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            std::cerr << CLR_RED CLR_BOLD
                      << "\n🔴 Type errors found — execution blocked:\n"
                      << CLR_RESET;
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "   ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            std::cerr << "\n";
            return;
        }

        if (isReload) {
            std::cout << CLR_YELLOW << "\n🔄 Hot reload detected..." << CLR_RESET << "\n";
            std::cout << CLR_GRAY   << "─────────────────────────────\n" << CLR_RESET;
        }

        if (g_useVM) {
            // ── 字节码 VM 路径 ─────────────────────────────
            interp.initProgram(program.get());
            Chunk chunk;
            Compiler compiler(chunk, interp);
            compiler.compile(program.get());
            VM vm(interp);
            vm.run(chunk, interp.globalEnv_);
        } else {
            interp.execute(program.get());
        }

        if (isReload)
            std::cout << CLR_GREEN << "✅ Hot reload complete\n" << CLR_RESET;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "\n❌ Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "\n❌ Runtime error: " << e.what() << CLR_RESET << "\n";
    }
}

// ── REPL 专用执行（保留跨调用状态）────────────────────────
// keepAlive: REPL 会话中所有 Program 的生命周期容器，
// 防止 FnDecl* 等原始指针在 Interpreter::functions_ 中悬空。
static void runSourceRepl(const std::string& source,
                          Interpreter& interp,
                          std::vector<std::unique_ptr<Program>>& keepAlive) {
    try {
        Lexer  lexer(source);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "  ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            return;
        }

        interp.executeRepl(program.get());
        // 将 program 所有权转移到 keepAlive，防止 AST 被销毁
        keepAlive.push_back(std::move(program));

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "❌ Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "❌ Runtime error: " << e.what() << CLR_RESET << "\n";
    }
}

// ═══════════════════════════════════════════════════════════
// flux run <file> — 运行一次，无热更新循环
// ═══════════════════════════════════════════════════════════
static int cmdRun(const std::string& filepath) {
    Interpreter interp;
    try {
        runSource(readFile(filepath), interp, false);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// flux check <file> — 类型检查，不执行
// ═══════════════════════════════════════════════════════════
static int cmdCheck(const std::string& filepath) {
    try {
        std::string src = readFile(filepath);

        Lexer  lexer(src);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        TypeChecker checker;
        auto errors = checker.check(program.get());

        if (errors.empty()) {
            std::cout << CLR_GREEN << "✅ No type errors found in: "
                      << filepath << CLR_RESET << "\n";
            return 0;
        }

        std::cerr << CLR_RED CLR_BOLD << "Type errors in: "
                  << filepath << CLR_RESET << "\n";
        for (auto& e : errors) {
            std::cerr << CLR_RED << "  ";
            if (e.line > 0) std::cerr << "line " << e.line << ": ";
            std::cerr << e.message << CLR_RESET << "\n";
        }
        return 1;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// flux fmt <file> [--write] — 格式化 Flux 源代码
//   默认输出到 stdout；--write / -w 覆写原文件
// ═══════════════════════════════════════════════════════════
static int cmdFmt(const std::string& filepath, bool writeBack) {
    try {
        std::string src = readFile(filepath);

        Lexer  lexer(src);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        Formatter fmt;
        std::string formatted = fmt.format(program.get());

        if (writeBack) {
            std::ofstream f(filepath);
            if (!f) {
                std::cerr << CLR_RED << "Cannot write to: " << filepath
                          << CLR_RESET << "\n";
                return 1;
            }
            f << formatted;
            std::cout << CLR_GREEN << "✅ Formatted: " << filepath
                      << CLR_RESET << "\n";
        } else {
            std::cout << formatted;
        }
        return 0;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// REPL 多行支持：统计 { } 深度，决定何时执行
// ═══════════════════════════════════════════════════════════
static void updateBraceDepth(const std::string& line, int& depth) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i-1] != '\\')) { inStr = !inStr; continue; }
        if (inStr) continue;
        if (c == '/' && i + 1 < line.size() && line[i+1] == '/') break; // 注释
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
}

// ── REPL 模式（改进：多行输入 + 命令历史）──────────────
static void cmdRepl() {
    Interpreter interp;
    std::cout << CLR_BOLD CLR_CYAN
              << "Flux REPL  (Ctrl+D to exit)\n"
              << CLR_RESET;
    std::cout << CLR_GRAY
              << "Multi-line blocks supported. Special commands:\n"
              << "  .clear   — reset interpreter state\n"
              << "  .history — show command history\n"
              << "  .help    — show this help\n\n"
              << CLR_RESET;

    // keepAlive: 让 Program 的 AST 在整个 REPL 会话中存活，
    // 避免 interpreter 的 functions_ 持有悬空 FnDecl*。
    std::vector<std::unique_ptr<Program>> keepAlive;

    std::vector<std::string> history;
    std::string buffer;
    int braceDepth = 0;

    while (true) {
        // 主提示符 vs 续行提示符
        if (buffer.empty()) {
            std::cout << CLR_CYAN << "flux> " << CLR_RESET;
        } else {
            std::cout << CLR_CYAN << "...   " << CLR_RESET;
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) break;  // Ctrl+D

        if (line.empty() && buffer.empty()) continue;

        // ── 特殊 REPL 命令 ─────────────────────────────
        if (buffer.empty()) {
            if (line == ".clear") {
                interp = Interpreter();
                keepAlive.clear();  // AST 可以安全销毁（functions_ 已随 interp 重置）
                std::cout << CLR_GRAY << "(Interpreter state cleared)\n" << CLR_RESET;
                continue;
            }
            if (line == ".history") {
                if (history.empty()) {
                    std::cout << CLR_GRAY << "(no history)\n" << CLR_RESET;
                } else {
                    for (size_t i = 0; i < history.size(); i++) {
                        // 多行条目缩进显示
                        std::string entry = history[i];
                        bool first = true;
                        std::istringstream ss(entry);
                        std::string part;
                        while (std::getline(ss, part)) {
                            std::cout << CLR_GRAY;
                            if (first) std::cout << "[" << (i + 1) << "] ";
                            else       std::cout << "    ";
                            std::cout << CLR_RESET << part << "\n";
                            first = false;
                        }
                    }
                }
                continue;
            }
            if (line == ".help") {
                std::cout << CLR_GRAY
                          << "  .clear   — reset interpreter state\n"
                          << "  .history — show command history\n"
                          << "  .help    — show this help\n"
                          << CLR_RESET;
                continue;
            }
        }

        // ── 积累输入 ────────────────────────────────────
        if (!buffer.empty()) buffer += "\n";
        buffer += line;
        updateBraceDepth(line, braceDepth);

        // 所有大括号已闭合 → 执行
        if (braceDepth <= 0) {
            braceDepth = 0;
            if (!buffer.empty()) {
                history.push_back(buffer);
                runSourceRepl(buffer, interp, keepAlive);
            }
            buffer.clear();
        }
    }

    // 若有未完成的输入（Ctrl+D 打断），尝试执行
    if (!buffer.empty()) {
        history.push_back(buffer);
        runSource(buffer, interp, false);
    }

    std::cout << "\n" << CLR_GRAY << "Bye!\n" << CLR_RESET;
}

// ── 文件执行 + 热更新模式 ─────────────────────────────────
static void runFile(const std::string& filepath) {
    Interpreter interp;

    printBanner(filepath);

    // 首次执行
    try {
        runSource(readFile(filepath), interp, false);
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error reading file: " << e.what() << CLR_RESET << "\n";
        return;
    }

    // 启动文件监听（热更新）
    std::mutex reloadMutex;
    FileWatcher watcher(filepath, [&](const std::string& path) {
        std::lock_guard<std::mutex> lock(reloadMutex);
        try {
            std::string src = readFile(path);
            runSource(src, interp, true);
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "File read error: " << e.what() << CLR_RESET << "\n";
        }
    });

    watcher.start();
    std::cout << CLR_GRAY << "\n[Waiting for file changes... Ctrl+C to exit]\n" << CLR_RESET;

    // 主线程阻塞
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ── 帮助信息 ──────────────────────────────────────────────
static void printHelp() {
    std::cout
        << CLR_BOLD << "Flux Language — Toolchain\n\n" << CLR_RESET
        << CLR_BOLD << "Usage:\n" << CLR_RESET
        << "  flux                        Start interactive REPL\n"
        << "  flux repl                   Start interactive REPL (explicit)\n"
        << "  flux <file.flux>            Run file with hot-reload (watch mode)\n"
        << "  flux run   <file.flux>      Run file once, no file watcher\n"
        << "  flux check <file.flux>      Type-check file, do not execute\n"
        << "  flux fmt   <file.flux>      Format file to stdout\n"
        << "  flux fmt   -w <file.flux>   Format file in-place (overwrite)\n"
        << "  flux --vm  <file.flux>      Run file with bytecode VM (Feature G)\n"
        << "  flux --help                 Show this help\n"
        << "\n"
        << CLR_BOLD << "Package Manager (Feature L):\n" << CLR_RESET
        << "  flux new    <name>          Create a new Flux project\n"
        << "  flux build  [script]        Build & run project (default: run)\n"
        << "  flux add    <pkg>[@ver]     Add dependency to flux.toml\n"
        << "  flux add    <pkg> path=..   Add local path dependency\n"
        << "  flux remove <pkg>           Remove dependency\n"
        << "  flux install                Install all dependencies from flux.toml\n"
        << "  flux publish                Publish to local registry (~/.flux/packages)\n"
        << "  flux search [query]         Search local registry\n"
        << "  flux info   <pkg>           Show package info\n"
        << "  flux list                   List project dependencies\n"
        << "\n"
        << CLR_BOLD << "Examples:\n" << CLR_RESET
        << "  flux new myapp\n"
        << "  flux add mathlib@1.0.0\n"
        << "  flux install\n"
        << "  flux build\n"
        << "  flux run examples/hello.flux\n"
        << "  flux check examples/module_demo.flux\n"
        << "  flux fmt -w examples/stdlib_demo.flux\n"
        << "  flux --vm examples/language_demo.flux\n";
}

// ── 入口 ──────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc == 1) {
        cmdRepl();
        return 0;
    }

    std::string sub = argv[1];

    // ── --help ───────────────────────────────────────────
    if (sub == "--help" || sub == "-h" || sub == "help") {
        printHelp();
        return 0;
    }

    // ── flux repl ────────────────────────────────────────
    if (sub == "repl") {
        cmdRepl();
        return 0;
    }

    // ── flux run <file> ──────────────────────────────────
    if (sub == "run") {
        if (argc < 3) {
            std::cerr << "Usage: flux run <file.flux>\n";
            return 1;
        }
        return cmdRun(argv[2]);
    }

    // ── flux check <file> ────────────────────────────────
    if (sub == "check") {
        if (argc < 3) {
            std::cerr << "Usage: flux check <file.flux>\n";
            return 1;
        }
        return cmdCheck(argv[2]);
    }

    // ── flux fmt [-w] <file> ─────────────────────────────
    if (sub == "fmt") {
        if (argc < 3) {
            std::cerr << "Usage: flux fmt [-w] <file.flux>\n";
            return 1;
        }
        bool writeBack = false;
        std::string filepath = argv[2];
        if (filepath == "-w" || filepath == "--write") {
            writeBack = true;
            if (argc < 4) {
                std::cerr << "Usage: flux fmt -w <file.flux>\n";
                return 1;
            }
            filepath = argv[3];
        }
        return cmdFmt(filepath, writeBack);
    }

    // ══════════════════════════════════════════════════════
    // Feature L: 包管理器命令
    // ══════════════════════════════════════════════════════

    // ── flux new <name> ──────────────────────────────────
    if (sub == "new") {
        if (argc < 3) {
            std::cerr << "Usage: flux new <project-name>\n";
            return 1;
        }
        try { pkgNew(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux build [script] ──────────────────────────────
    // 读取 flux.toml，合并 flux_packages/*.flux + 入口文件，然后执行
    if (sub == "build") {
        std::string script = argc > 2 ? argv[2] : "run";
        try {
            auto r = pkgBuild(".", script);
            if (!r.ok) {
                std::cerr << CLR_RED << "Build error: " << r.error << CLR_RESET << "\n";
                return 1;
            }
            Interpreter interp;
            runSource(r.source, interp, false);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux add <pkg>[@version] [path=../lib] ───────────
    if (sub == "add") {
        if (argc < 3) {
            std::cerr << "Usage: flux add <pkg>[@version]  or  flux add <pkg> path=<dir>\n";
            return 1;
        }
        // 收集 argv[2..] 拼成一个 spec 字符串（兼容 "pkg path=../lib"）
        std::string spec = argv[2];
        if (argc > 3) spec += std::string(" ") + argv[3];
        try { pkgAdd(spec); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux remove <pkg> ────────────────────────────────
    if (sub == "remove") {
        if (argc < 3) {
            std::cerr << "Usage: flux remove <pkg>\n";
            return 1;
        }
        try { pkgRemove(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux install ─────────────────────────────────────
    if (sub == "install") {
        try { pkgInstall(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux publish ─────────────────────────────────────
    if (sub == "publish") {
        try { pkgPublish(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux search [query] ──────────────────────────────
    if (sub == "search") {
        std::string query = argc > 2 ? argv[2] : "";
        try { pkgSearch(query); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux info <pkg> ──────────────────────────────────
    if (sub == "info") {
        if (argc < 3) {
            std::cerr << "Usage: flux info <pkg>\n";
            return 1;
        }
        try { pkgInfo(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux list ────────────────────────────────────────
    if (sub == "list") {
        try { pkgList(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux --vm <file> ────────────────────────────────
    if (sub == "--vm") {
        if (argc < 3) {
            std::cerr << "Usage: flux --vm <file.flux>\n";
            return 1;
        }
        g_useVM  = true;
        runFile(argv[2]);
        return 0;
    }

    // ── flux <file.flux> — 热更新模式 ───────────────────
    runFile(sub);
    return 0;
}
