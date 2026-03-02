#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "interpreter.h"
#include "compiler.h"
#include "vm.h"
#include "watcher.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
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
            // 1. 初始化：注册函数 + persistent + 模块声明（解释器处理）
            interp.initProgram(program.get());
            // 2. 编译常规语句到字节码
            Chunk chunk;
            Compiler compiler(chunk, interp);
            compiler.compile(program.get());
            // 3. 用 VM 执行
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

// ── REPL 模式 ─────────────────────────────────────────────
static void runRepl() {
    Interpreter interp;
    std::cout << CLR_BOLD CLR_CYAN << "Flux REPL  (Ctrl+D to exit)\n" << CLR_RESET;
    std::cout << CLR_GRAY << "Type Flux code and press Enter.\n\n" << CLR_RESET;

    std::string line;
    while (true) {
        std::cout << CLR_CYAN << "flux> " << CLR_RESET;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        runSource(line, interp, false);
    }
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

// ── 入口 ──────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc == 1) {
        // 无参数 → REPL
        runRepl();
        return 0;
    }

    std::string arg = argv[1];

    if (arg == "--help" || arg == "-h") {
        std::cout << CLR_BOLD << "Flux Language\n" << CLR_RESET
                  << "  flux               启动 REPL\n"
                  << "  flux <file.flux>   运行文件（启用热更新）\n"
                  << "  flux --vm <file>   用字节码 VM 运行（Feature G）\n"
                  << "  flux --help        显示帮助\n";
        return 0;
    }

    // --vm 标志
    std::string filepath = arg;
    if (arg == "--vm") {
        if (argc < 3) {
            std::cerr << "Usage: flux --vm <file.flux>\n";
            return 1;
        }
        g_useVM   = true;
        filepath  = argv[2];
    }

    // 运行文件
    runFile(filepath);
    return 0;
}
