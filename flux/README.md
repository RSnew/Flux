# Flux Language 🔥

A hot-reload-first systems language. Save your file, changes take effect instantly — no restart needed.

## Core Philosophy

- **Hot Reload by default** — file change → re-parse → re-execute, state preserved
- **Persistent state** — `persistent { }` variables survive hot reloads
- **Module system** — isolated state spaces per module
- **`migrate` safety** — schema changes require explicit migration blocks
- **`@supervised` crash isolation** — one module panics, others keep running
- **Gradual type system** — unannotated code runs freely; annotations are strictly checked at compile time

## Quick Start

```bash
g++ -std=c++17 -O2 -pthread \
  src/main.cpp src/lexer.cpp src/parser.cpp \
  src/typechecker.cpp src/interpreter.cpp src/watcher.cpp \
  -o flux

./flux examples/module_demo.flux
```

## Syntax Overview

```swift
// Persistent state (survives hot reload)
persistent {
    visits: 0
}
state.visits = state.visits + 1

// Modules with isolated state
module Counter {
    persistent { count: 0 }

    fn increment() {
        state.count = state.count + 1
    }

    fn getValue() -> Int {
        return state.count
    }
}

Counter.increment()
print(Counter.getValue())

// Migration (required when adding persistent fields)
module Analytics {
    persistent {
        pageViews: 0,
        errors: 0       // new field
    }
    migrate {
        errors: 0       // must cover new fields
    }
}

// Crash isolation
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    fn charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}

// Type system
fn add(a: Int, b: Int) -> Int {
    return a + b
}

let x: Int = 42
let name = "Flux"   // inferred as String
```

## Features

| Feature | Status |
|---------|--------|
| Hot Reload (inotify) | ✅ |
| `persistent` state | ✅ |
| Module system | ✅ |
| `migrate` schema safety | ✅ |
| `@supervised` crash isolation | ✅ |
| Type system (inference + annotations) | ✅ |
| Arrays / for-in / string interpolation | 🔧 In progress |

## Project Structure

```
src/
  token.h          — Token types
  lexer.h/.cpp     — Lexer (with string interpolation framework)
  ast.h            — AST node definitions
  parser.h/.cpp    — Recursive descent parser
  typechecker.h/.cpp — Type inference + annotation checking
  interpreter.h/.cpp — Tree-walking interpreter
  watcher.h/.cpp   — inotify file watcher
  main.cpp         — Pipeline: Lex → Parse → TypeCheck → Execute

examples/
  hello.flux
  persistent_demo.flux
  module_demo.flux
  migrate_demo.flux
  supervisor_demo.flux
  types_demo.flux
```

## Lines of Code

~2,800 lines of C++17.
