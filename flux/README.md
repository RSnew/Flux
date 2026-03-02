# Flux Language 🔥

A hot-reload-first scripting language with persistent state, module isolation,
supervised crash recovery, and an optional bytecode VM.

---

## Quick Start

```bash
# Build (C++17, ~4,500 lines)
cd flux && mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# Run with hot-reload (tree-walking interpreter)
./flux examples/hello.flux

# Run with bytecode VM (Feature G)
./flux --vm examples/language_demo.flux

# REPL
./flux
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
| J  | 工具链   | `flux check`, `flux fmt`, `flux run`, VSCode extension | 📋 Planned |
| K  | 并发模型 | `async`/`await`, Actor model, channels | 📋 Planned |
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

### 📋 Feature J — 工具链 (Toolchain)
- `flux check <file>` — type-check without running
- `flux fmt <file>` — auto-format source
- `flux run <file>` — run once, no file watcher
- `flux repl` — improved interactive REPL (multi-line, history)
- VSCode extension — syntax highlighting, hover types, diagnostics

### 📋 Feature K — 并发模型 (Concurrency Model)
- `async fn` / `await` — cooperative coroutines on a thread pool
- Actor model — each module becomes an actor with a message queue
- Channels — typed `Chan<T>` for inter-actor communication
- Backpressure — bounded channels, `select` statement

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
│   ├── interpreter.h/.cpp  Tree-walking interpreter (hot reload, modules)
│   ├── stdlib.cpp       Standard library (File, Json, Http, Time)
│   ├── compiler.h/.cpp  AST → bytecode compiler (Feature G)
│   ├── vm.h/.cpp        Stack-based bytecode VM (Feature G)
│   ├── watcher.h/.cpp   inotify file watcher (hot reload)
│   └── main.cpp         CLI: lex→parse→typecheck→interpret/VM
└── examples/
    ├── hello.flux
    ├── language_demo.flux
    ├── module_demo.flux
    ├── persistent_demo.flux
    ├── migrate_demo.flux
    ├── supervisor_demo.flux
    ├── stdlib_demo.flux
    └── test_array_interp_forin.flux
```

## Lines of Code

```
src/interpreter.cpp   ~930
src/stdlib.cpp        ~510
src/vm.cpp            ~420
src/compiler.cpp      ~310
src/parser.cpp        ~450
src/typechecker.cpp   ~430
src/vm.h              ~130
src/main.cpp          ~160
other headers/files   ~450
─────────────────────────
Total                ~3,790 lines of C++17
```
