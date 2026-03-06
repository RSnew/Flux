# Flux Language Project

## Overview

Flux is a hot-reload-first scripting language built in ~5,000 lines of C++17.
Key features: persistent state, module isolation, supervised crash recovery, optional bytecode VM.
No external dependencies beyond pthreads and OpenSSL.

## Build

```bash
cd flux && mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)
```

## Project Structure

```
Flux/
├── flux/src/              # C++17 language implementation
│   ├── main.cpp           # CLI entry: run/watch/repl/check/fmt/vm/pkg commands
│   ├── token.h            # TokenType enum (keywords, operators, literals)
│   ├── lexer.h/.cpp       # Lexer with string interpolation \()
│   ├── parser.h/.cpp      # Recursive-descent parser → AST
│   ├── ast.h              # 33 AST node types (expressions + statements)
│   ├── typechecker.h/.cpp # Type inference & annotation checking
│   ├── interpreter.h/.cpp # Tree-walking interpreter (hot reload, modules, persistent state)
│   ├── compiler.h/.cpp    # AST → bytecode compiler
│   ├── vm.h/.cpp          # 28-opcode stack-based bytecode VM
│   ├── stdlib.cpp         # Standard library: File, Json, Http, Time, Map, Chan
│   ├── concurrency.h      # GIL + thread safety primitives
│   ├── threadpool.h       # Thread pools with overflow policies (block/drop/error)
│   ├── formatter.h        # AST → formatted source pretty-printer
│   ├── watcher.h/.cpp     # inotify-based file watcher for hot reload
│   ├── toml.h             # Zero-dependency TOML parser
│   ├── pkgmgr.h/.cpp      # Package manager (flux.toml, dependency resolution)
│   ├── lsp.h              # Language Server Protocol support
│   ├── debugger.h         # Debugger (breakpoints, step, inspect)
│   ├── profiler.h         # Performance profiler
│   ├── gc.h               # Garbage collector
│   ├── hir.h / mir.h      # High/Mid-level IR for optimization
│   ├── jit.h              # JIT compiler
│   ├── codegen.h          # Native code generation
│   └── fluz.h             # Binary format protection (.fluz)
├── flux/examples/         # 13 demo .flux scripts
├── vscode-flux/           # VSCode extension (syntax highlighting, snippets)
└── README.md / README.zh-CN.md
```

## Architecture Pipeline

```
Source (.flux) → Lexer → Tokens → Parser → AST → TypeChecker → Interpreter (tree-walk)
                                                              → Compiler → Bytecode → VM
                                                              → HIR → MIR → JIT/Codegen
```

## Key Conventions

- Source language is C++17, comments are in Chinese (中文)
- AST nodes use `std::unique_ptr<ASTNode>` (aliased as `NodePtr`)
- The interpreter maintains a `std::map<std::string, Value>` environment
- Modules have isolated environments with optional persistent state
- Hot reload re-runs the full file while preserving persistent state
- `func` is the function keyword (`fn` was removed)
- String interpolation uses `\(expr)` syntax (Swift-style)

## Flux Language Quick Reference

- Variables: `var x = 42`, `var name: String = "Flux"`, `!var forced = 0`
- Functions: `func foo(a, b) { ... }`
- Modules: `module Name { persistent { key: val } func method() { ... } }`
- Interfaces: `var Shape: interface = { func area() }`
- Structs: `var Circle = Shape { radius: 1, func area() { ... } }`
- Concurrency: `@threadpool(name: "pool", size: 4)` + `@concurrent(pool: "pool")`
- Async: `var f = obj.method.async(args)`, `f.await()`
- Supervision: `@supervised(restart: .always, maxRetries: 3)`
- Exception: `exception "message"`
