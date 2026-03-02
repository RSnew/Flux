# Flux Language 🔥

A hot-reload-first scripting language with persistent state, module isolation,
supervised crash recovery, and an optional bytecode VM.

---

## Quick Start

```bash
# Build (C++17, ~4,700 lines)
cd flux && mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# Run once (no file watcher)
./flux run examples/hello.flux

# Run with hot-reload (watch mode)
./flux examples/hello.flux

# Run with bytecode VM (Feature G)
./flux --vm examples/language_demo.flux

# Type-check only
./flux check examples/module_demo.flux

# Format source to stdout
./flux fmt examples/hello.flux

# Format in-place
./flux fmt -w examples/hello.flux

# REPL (multi-line, history)
./flux
./flux repl
```

---

## Syntax Overview

```swift
// ── Variables & types ──────────────────────────────────────
let name  = "Flux"          // inferred String
let count: Int = 0          // annotated
var x = 3.14                // mutable

// ── String interpolation ───────────────────────────────────
print("Hello, \(name)!  x = \(x * 2)")

// ── Arrays & for-in ───────────────────────────────────────
let nums = [1, 2, 3, 4, 5]
for n in nums { print(n * n) }

// ── Functions ──────────────────────────────────────────────
fn add(a: Int, b: Int) -> Int { return a + b }
fn greet(who) { print("Hi, \(who)!") }

// ── Persistent state (survives hot reload) ─────────────────
persistent { visits: 0 }
state.visits = state.visits + 1
print("Visit #\(state.visits)")

// ── Modules with isolated state ────────────────────────────
module Counter {
    persistent { count: 0 }
    fn increment()       { state.count = state.count + 1 }
    fn getValue() -> Int { return state.count }
}
Counter.increment()
print(Counter.getValue())

// ── Schema migration (hot reload safety) ───────────────────
module Analytics {
    persistent { pageViews: 0, errors: 0 }
    migrate    { errors: 0 }   // required when adding new fields
}

// ── Supervised crash isolation ─────────────────────────────
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    fn charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}

// ── Standard library ───────────────────────────────────────
File.write("/tmp/out.txt", "hello\n")
let lines = File.lines("/tmp/out.txt")

let obj = Json.parse("{\"x\": 1, \"y\": [2, 3]}")
print(obj["x"], Json.pretty(obj))

let t0 = Time.now()
Time.sleep(10)
print("elapsed: \(Time.diff(t0, Time.now())} s")

// Http (requires HTTP server; HTTPS needs libcurl):
// let resp = Http.get("http://api.example.com/data")

// ── Map type ───────────────────────────────────────────────
let m = Map()
m["key"] = "value"
for k in m { print(k, "→", m[k]) }
print(Json.stringify(m))
```

---

## Feature Status

| ID | Feature | 描述 | Status |
|----|---------|------|--------|
| H  | 语言完善 | Arrays, string interpolation `\()`, for-in, method calls | ✅ Done |
| I  | 标准库   | File IO, JSON (zero-dep parser), HTTP (POSIX socket), Time | ✅ Done |
| G  | 字节码VM | Stack-based bytecode VM + compiler; `--vm` flag; modules via tree-walker | ✅ Done |
| J  | 工具链   | `flux check`, `flux fmt`, `flux run`, improved REPL, VSCode extension | ✅ Done |
| K  | 并发模型 | `async`/`await`, GIL-based threads, `Chan` channels, `spawn` | ✅ Done |
| L  | 包管理器 | `flux.toml`, dependency resolution, registry | 📋 Planned |
| M  | 自举编译器 | Flux compiles Flux (self-hosting) | 🔭 Future |

---

## Roadmap Details

