# Flux Language

[简体中文](README.zh-CN.md)

A hot-reload-first programming language with persistent state, module isolation,
supervised crash recovery, and an optional bytecode VM.

Built in **~5,000 lines of C++17**. No external dependencies beyond pthreads.

> **Flux is not a scripting language.**
> It features a complete type system with HM inference, a bytecode compiler and stack-based VM,
> multi-level IR optimization (HIR → MIR → JIT/Codegen), module isolation with supervised crash recovery,
> and a full toolchain (type checker, formatter, package manager, LSP, debugger).
> Hot reload is a design choice, not a limitation.

---

## Quick Start

```bash
# Build
cd flux && mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# Run a script (with hot-reload file watcher)
./flux examples/hello.flux

# Run once (no watcher)
./flux run examples/hello.flux

# Interactive REPL
./flux
```

## Hello, Flux

```flux
var name = "World"
print("Hello, \(name)!")

func factorial(n) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
print("10! = \(factorial(10))")
```

## Language Highlights

**Variables, Constants & Types**
```flux
var x = 42                    // inferred
var name: String = "Flux"     // annotated
conf MAX = 100                // constant (read-only)
var nums = [1, 2, 3, 4, 5]   // arrays
var m = Map()                 // hash map
```

**Enums (Natural-only values)**
```flux
enum Direction {
    North = 0,
    South = 1,
    East = 2,
    West = 3
}
var heading = Direction.North
```

**Structs & Interfaces**
```flux
var Shape: interface = { func area() }

var Circle = Shape {
    radius: 1,
    func area() { return 3.14159 * self.radius * self.radius }
}

var c = Circle(radius: 5)
print(c.area())   // 78.54
```

**Modules with Persistent State** — state survives hot-reload
```flux
module Counter {
    persistent { count: 0 }
    func increment() { state.count = state.count + 1 }
    func getValue()  { return state.count }
}

Counter.increment()
print(Counter.getValue())
```

**Supervised Crash Recovery**
```flux
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    func charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}
```

**Thread Pool Concurrency**
```flux
@threadpool(name: "cpu-pool", size: 4)

@concurrent(pool: "cpu-pool")
module ImageProcessor {
    func resize(w, h) { return w * h }
}

var future = ImageProcessor.resize.async(1920, 1080)
var result = future.await()
```

**Arrays & Concatenation**
```flux
var a = [1, 2, 3]
var b = [4, 5, 6]
var c = a + b              // [1, 2, 3, 4, 5, 6]
a.push(99)                 // [1, 2, 3, 99]
print(a.len())             // 4
```

**AI-Native Types (`specify`)**
```flux
var validator = specify {
    intent: "Validate payment data",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "currency in [USD, EUR, CNY]"],
    examples: ["amount=100 currency=USD -> true"]
}

Specify.describe(validator)   // human-readable summary
Specify.schema(validator)     // structured Map
Specify.validate(validator, input)  // true/false
```

**Standard Library**
```flux
File.write("/tmp/out.txt", "hello\n")
var content = File.read("/tmp/out.txt")

var obj = Json.parse("{\"x\": 1}")
print(Json.pretty(obj))

Http.download("https://example.com/file.bin", "/tmp/file.bin")

var t0 = Time.now()
Time.sleep(100)
print("elapsed: \(Time.diff(t0, Time.now())) ms")
```

## Toolchain

| Command | Description |
|---------|-------------|
| `flux <file>` | Run with hot-reload (file watcher) |
| `flux dev <file>` | Dev mode — hot-reload + file watcher |
| `flux run <file>` | Run once |
| `flux --vm <file>` | Run with bytecode VM |
| `flux check <file>` | Type-check only |
| `flux fmt <file>` | Format source to stdout |
| `flux fmt -w <file>` | Format in-place |
| `flux repl` | Multi-line REPL with history |
| `flux compile <file>` | Compile to native binary (x86_64, arm64, riscv64) |
| `flux profile <file>` | Run with profiling on all functions |

### Package Manager

```bash
flux new myapp          # create project with flux.toml
flux add mathlib        # add dependency
flux install            # install all deps
flux build              # build & run
flux publish            # publish to local registry
```

### AI-Friendly Tooling

Flux provides first-class support for AI agent workflows with structured I/O and machine-readable introspection:

| Command | Description |
|---------|-------------|
| `flux check <file> --json` | Type-check with JSON error output |
| `flux inspect <file>` | List symbols, signatures & contracts (human-readable) |
| `flux inspect <file> --json` | Same, as structured JSON |
| `flux eval "<code>" --json` | Evaluate a code snippet, return JSON result |

**`specify` — Structured Specification Type**

Declare intent, constraints, and examples as first-class values:

```flux
var paymentValidator = specify {
    intent: "验证用户支付数据的合法性",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "currency in [USD, EUR, CNY]"],
    examples: ["amount=100 currency=USD -> true"]
}

Specify.describe(paymentValidator)  // human-readable summary
Specify.schema(paymentValidator)    // structured Map
Specify.intent(paymentValidator)    // → "验证用户支付数据的合法性"
Specify.validate(paymentValidator, input)  // → true/false
```

