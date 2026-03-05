// codegen.h — Flux 多目标代码生成后端
// MIR → ARM64 / RISC-V / x86-64 汇编输出
#pragma once
#include "mir.h"
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <stdexcept>

// ═══════════════════════════════════════════════════════════
// 目标架构
// ═══════════════════════════════════════════════════════════
enum class TargetArch {
    X86_64,
    ARM64,
    RISCV64,
};

inline std::string archName(TargetArch arch) {
    switch (arch) {
    case TargetArch::X86_64:  return "x86_64";
    case TargetArch::ARM64:   return "arm64";
    case TargetArch::RISCV64: return "riscv64";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════
// 代码生成结果
// ═══════════════════════════════════════════════════════════
struct CodegenResult {
    bool ok = false;
    std::string error;
    std::string assembly;      // 目标汇编文本
    TargetArch  arch;
};

// ═══════════════════════════════════════════════════════════
// 代码生成器基类
// ═══════════════════════════════════════════════════════════
class CodeGenerator {
public:
    virtual ~CodeGenerator() = default;
    virtual TargetArch target() const = 0;
    virtual CodegenResult generate(const MIRProgram& prog) = 0;

protected:
    std::ostringstream out_;

    void emitLine(const std::string& line) { out_ << line << "\n"; }
    void emitLabel(const std::string& label) { out_ << label << ":\n"; }
    void emitComment(const std::string& comment) { out_ << "    // " << comment << "\n"; }
    void emitInst(const std::string& inst) { out_ << "    " << inst << "\n"; }
};

// ═══════════════════════════════════════════════════════════
// ARM64 代码生成器 (AArch64)
// ═══════════════════════════════════════════════════════════
class ARM64CodeGen : public CodeGenerator {
public:
    TargetArch target() const override { return TargetArch::ARM64; }

    CodegenResult generate(const MIRProgram& prog) override {
        CodegenResult result;
        result.arch = TargetArch::ARM64;
        out_.str("");

        emitLine(".text");
        emitLine(".global _start");
        emitLine("");

        for (auto& fn : prog.functions) {
            generateFunction(fn);
        }

        // Entry point
        emitLine("_start:");
        emitInst("bl __main__");
        emitInst("mov x8, #93");       // exit syscall
        emitInst("mov x0, #0");
        emitInst("svc #0");

        result.assembly = out_.str();
        result.ok = true;
        return result;
    }

private:
    int nextReg_ = 0;
    std::unordered_map<int, int> regMap_;  // SSA value → register

    int allocReg(int valueId) {
        auto it = regMap_.find(valueId);
        if (it != regMap_.end()) return it->second;
        int r = nextReg_++ % 28;  // x0-x27 (avoid x28-x30/sp)
        regMap_[valueId] = r;
        return r;
    }

    std::string xReg(int r) { return "x" + std::to_string(r); }
    std::string dReg(int r) { return "d" + std::to_string(r % 32); }

    void generateFunction(const MIRFunction& fn) {
        emitLine(".global " + fn.name);
        emitLabel(fn.name);

        // Function prologue
        emitInst("stp x29, x30, [sp, #-16]!");
        emitInst("mov x29, sp");

        regMap_.clear();
        nextReg_ = 0;

        // Map parameters to registers
        for (int i = 0; i < (int)fn.params.size() && i < 8; i++) {
            // 参数在 d0-d7（浮点）
            regMap_[i] = i;
        }

        for (auto& bb : fn.blocks) {
            emitLabel(".L" + fn.name + "_bb" + std::to_string(bb.id));
            for (auto& inst : bb.instructions) {
                generateInst(inst, fn);
            }
        }

        emitLine("");
    }

    void generateInst(const MIRInst& inst, const MIRFunction& fn) {
        switch (inst.op) {
        case MIROp::Const: {
            if (inst.dest < 0) break;
            int r = allocReg(inst.dest);
            // Load double constant via literal pool
            emitComment("const " + std::to_string(inst.constNum));
            emitInst("ldr " + dReg(r) + ", =" + std::to_string((long long)inst.constNum));
            break;
        }
        case MIROp::Add: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fadd " + dReg(d) + ", " + dReg(l) + ", " + dReg(r));
            break;
        }
        case MIROp::Sub: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fsub " + dReg(d) + ", " + dReg(l) + ", " + dReg(r));
            break;
        }
        case MIROp::Mul: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fmul " + dReg(d) + ", " + dReg(l) + ", " + dReg(r));
            break;
        }
        case MIROp::Div: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fdiv " + dReg(d) + ", " + dReg(l) + ", " + dReg(r));
            break;
        }
        case MIROp::Load: {
            if (inst.dest < 0) break;
            int r = allocReg(inst.dest);
            // 查找参数
            for (int i = 0; i < (int)fn.params.size(); i++) {
                if (fn.params[i].first == inst.name) {
                    if (r != i) emitInst("fmov " + dReg(r) + ", " + dReg(i));
                    break;
                }
            }
            break;
        }
        case MIROp::Branch: {
            if (inst.operands.empty()) break;
            int cond = allocReg(inst.operands[0]);
            emitInst("fcmp " + dReg(cond) + ", #0.0");
            emitInst("b.eq .L_bb" + std::to_string(inst.falseBlock));
            emitInst("b .L_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Jump: {
            emitInst("b .L_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Return: {
            if (!inst.operands.empty()) {
                int r = allocReg(inst.operands[0]);
                if (r != 0) emitInst("fmov d0, " + dReg(r));
            }
            emitInst("ldp x29, x30, [sp], #16");
            emitInst("ret");
            break;
        }
        default:
            emitComment("unsupported MIR op: " + std::to_string((int)inst.op));
            break;
        }
    }
};

