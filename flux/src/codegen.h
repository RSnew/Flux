// codegen.h — Flux 多目标代码生成后端
// MIR → ARM64 / RISC-V / x86-64 汇编输出
#pragma once
#include "mir.h"
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <cstring>

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
    std::unordered_map<std::string, int> varOffsets_; // 局部变量 → 栈偏移
    int nextVarOffset_ = 16; // 从 sp+16 开始（跳过保存的 x29/x30）

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

        // Function prologue — 预留 256 字节栈空间给局部变量
        emitInst("stp x29, x30, [sp, #-16]!");
        emitInst("mov x29, sp");
        emitInst("sub sp, sp, #256");

        regMap_.clear();
        nextReg_ = 0;
        varOffsets_.clear();
        nextVarOffset_ = 16;

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
            emitComment("const " + std::to_string(inst.constNum));
            if (inst.constNum == 0.0) {
                emitInst("movi " + dReg(r) + ", #0");
            } else if (inst.constNum == 1.0) {
                emitInst("fmov " + dReg(r) + ", #1.0");
            } else if (inst.constNum == 2.0) {
                emitInst("fmov " + dReg(r) + ", #2.0");
            } else {
                // 用整数寄存器加载 64 位立即数，再转移到浮点寄存器
                long long bits;
                double val = inst.constNum;
                std::memcpy(&bits, &val, 8);
                emitInst("mov x9, #" + std::to_string(bits & 0xFFFF));
                if ((bits >> 16) & 0xFFFF)
                    emitInst("movk x9, #" + std::to_string((bits >> 16) & 0xFFFF) + ", lsl #16");
                if ((bits >> 32) & 0xFFFF)
                    emitInst("movk x9, #" + std::to_string((bits >> 32) & 0xFFFF) + ", lsl #32");
                if ((bits >> 48) & 0xFFFF)
                    emitInst("movk x9, #" + std::to_string((bits >> 48) & 0xFFFF) + ", lsl #48");
                emitInst("fmov " + dReg(r) + ", x9");
            }
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
            bool found = false;
            for (int i = 0; i < (int)fn.params.size(); i++) {
                if (fn.params[i].first == inst.name) {
                    if (r != i) emitInst("fmov " + dReg(r) + ", " + dReg(i));
                    found = true;
                    break;
                }
            }
            // 查找局部变量（从栈加载）
            if (!found && varOffsets_.count(inst.name)) {
                int off = varOffsets_[inst.name];
                emitComment("load var " + inst.name);
                emitInst("ldr " + dReg(r) + ", [sp, #" + std::to_string(off) + "]");
            }
            break;
        }
        case MIROp::Store: {
            // store %val → <name>：将值存入栈上局部变量
            if (inst.operands.empty()) break;
            int src = allocReg(inst.operands[0]);
            if (!varOffsets_.count(inst.name)) {
                varOffsets_[inst.name] = nextVarOffset_;
                nextVarOffset_ += 8;
            }
            int off = varOffsets_[inst.name];
            emitComment("store var " + inst.name);
            emitInst("str " + dReg(src) + ", [sp, #" + std::to_string(off) + "]");
            break;
        }
        case MIROp::Call: {
            // %r = call <name>(%args...)：调用函数
            emitComment("call " + inst.name);
            // 将参数放入 d0..d7（ARM64 浮点调用约定）
            for (int i = 0; i < (int)inst.operands.size() && i < 8; i++) {
                int src = allocReg(inst.operands[i]);
                if (src != i) emitInst("fmov " + dReg(i) + ", " + dReg(src));
            }
            emitInst("bl " + inst.name);
            // 返回值在 d0
            if (inst.dest >= 0) {
                int d = allocReg(inst.dest);
                if (d != 0) emitInst("fmov " + dReg(d) + ", d0");
            }
            break;
        }
        case MIROp::Mod: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            // ARM64: a % b = a - (a/b)*b
            emitComment("mod");
            emitInst("fdiv " + dReg(d) + ", " + dReg(l) + ", " + dReg(r));
            emitInst("frintm " + dReg(d) + ", " + dReg(d));  // floor
            emitInst("fmul " + dReg(d) + ", " + dReg(d) + ", " + dReg(r));
            emitInst("fsub " + dReg(d) + ", " + dReg(l) + ", " + dReg(d));
            break;
        }
        case MIROp::Eq: case MIROp::Ne: case MIROp::Lt:
        case MIROp::Le: case MIROp::Gt: case MIROp::Ge: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int r = allocReg(inst.operands[1]);
            emitInst("fcmp " + dReg(l) + ", " + dReg(r));
            std::string cond;
            switch (inst.op) {
            case MIROp::Eq: cond = "eq"; break;
            case MIROp::Ne: cond = "ne"; break;
            case MIROp::Lt: cond = "mi"; break;
            case MIROp::Le: cond = "ls"; break;
            case MIROp::Gt: cond = "gt"; break;
            case MIROp::Ge: cond = "ge"; break;
            default: cond = "eq"; break;
            }
            // 将比较结果转换为 1.0 / 0.0
            emitInst("fmov " + dReg(d) + ", #1.0");
            emitInst("b." + cond + " .Lcmp_" + std::to_string(inst.dest));
            emitInst("fmov " + dReg(d) + ", xzr");
            emitLine(".Lcmp_" + std::to_string(inst.dest) + ":");
            break;
        }
        case MIROp::Negate: {
            if (inst.operands.empty() || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int s = allocReg(inst.operands[0]);
            emitInst("fneg " + dReg(d) + ", " + dReg(s));
            break;
        }
        case MIROp::Not: {
            if (inst.operands.empty() || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int s = allocReg(inst.operands[0]);
            // !x: if x==0 then 1.0 else 0.0
            emitInst("fcmp " + dReg(s) + ", #0.0");
            emitInst("fmov " + dReg(d) + ", #1.0");
            emitInst("b.eq .Lnot_" + std::to_string(inst.dest));
            emitInst("fmov " + dReg(d) + ", xzr");
            emitLine(".Lnot_" + std::to_string(inst.dest) + ":");
            break;
        }
        case MIROp::Branch: {
            if (inst.operands.empty()) break;
            int cond = allocReg(inst.operands[0]);
            emitInst("fcmp " + dReg(cond) + ", #0.0");
            emitInst("b.eq .L" + fn.name + "_bb" + std::to_string(inst.falseBlock));
            emitInst("b .L" + fn.name + "_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Jump: {
            emitInst("b .L" + fn.name + "_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Return: {
            if (!inst.operands.empty()) {
                int r = allocReg(inst.operands[0]);
                if (r != 0) emitInst("fmov d0, " + dReg(r));
            }
            emitInst("add sp, sp, #256");
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
// x86-64 代码生成器 (System V ABI)
// ═══════════════════════════════════════════════════════════
class X86_64CodeGen : public CodeGenerator {
public:
    bool sharedLib = false;  // true → 生成 .so（无 _start，导出所有函数）

    TargetArch target() const override { return TargetArch::X86_64; }

    CodegenResult generate(const MIRProgram& prog) override {
        CodegenResult result;
        result.arch = TargetArch::X86_64;
        out_.str("");

        emitLine(".text");
        if (!sharedLib) {
            emitLine(".global _start");
        }
        emitLine("");

        for (auto& fn : prog.functions) {
            generateFunction(fn);
        }

        if (!sharedLib) {
            // Entry point: call __main__ then exit(0)
            emitLine("_start:");
            emitInst("call __main__");
            emitInst("movq $60, %rax");      // exit syscall
            emitInst("xorq %rdi, %rdi");     // status = 0
            emitInst("syscall");
        }

        result.assembly = out_.str();
        result.ok = true;
        return result;
    }

private:
    int nextReg_ = 0;
    std::unordered_map<int, int> regMap_;  // SSA value → xmm register slot

    int allocReg(int valueId) {
        auto it = regMap_.find(valueId);
        if (it != regMap_.end()) return it->second;
        int r = nextReg_++ % 16;
        regMap_[valueId] = r;
        return r;
    }

    std::string xmm(int r) { return "%xmm" + std::to_string(r % 16); }

    void generateFunction(const MIRFunction& fn) {
        emitLine(".global " + fn.name);
        emitLabel(fn.name);

        // Function prologue (System V ABI)
        emitInst("pushq %rbp");
        emitInst("movq %rsp, %rbp");

        regMap_.clear();
        nextReg_ = 0;

        // Parameters arrive in xmm0-xmm7 (System V floating-point ABI)
        for (int i = 0; i < (int)fn.params.size() && i < 8; i++) {
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
            // Load double constant via integer register then move to xmm
            union { double d; uint64_t u; } conv;
            conv.d = inst.constNum;
            emitComment("const " + std::to_string(inst.constNum));
            emitInst("movabsq $" + std::to_string(conv.u) + ", %rax");
            emitInst("movq %rax, " + xmm(r));
            break;
        }
        case MIROp::Add: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            emitInst("addsd " + xmm(rv) + ", " + xmm(d));
            break;
        }
        case MIROp::Sub: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            emitInst("subsd " + xmm(rv) + ", " + xmm(d));
            break;
        }
        case MIROp::Mul: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            emitInst("mulsd " + xmm(rv) + ", " + xmm(d));
            break;
        }
        case MIROp::Div: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            emitInst("divsd " + xmm(rv) + ", " + xmm(d));
            break;
        }
        case MIROp::Mod: {
            // x86-64 没有浮点求余指令，用 a - floor(a/b)*b
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            emitComment("mod via a - floor(a/b)*b");
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            emitInst("divsd " + xmm(rv) + ", " + xmm(d));
            emitInst("roundsd $1, " + xmm(d) + ", " + xmm(d));  // floor
            emitInst("mulsd " + xmm(rv) + ", " + xmm(d));
            // result = l - d
            emitInst("movsd " + xmm(l) + ", %xmm15");
            emitInst("subsd " + xmm(d) + ", %xmm15");
            emitInst("movsd %xmm15, " + xmm(d));
            break;
        }
        case MIROp::Eq: case MIROp::Ne:
        case MIROp::Lt: case MIROp::Le:
        case MIROp::Gt: case MIROp::Ge: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            emitInst("ucomisd " + xmm(rv) + ", " + xmm(l));
            std::string setcc;
            switch (inst.op) {
                case MIROp::Eq: setcc = "sete"; break;
                case MIROp::Ne: setcc = "setne"; break;
                case MIROp::Lt: setcc = "setb"; break;
                case MIROp::Le: setcc = "setbe"; break;
                case MIROp::Gt: setcc = "seta"; break;
                case MIROp::Ge: setcc = "setae"; break;
                default: setcc = "sete"; break;
            }
            emitInst(setcc + " %al");
            emitInst("movzbq %al, %rax");
            emitInst("cvtsi2sd %rax, " + xmm(d));
            break;
        }
        case MIROp::Negate: {
            if (inst.operands.empty() || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int s = allocReg(inst.operands[0]);
            // Negate by XOR with sign bit
            emitInst("xorpd " + xmm(d) + ", " + xmm(d));
            emitInst("subsd " + xmm(s) + ", " + xmm(d));
            break;
        }
        case MIROp::Not: {
            if (inst.operands.empty() || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int s = allocReg(inst.operands[0]);
            // not: 0.0 → 1.0, nonzero → 0.0
            emitInst("xorpd %xmm15, %xmm15");
            emitInst("ucomisd %xmm15, " + xmm(s));
            emitInst("sete %al");
            emitInst("movzbq %al, %rax");
            emitInst("cvtsi2sd %rax, " + xmm(d));
            break;
        }
        case MIROp::And: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            emitInst("andpd " + xmm(rv) + ", " + xmm(l));
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            break;
        }
        case MIROp::Or: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int d = allocReg(inst.dest);
            int l = allocReg(inst.operands[0]);
            int rv = allocReg(inst.operands[1]);
            emitInst("orpd " + xmm(rv) + ", " + xmm(l));
            if (d != l) emitInst("movsd " + xmm(l) + ", " + xmm(d));
            break;
        }
        case MIROp::Load: {
            if (inst.dest < 0) break;
            int r = allocReg(inst.dest);
            for (int i = 0; i < (int)fn.params.size(); i++) {
                if (fn.params[i].first == inst.name) {
                    if (r != i) emitInst("movsd " + xmm(i) + ", " + xmm(r));
                    break;
                }
            }
            break;
        }
        case MIROp::Store: {
            emitComment("store " + inst.name);
            break;
        }
        case MIROp::Call: {
            if (inst.dest < 0) break;
            int d = allocReg(inst.dest);
            // Place arguments in xmm0-xmm7
            for (int i = 0; i < (int)inst.operands.size() && i < 8; i++) {
                int src = allocReg(inst.operands[i]);
                if (src != i) emitInst("movsd " + xmm(src) + ", " + xmm(i));
            }
            emitInst("call " + inst.name);
            if (d != 0) emitInst("movsd %xmm0, " + xmm(d));
            break;
        }
        case MIROp::Branch: {
            if (inst.operands.empty()) break;
            int cond = allocReg(inst.operands[0]);
            emitInst("xorpd %xmm15, %xmm15");
            emitInst("ucomisd %xmm15, " + xmm(cond));
            emitInst("je .L" + fn.name + "_bb" + std::to_string(inst.falseBlock));
            emitInst("jmp .L" + fn.name + "_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Jump: {
            emitInst("jmp .L" + fn.name + "_bb" + std::to_string(inst.targetBlock));
            break;
        }
        case MIROp::Return: {
            if (!inst.operands.empty()) {
                int r = allocReg(inst.operands[0]);
                if (r != 0) emitInst("movsd " + xmm(r) + ", %xmm0");
            }
            emitInst("popq %rbp");
            emitInst("ret");
            break;
        }
        case MIROp::DerefLoad: {
            // deref_load %addr, %offset → read 1 byte at addr+offset
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int addr = allocReg(inst.operands[0]);
            int off  = allocReg(inst.operands[1]);
            int d    = allocReg(inst.dest);
            // Convert addr from double (xmm) to integer in rax
            emitInst("cvttsd2si " + xmm(addr) + ", %rax");
            // Convert offset from double to integer in rcx
            emitInst("cvttsd2si " + xmm(off) + ", %rcx");
            emitInst("addq %rcx, %rax");
            // Load 1 byte, zero-extend to rax
            emitInst("movzbq (%rax), %rax");
            // Convert back to double in dest xmm
            emitInst("cvtsi2sd %rax, " + xmm(d));
            break;
        }
        case MIROp::DerefStore: {
            // deref_store %addr, %offset, %val → write 1 byte
            if (inst.operands.size() < 3) break;
            int addr = allocReg(inst.operands[0]);
            int off  = allocReg(inst.operands[1]);
            int val  = allocReg(inst.operands[2]);
            // Convert addr to integer
            emitInst("cvttsd2si " + xmm(addr) + ", %rax");
            // Convert offset to integer
            emitInst("cvttsd2si " + xmm(off) + ", %rcx");
            emitInst("addq %rcx, %rax");
            // Convert value to integer byte
            emitInst("cvttsd2si " + xmm(val) + ", %rdx");
            emitInst("movb %dl, (%rax)");
            break;
        }
        case MIROp::AsmBlock: {
            // Emit raw assembly text from constStr
            // Each line separated by newline
            emitComment("inline asm block");
            std::istringstream ss(inst.constStr);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty()) emitInst(line);
            }
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
    case TargetArch::X86_64:  return std::make_unique<X86_64CodeGen>();
    case TargetArch::ARM64:   return std::make_unique<ARM64CodeGen>();
    case TargetArch::RISCV64: return std::make_unique<RISCV64CodeGen>();
    }
    throw std::runtime_error("codegen: unsupported target architecture: " + archName(arch));
}