**Design-by-Contract — `requires` / `ensures`**

```flux
func divide(a, b)
requires { b != 0 }
ensures  { result != null }
{
    return a / b
}
```

AI agents can use `flux inspect --json` to discover contracts without parsing source code.

## Feature Status

| ID | Feature | Description | Status |
|----|---------|-------------|--------|
| H  | Language Core | Arrays, string interpolation `\()`, for-in, method calls | Done |
| I  | Standard Library | File IO, JSON, HTTP, Time, Map | Done |
| G  | Bytecode VM | 28-opcode stack VM, `--vm` flag | Done |
| J  | Toolchain | `check` / `fmt` / `run` / REPL / VSCode extension | Done |
| K  | Concurrency | Thread pools, `@concurrent`, `.async()` / `.await()`, channels | Done |
| L  | Package Manager | `flux.toml`, dependency resolution, local registry | Done |
| v1 | Spec v1.0 | Structs, interfaces, `func`, `!var`, intervals, `exception` | Done |
| v2 | Constants & Enums | `conf` constants, `enum` with Natural-only values | Done |
| v2 | Pre-exec Checks | Interface completeness, enum validation | Done |
| v2 | AI-Native Types | `specify` declarations, `Specify.validate/describe/schema`, contract-based programming | Done |
| v2 | Native Compilation | `flux compile` to x86_64/arm64/riscv64 binary, `flux profile` | Done |
| M  | Self-hosting | Flux compiles Flux | Planned |

## Design Philosophy — Why Not Rust / C++?

Flux is not a systems language. Every "missing" feature is an intentional trade-off
for hot-reload-first development. See [`docs/design-comparison.md`](docs/design-comparison.md) for full details.

| Design Choice | Rust / C++ | Flux | Why |
|---------------|-----------|------|-----|
| Type system | Static, generics / templates | Dynamic + HM inference (no `<T>` syntax) | Faster save→effect loop; type complexity hidden from user |
| Memory | Ownership / RAII | GC (shared_ptr + cycle detection) | GC overhead ≪ compile-time cost of borrow checking for hot-reload workflows |
| Concurrency | No GIL, true parallelism | GIL + thread pools + channels | GIL structurally eliminates data races; I/O parallelism unaffected |
| Error handling | `Result<T,E>` / exceptions | `exception` + `default` + `@supervised` | Errors isolated at module boundary; supervisor auto-recovers |
| Polymorphism | Traits / virtual functions | Duck-typed interfaces | No `impl` boilerplate; hot-reload friendly |
| Branching | `match` / `switch` | `if value { else: pattern { } }` | Covers value matching; persistent state handles state machines |
| AI integration | External tooling / prompts | `specify` as first-class type + `flux inspect --json` | Contracts and intent are part of the language, not comments |
| Dev feedback | Edit → compile → run | Edit → save → **instant effect** | The reason Flux exists |

## Project Structure

```
Flux/
├── flux/                       # Language implementation
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp            CLI entry point
│   │   ├── token.h             TokenType enum (keywords, operators, literals)
│   │   ├── lexer.h/.cpp        Lexer (string interpolation, all tokens)
│   │   ├── parser.h/.cpp       Recursive-descent parser
│   │   ├── ast.h               33 AST node types
│   │   ├── typechecker.h/.cpp  Type inference & annotation checking
│   │   ├── interpreter.h/.cpp  Tree-walking interpreter (hot reload, modules)
│   │   ├── compiler.h/.cpp     AST → bytecode compiler
│   │   ├── vm.h/.cpp           Stack-based bytecode VM
│   │   ├── stdlib.cpp          File, Json, Http, Time, Chan, Math, Specify
│   │   ├── concurrency.h       GIL + thread safety
│   │   ├── threadpool.h        Thread pools with overflow policies
│   │   ├── formatter.h         AST → source pretty-printer
│   │   ├── watcher.h/.cpp      inotify file watcher
│   │   ├── toml.h              Zero-dep TOML parser
│   │   ├── pkgmgr.h/.cpp      Package manager
│   │   ├── lsp.h               Language Server Protocol support
│   │   ├── debugger.h          Debugger (breakpoints, step, inspect)
│   │   ├── profiler.h          Performance profiler
│   │   ├── gc.h                Garbage collector
│   │   ├── hir.h / mir.h       High/Mid-level IR for optimization
│   │   ├── jit.h               JIT compiler
│   │   ├── codegen.h           Native code generation
│   │   └── fluz.h              Binary format protection (.fluz)
│   └── examples/               23 demo scripts
├── vscode-flux/                VSCode extension (syntax, snippets, folding)
├── Flux Language Spec.docx     Language specification document
└── README.md                   ← you are here
```

See [`flux/README.md`](flux/README.md) for detailed documentation including
bytecode instruction set, formatter internals, REPL commands, and package
manager architecture.

## Build Requirements

- C++17 compiler (GCC 7+ / Clang 5+)
- CMake 3.16+
- pthreads (for file watcher & concurrency)

## License

See repository for license information.