// ═══════════════════════════════════════════════════════════
// RISC-V 64 代码生成器
// ═══════════════════════════════════════════════════════════
class RISCV64CodeGen : public CodeGenerator {
public:
    TargetArch target() const override { return TargetArch::RISCV64; }

    CodegenResult generate(const MIRProgram& prog) override {
        CodegenResult result;
        result.arch = TargetArch::RISCV64;
        out_.str("");

        emitLine(".text");
        emitLine(".global _start");
        emitLine("");

        for (auto& fn : prog.functions) {
            generateFunction(fn);
        }

        // Entry point
        emitLine("_start:");
        emitInst("call __main__");
        emitInst("li a7, 93");        // exit ecall
        emitInst("li a0, 0");
        emitInst("ecall");

        result.assembly = out_.str();
        result.ok = true;
        return result;
    }

private:
    int nextReg_ = 0;
    std::unordered_map<int, int> regMap_;

    int allocReg(int valueId) {
        auto it = regMap_.find(valueId);
        if (it != regMap_.end()) return it->second;
        int r = nextReg_++ % 32;
        regMap_[valueId] = r;
        return r;
    }

    std::string fReg(int r) { return "ft" + std::to_string(r % 12); }
    std::string faReg(int r) { return "fa" + std::to_string(r % 8); }

    void generateFunction(const MIRFunction& fn) {
        emitLine(".global " + fn.name);
        emitLabel(fn.name);

        // Function prologue
        emitInst("addi sp, sp, -16");
        emitInst("sd ra, 8(sp)");
        emitInst("sd s0, 0(sp)");
        emitInst("addi s0, sp, 16");

        regMap_.clear();
        nextReg_ = 0;

        for (auto& bb : fn.blocks) {
            emitLabel(".L" + fn.name + "_bb" + std::to_string(bb.id));
            for (auto& inst : bb.instructions) {
                generateInst(inst, fn);
            }
        }

        emitLine("");
    }

    void generateInst(const MIRInst& inst, const MIRFunction& fn) {
        switch (inst.op) {
        case MIROp::Const: {
            if (inst.dest < 0) break;
            int r = allocReg(inst.dest);
            emitComment("const " + std::to_string(inst.constNum));
            // Load double via memory (simplified)
            emitInst("li t0, " + std::to_string((long long)inst.constNum));
            emitInst("fcvt.d.l " + fReg(r) + ", t0");
            break;
        }
        case MIROp::Add: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fadd.d " + fReg(d) + ", " + fReg(l) + ", " + fReg(r));
            break;
        }
        case MIROp::Sub: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fsub.d " + fReg(d) + ", " + fReg(l) + ", " + fReg(r));
            break;
        }
        case MIROp::Mul: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fmul.d " + fReg(d) + ", " + fReg(l) + ", " + fReg(r));
            break;
        }
        case MIROp::Div: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fdiv.d " + fReg(d) + ", " + fReg(l) + ", " + fReg(r));
            break;
        }
        case MIROp::Load: {
            if (inst.dest < 0) break;
            int r = allocReg(inst.dest);
            for (int i = 0; i < (int)fn.params.size(); i++) {
                if (fn.params[i].first == inst.name) {
                    if (r != i) emitInst("fmv.d " + fReg(r) + ", " + faReg(i));
                    break;
                }
            }
            break;
        }
        case MIROp::Branch: {
            if (inst.operands.empty()) break;
            int cond = allocReg(inst.operands[0]);
            emitInst("feq.d t0, " + fReg(cond) + ", " + fReg(cond));
            emitInst("beqz t0, .L_bb" + std::to_string(inst.falseBlock));
            emitInst("j .L_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Jump: {
            emitInst("j .L_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Return: {
            if (!inst.operands.empty()) {
                int r = allocReg(inst.operands[0]);
                emitInst("fmv.d fa0, " + fReg(r));
            }
            emitInst("ld ra, 8(sp)");
            emitInst("ld s0, 0(sp)");
            emitInst("addi sp, sp, 16");
            emitInst("ret");
            break;
        }
        default:
            emitComment("unsupported MIR op: " + std::to_string((int)inst.op));
            break;
        }
    }
};

// ═══════════════════════════════════════════════════════════
// 工厂函数
// ═══════════════════════════════════════════════════════════
inline std::unique_ptr<CodeGenerator> createCodeGenerator(TargetArch arch) {
    switch (arch) {
    case TargetArch::ARM64:   return std::make_unique<ARM64CodeGen>();
    case TargetArch::RISCV64: return std::make_unique<RISCV64CodeGen>();
    default:
        throw std::runtime_error("codegen: unsupported target architecture: " + archName(arch));
    }
}