// ═══════════════════════════════════════════════════════════
// 编译到二进制 — 生成汇编 → 汇编器 → 链接器 → 可执行文件
// ═══════════════════════════════════════════════════════════
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>

struct CompileOptions {
    TargetArch arch = TargetArch::X86_64;
    std::string output = "a.out";
    bool keepAsm = false;       // 保留 .s 文件
    bool verbose = false;
    bool sharedLib = false;     // 生成 .so 而非可执行文件
};

inline TargetArch detectHostArch() {
#if defined(__x86_64__) || defined(_M_X64)
    return TargetArch::X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return TargetArch::ARM64;
#elif defined(__riscv) && __riscv_xlen == 64
    return TargetArch::RISCV64;
#else
    return TargetArch::X86_64;
#endif
}

inline bool compileToBinary(const MIRProgram& mir, const CompileOptions& opts) {
    // 1) 代码生成 → 汇编
    auto gen = createCodeGenerator(opts.arch);
    // 设置 shared lib 模式
    if (opts.sharedLib) {
        if (auto* x86 = dynamic_cast<X86_64CodeGen*>(gen.get()))
            x86->sharedLib = true;
    }
    auto result = gen->generate(mir);
    if (!result.ok) {
        std::cerr << "Codegen error: " << result.error << "\n";
        return false;
    }

    // 2) 写入临时 .s 文件
    std::string asmFile = opts.output + ".s";
    {
        std::ofstream f(asmFile);
        if (!f) {
            std::cerr << "Cannot write assembly file: " << asmFile << "\n";
            return false;
        }
        f << result.assembly;
    }

    if (opts.verbose) {
        std::cerr << "[compile] Generated " << archName(opts.arch) << " assembly → " << asmFile << "\n";
    }

    // 3) 选择汇编器和链接器命令（根据宿主平台）
    std::string assembler, linker;
#ifdef __APPLE__
    // macOS: 使用原生工具链（Xcode / CommandLineTools）
    switch (opts.arch) {
    case TargetArch::X86_64:
        assembler = "as -arch x86_64";
        linker = "clang -arch x86_64";
        break;
    case TargetArch::ARM64:
        assembler = "as -arch arm64";
        linker = "clang -arch arm64";
        break;
    case TargetArch::RISCV64:
        assembler = "riscv64-linux-gnu-as";  // macOS 无原生 RISC-V 支持
        linker = "riscv64-linux-gnu-gcc";
        break;
    }
#else
    // Linux: 原生或交叉编译工具链
    switch (opts.arch) {
    case TargetArch::X86_64:
        assembler = "as";
        linker = "gcc";
        break;
    case TargetArch::ARM64:
        assembler = "aarch64-linux-gnu-as";
        linker = "aarch64-linux-gnu-gcc";
        break;
    case TargetArch::RISCV64:
        assembler = "riscv64-linux-gnu-as";
        linker = "riscv64-linux-gnu-gcc";
        break;
    }
#endif

    // 4) 汇编: .s → .o（.so 模式需要 -fPIC）
    std::string objFile = opts.output + ".o";
    std::string asmCmd = assembler + " -o " + objFile + " " + asmFile + " 2>&1";
    if (opts.verbose) {
        std::cerr << "[compile] " << asmCmd << "\n";
    }
    int rc = std::system(asmCmd.c_str());
    if (rc != 0) {
        std::cerr << "Assembly failed (exit " << rc << ")\n";
        return false;
    }

    // 5) 链接: .o → binary 或 .so
    std::string linkCmd;
    if (opts.sharedLib) {
        // 生成共享库
        linkCmd = linker + " -shared -nostdlib -o " + opts.output + " " + objFile + " 2>&1";
    } else {
        // 生成可执行文件
        std::string ld;
#ifdef __APPLE__
        // macOS: 使用 clang 驱动链接
        ld = linker;
        linkCmd = ld + " -o " + opts.output + " " + objFile + " -lSystem 2>&1";
#else
        switch (opts.arch) {
        case TargetArch::X86_64:  ld = "ld"; break;
        case TargetArch::ARM64:   ld = "aarch64-linux-gnu-ld"; break;
        case TargetArch::RISCV64: ld = "riscv64-linux-gnu-ld"; break;
        }
        linkCmd = ld + " -static -o " + opts.output + " " + objFile + " 2>&1";
#endif
    }
    if (opts.verbose) {
        std::cerr << "[compile] " << linkCmd << "\n";
    }
    rc = std::system(linkCmd.c_str());
    if (rc != 0) {
        std::cerr << "Linking failed (exit " << rc << ")\n";
        return false;
    }

    // 6) 清理临时文件
    if (!opts.keepAsm) {
        std::remove(asmFile.c_str());
    }
    std::remove(objFile.c_str());

    return true;
}
