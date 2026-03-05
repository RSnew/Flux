# Code Understanding Skill

## Skill Description

Help AI deeply understand the Flux language codebase — its architecture, data flow, module responsibilities, and implementation patterns.

## When to Use

- When the user asks to understand, explain, or analyze any part of the Flux codebase
- When the user asks "how does X work" about any Flux component
- When the user needs to trace data flow or execution paths
- When the user wants to understand relationships between modules
- When the user asks about the Flux language syntax/semantics (refer to source code as ground truth)

## Instructions

### Step 1: Identify the Scope

Determine what the user wants to understand:
- **Single file/component**: Read the specific file and explain its structure
- **Cross-cutting concern**: Identify all relevant files and trace the flow
- **Architecture overview**: Provide a high-level map then drill into specifics
- **Specific feature**: Trace from entry point through the full pipeline

### Step 2: Read Source Code (Always)

**Never guess or assume — always read the actual source files.**

Use this map to locate components:

| Topic | Primary Files |
|-------|--------------|
| Tokenization / Lexing | `flux/src/token.h`, `flux/src/lexer.h`, `flux/src/lexer.cpp` |
| Parsing / AST | `flux/src/parser.h`, `flux/src/parser.cpp`, `flux/src/ast.h` |
| Type system | `flux/src/typechecker.h`, `flux/src/typechecker.cpp` |
| Interpreter / Runtime | `flux/src/interpreter.h`, `flux/src/interpreter.cpp` |
| Bytecode compilation | `flux/src/compiler.h`, `flux/src/compiler.cpp` |
| Virtual machine | `flux/src/vm.h`, `flux/src/vm.cpp` |
| Standard library | `flux/src/stdlib.cpp` |
| Concurrency | `flux/src/concurrency.h`, `flux/src/threadpool.h` |
| Hot reload | `flux/src/watcher.h`, `flux/src/watcher.cpp` |
| Formatter | `flux/src/formatter.h` |
| Package manager | `flux/src/pkgmgr.h`, `flux/src/pkgmgr.cpp`, `flux/src/toml.h` |
| CLI entry point | `flux/src/main.cpp` |
| Optimization pipeline | `flux/src/hir.h`, `flux/src/mir.h`, `flux/src/jit.h`, `flux/src/codegen.h` |
| Debugging / Profiling | `flux/src/debugger.h`, `flux/src/profiler.h` |
| GC / Memory | `flux/src/gc.h` |
| Binary protection | `flux/src/fluz.h` |
| LSP support | `flux/src/lsp.h` |
| Example programs | `flux/examples/*.flux` |
| VSCode extension | `vscode-flux/` |

### Step 3: Analyze and Explain

When explaining code, follow this structure:

1. **Purpose**: What does this component do and why does it exist?
2. **Key Data Structures**: The main types/structs/classes and their roles
3. **Core Algorithm / Flow**: Step-by-step walkthrough of the main logic
4. **Integration Points**: How it connects to other components (inputs/outputs)
5. **Design Decisions**: Notable patterns, trade-offs, or clever techniques

### Step 4: Provide Examples (When Helpful)

- Reference specific line numbers: `file.cpp:42`
- Show relevant Flux code snippets from `flux/examples/` that exercise the feature
- Trace a concrete example through the pipeline when explaining flow

## Architecture Reference

```
┌─────────────────────────────────────────────────────────┐
│                    main.cpp (CLI)                        │
│  run / watch / repl / check / fmt / vm / pkg commands   │
└─────────┬───────────────────────────────────────────────┘
          │
          ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│   Lexer      │───▶│   Parser     │───▶│     AST      │
│  lexer.h/cpp │    │ parser.h/cpp │    │    ast.h     │
│  token.h     │    │              │    │ (33 nodes)   │
└──────────────┘    └──────────────┘    └──────┬───────┘
                                               │
                    ┌──────────────────────────┬┴──────────────┐
                    ▼                          ▼               ▼
          ┌──────────────┐          ┌──────────────┐  ┌──────────────┐
          │ TypeChecker  │          │ Interpreter  │  │  Compiler    │
          │typechecker.* │          │interpreter.* │  │ compiler.*   │
          └──────────────┘          │ + stdlib.cpp │  └──────┬───────┘
                                    │ + concurrency│         │
                                    │ + threadpool │         ▼
                                    └──────────────┘  ┌──────────────┐
                                                      │     VM       │
                                                      │  vm.h/cpp    │
                                                      │ (28 opcodes) │
                                                      └──────────────┘
```

## Notes

- Source comments are in Chinese (中文) — this is intentional
- The project uses `std::unique_ptr<ASTNode>` (`NodePtr`) for AST ownership
- Hot reload preserves `persistent` state while re-executing the file
- The interpreter and VM are two independent execution backends