### ✅ Feature H — 语言完善 (Language Completion)
- **Arrays**: literals `[1,2,3]`, indexing `arr[i]`, methods `push/pop/join/contains/reverse`
- **String interpolation**: `"value = \(expr)"` (lexer-level expansion)
- **for-in loops**: over `Array`, `String` (chars), `Map` (keys), `range(n)`
- **Method calls**: `obj.method(args)` for Array, String, Map types
- **Negative indexing**: `arr[-1]` → last element

### ✅ Feature I — 标准库 (Standard Library)

| Module | Functions |
|--------|-----------|
| `File` | `read`, `write`, `append`, `exists`, `lines`, `delete` |
| `Json` | `parse`, `stringify`, `pretty` |
| `Http` | `get`, `post`, `put`, `delete` (HTTP only; HTTPS needs libcurl) |
| `Time` | `now`, `clock`, `sleep`, `format`, `diff` |

Also adds **`Map` type** (`Map()` builtin, `m[key]`, `m.keys()`, `m.values()`, `m.entries()`).

### ✅ Feature G — 字节码VM (Bytecode VM)

A stack-based bytecode virtual machine replacing the tree-walking interpreter for
non-module top-level code.

**Architecture:**
- `Chunk` — instruction sequence + constant pool + name pool
- `Compiler` — walks AST, emits `OpCode` instructions
- `VM` — single-threaded dispatch loop; delegates builtins/modules to `Interpreter`

**Instruction set (28 opcodes):**
`PUSH_NIL` `PUSH_TRUE` `PUSH_FALSE` `PUSH_CONST` `LOAD` `STORE` `DEFINE`
`LOAD_STATE` `STORE_STATE` `ADD` `SUB` `MUL` `DIV` `MOD` `NEG` `NOT`
`EQ` `NEQ` `LT` `GT` `LEQ` `GEQ`
`PUSH_SCOPE` `POP_SCOPE` `POP` `JUMP` `JUMP_IF_FALSE` `JUMP_IF_TRUE`
`CALL` `CALL_MODULE` `CALL_METHOD` `RETURN` `RETURN_NIL`
`MAKE_ARRAY` `MAKE_MAP` `INDEX_GET` `INDEX_SET`

**Short-circuit `&&` / `||`** — compiled with patch-back jumps.
**`for-in`** — expanded to index loop with hidden `__iter_N` / `__idx_N` variables.
**Modules** — still handled by the tree-walking interpreter (`initProgram()` phase).

### ✅ Feature J — 工具链 (Toolchain)

| Command | Description |
|---------|-------------|
| `flux run <file>` | Run file once (no file watcher) |
| `flux check <file>` | Type-check without executing; exit 0 = clean |
| `flux fmt <file>` | Format source to stdout (canonical style) |
| `flux fmt -w <file>` | Format in-place (overwrite) |
| `flux repl` | Multi-line REPL with history (`.history`, `.clear`, `.help`) |

**Formatter** (`src/formatter.h`) — AST-to-source pretty-printer:
- 4-space indentation
- Opening `{` on same line as keyword
- Operator spacing normalized
- Correct parenthesization via precedence-aware recursive descent

**REPL improvements** over the original:
- Multi-line block support (brace depth counting)
- Session history (`.history` command)
- State preserved across inputs: variables, functions, persistent blocks
- `.clear` resets interpreter; `.help` lists commands

**VSCode Extension** (`vscode-flux/`):
- Syntax highlighting via TextMate grammar (`flux.tmLanguage.json`)
- Bracket matching and auto-closing
- Code folding on `{` blocks
- Snippets: `fn`, `module`, `if`, `while`, `for`, `persistent`, …

### ✅ Feature K — 并发模型 (Concurrency Model) v2

**线程池声明** — 用户显式控制，语言保证安全：
```flux
@threadpool(name: "io-pool", size: 4)    // 声明 IO 线程池
@threadpool(name: "cpu-pool", size: 2)   // 声明 CPU 线程池
```

