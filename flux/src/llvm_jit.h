// llvm_jit.h — Flux LLVM JIT Compiler (public interface)
// HIR -> LLVM IR -> native code via ORC JIT
// Uses PIMPL to isolate LLVM headers from the rest of the codebase.
#pragma once

#include "hir.h"
#include "interpreter.h"
#include <string>
#include <memory>
#include <iostream>

// =====================================================================
// LLVMJITCompiler — HIR -> LLVM IR -> native execution
// Uses PIMPL pattern: LLVM headers are only included in llvm_jit.cpp,
// so the rest of the project never sees LLVM includes.
// =====================================================================
class LLVMJITCompiler {
public:
    LLVMJITCompiler();
    ~LLVMJITCompiler();

    // Non-copyable, moveable
    LLVMJITCompiler(const LLVMJITCompiler&) = delete;
    LLVMJITCompiler& operator=(const LLVMJITCompiler&) = delete;
    LLVMJITCompiler(LLVMJITCompiler&&) noexcept;
    LLVMJITCompiler& operator=(LLVMJITCompiler&&) noexcept;

    // Set the interpreter for runtime bridge calls
    void setInterpreter(Interpreter* interp);

    // Compile and execute an entire HIR program
    // Returns the exit value (0.0 on success, -1.0 if LLVM not available)
    double compileAndRun(const HIRProgram& program);

    // Recompile: remove old module, compile new HIR, re-execute __flux_main__
    // Used for hot reload — preserves JIT engine and persistent state
    double recompile(const HIRProgram& program);

    // Compile a single HIR function, return true on success
    bool compileFunction(HIRFnDecl* fn);

    // Print LLVM IR for debugging
    void dumpIR() const;

    // Is LLVM actually available at runtime?
    bool isAvailable() const;

    // Statistics
    struct Stats {
        int functionsCompiled = 0;
        int totalIRInstructions = 0;
        double compilationTimeMs = 0;
        double executionTimeMs = 0;
    };
    Stats getStats() const;

    void dumpStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
