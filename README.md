# Flux Language

[简体中文](README.zh-CN.md)

A hot-reload-first scripting language with persistent state, module isolation,
supervised crash recovery, and an optional bytecode VM.

Built in **~5,000 lines of C++17**. No external dependencies beyond pthreads.

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

fn factorial(n) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
print("10! = \(factorial(10))")
```

## Language Highlights

**Variables & Types**
```flux
var x = 42                    // inferred
var name: String = "Flux"     // annotated
var nums = [1, 2, 3, 4, 5]   // arrays
var m = Map()                 // hash map
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
    fn increment() { state.count = state.count + 1 }
    fn getValue()  { return state.count }
}

Counter.increment()
print(Counter.getValue())
```

**Supervised Crash Recovery**
```flux
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    fn charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}
```

**Thread Pool Concurrency**
```flux
@threadpool(name: "cpu-pool", size: 4)

@concurrent(pool: "cpu-pool")
module ImageProcessor {
    fn resize(w, h) { return w * h }
}

var future = ImageProcessor.resize.async(1920, 1080)
var result = future.await()
```

**Standard Library**
```flux
File.write("/tmp/out.txt", "hello\n")
var content = File.read("/tmp/out.txt")

var obj = Json.parse("{\"x\": 1}")
print(Json.pretty(obj))

var t0 = Time.now()
Time.sleep(100)
print("elapsed: \(Time.diff(t0, Time.now())) ms")
```

## Toolchain

| Command | Description |
|---------|-------------|
| `flux <file>` | Run with hot-reload (file watcher) |
| `flux run <file>` | Run once |
| `flux --vm <file>` | Run with bytecode VM |
| `flux check <file>` | Type-check only |
| `flux fmt <file>` | Format source to stdout |
| `flux fmt -w <file>` | Format in-place |
| `flux repl` | Multi-line REPL with history |

### Package Manager

```bash
flux new myapp          # create project with flux.toml
flux add mathlib        # add dependency
flux install            # install all deps
flux build              # build & run
flux publish            # publish to local registry
```

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
| M  | Self-hosting | Flux compiles Flux | Planned |

## Project Structure

```
Flux/
├── flux/                       # Language implementation
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp            CLI entry point
│   │   ├── lexer.h/.cpp        Lexer (string interpolation, all tokens)
│   │   ├── parser.h/.cpp       Recursive-descent parser
│   │   ├── ast.h               33 AST node types
│   │   ├── typechecker.h/.cpp  Type inference & annotation checking
│   │   ├── interpreter.h/.cpp  Tree-walking interpreter (hot reload, modules)
│   │   ├── compiler.h/.cpp     AST → bytecode compiler
│   │   ├── vm.h/.cpp           Stack-based bytecode VM
│   │   ├── stdlib.cpp          File, Json, Http, Time, Chan
│   │   ├── concurrency.h       GIL + thread safety
│   │   ├── threadpool.h        Thread pools with overflow policies
│   │   ├── formatter.h         AST → source pretty-printer
│   │   ├── watcher.h/.cpp      inotify file watcher
│   │   ├── toml.h              Zero-dep TOML parser
│   │   └── pkgmgr.h/.cpp      Package manager
│   └── examples/               11 demo scripts
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