**模块绑定** — 一行注解绑定到对应线程池：
```flux
@concurrent(pool: "io-pool")                                       // 默认: overflow: .block
@concurrent(pool: "io-pool", queue: 32, overflow: .drop)           // 日志/采样
@concurrent(pool: "order-pool", queue: 10000, overflow: .error)    // 金融关键路径
module MyService { ... }
```

**跨 pool 异步调用** — `.async()` 方法语法：
```flux
let f = ImageProcessor.resize.async(1920, 1080)   // 提交到 cpu-pool，立即返回 Future
let result = f.await()                             // 等待结果
let result = f.await(500)                          // 带 500ms 超时
```

**溢出策略**：
| 策略 | 说明 |
|------|------|
| `.block` (默认) | 阻塞发送方直到有空位，不丢数据 |
| `.drop` | 静默丢弃（适合日志/采样） |
| `.error` | 抛出异常（适合金融关键路径） |

**Channels & 向后兼容**：
- `Chan.make()` / `Chan.make(cap)` — 无界/有界线程安全通道
- `ch.send(v)`, `ch.recv()`, `ch.tryRecv()`, `ch.close()`
- `async fn(args)` / `await future` 关键字语法（向后兼容保留）
- `spawn { ... }` — 独立后台任务
- `nil` — 一等公民字面量（用于通道关闭检测）
- `fut.isReady()` — Future 状态轮询
- Example: `examples/concurrency_demo.flux`

### 📋 Feature L — 包管理器 (Package Manager)
- `flux.toml` — project manifest (name, version, deps, scripts)
- `flux add <pkg>` — fetch and pin dependency
- `flux build` — build with dependency graph
- Central registry at `pkg.fluxlang.dev`

### 🔭 Feature M — 自举编译器 (Self-hosting Compiler)
- Flux compiles Flux
- Replaces the C++ compiler/VM with a Flux-written backend
- Milestone: first self-hosted release

---

## Project Structure

```
flux/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── token.h          Token types
│   ├── lexer.h/.cpp     Lexer (string interpolation, all token types)
│   ├── ast.h            AST node definitions (26 node types)
│   ├── parser.h/.cpp    Recursive-descent parser
│   ├── typechecker.h/.cpp  Type inference + annotation checking
│   ├── concurrency.h    GIL + GILGuard + GILRelease (Feature K)
│   ├── threadpool.h     ThreadPool with overflow policies (Feature K v2)
│   ├── interpreter.h/.cpp  Tree-walking interpreter (hot reload, modules, async/await/spawn)
│   ├── stdlib.cpp       Standard library (File, Json, Http, Time, Chan)
│   ├── compiler.h/.cpp  AST → bytecode compiler (Feature G)
│   ├── vm.h/.cpp        Stack-based bytecode VM (Feature G)
│   ├── formatter.h      AST → source code formatter (Feature J)
│   ├── watcher.h/.cpp   inotify file watcher (hot reload)
│   └── main.cpp         CLI: run/check/fmt/repl/--vm
├── vscode-flux/         VSCode extension (Feature J)
│   ├── package.json
│   ├── language-configuration.json
│   ├── syntaxes/flux.tmLanguage.json
│   ├── snippets/flux.json
│   └── extension.js
└── examples/
    ├── hello.flux
    ├── language_demo.flux
    ├── module_demo.flux
    ├── persistent_demo.flux
    ├── migrate_demo.flux
    ├── supervisor_demo.flux
    ├── stdlib_demo.flux
    ├── concurrency_demo.flux
    └── test_array_interp_forin.flux
```

## Lines of Code

```
src/interpreter.cpp   ~960
src/stdlib.cpp        ~510
src/vm.cpp            ~420
src/compiler.cpp      ~310
src/parser.cpp        ~450
src/typechecker.cpp   ~430
src/formatter.h       ~220
src/vm.h              ~130
src/main.cpp          ~350
other headers/files   ~450
─────────────────────────
Total                ~4,230 lines of C++17
```
