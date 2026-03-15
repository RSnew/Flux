// jit.h — Flux JIT 编译器
// MIR → x86-64 机器码（可选高性能路径）
#pragma once
#include "mir.h"
#include "interpreter.h"
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <sys/mman.h>
#include <iostream>

// ═══════════════════════════════════════════════════════════
// JIT 机器码缓冲区
// ═══════════════════════════════════════════════════════════
class MachineCode {
public:
    ~MachineCode() {
        if (mem_ && size_ > 0) {
            munmap(mem_, size_);
        }
    }

    void emit8(uint8_t b) { code_.push_back(b); }
    void emit32(uint32_t v) {
        for (int i = 0; i < 4; i++) {
            code_.push_back((uint8_t)(v & 0xFF));
            v >>= 8;
        }
    }
    void emit64(uint64_t v) {
        for (int i = 0; i < 8; i++) {
            code_.push_back((uint8_t)(v & 0xFF));
            v >>= 8;
        }
    }

    // 补丁偏移
    void patch32(size_t offset, uint32_t value) {
        for (int i = 0; i < 4; i++)
            code_[offset + i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    }
    void patch32(size_t offset, int32_t value) {
        uint32_t uv; std::memcpy(&uv, &value, 4);
        patch32(offset, uv);
    }

    size_t size() const { return code_.size(); }
    size_t here() const { return code_.size(); }

    // 创建可执行内存映射
    bool finalize() {
        size_ = (code_.size() + 4095) & ~4095;  // 对齐到页
        mem_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem_ == MAP_FAILED) { mem_ = nullptr; return false; }
        memcpy(mem_, code_.data(), code_.size());
        if (mprotect(mem_, size_, PROT_READ | PROT_EXEC) != 0) {
            munmap(mem_, size_);
            mem_ = nullptr;
            return false;
        }
        return true;
    }

    void* ptr() const { return mem_; }

private:
    std::vector<uint8_t> code_;
    void* mem_ = nullptr;
    size_t size_ = 0;
};

// ═══════════════════════════════════════════════════════════
// JIT 编译结果
// ═══════════════════════════════════════════════════════════
struct JITCompiledFn {
    std::string name;
    std::shared_ptr<MachineCode> code;
    // 函数指针：double(double*, int nargs)
    // 纯数值函数可以直接调用
    using NumericFn = double(*)(double*, int);
    NumericFn fn = nullptr;
};

// ═══════════════════════════════════════════════════════════
// JIT 编译器
// ═══════════════════════════════════════════════════════════
class JITCompiler {
public:
    // 尝试 JIT 编译一个 MIR 函数
    // 仅支持纯数值函数（参数/返回值均为 double）
    // 返回 nullptr 表示无法 JIT（fallback 到 VM）
    std::shared_ptr<JITCompiledFn> compile(const MIRFunction& fn) {
        // 检查是否可以 JIT：仅支持纯数值函数
        if (!canJIT(fn)) return nullptr;

        auto mc = std::make_shared<MachineCode>();
        auto result = std::make_shared<JITCompiledFn>();
        result->name = fn.name;
        result->code = mc;

        // x86-64 函数序言
        // push rbp
        mc->emit8(0x55);
        // mov rbp, rsp
        mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0xE5);
        // 保存 rdi (args ptr) 和 rsi (nargs)
        // sub rsp, 16
        mc->emit8(0x48); mc->emit8(0x83); mc->emit8(0xEC); mc->emit8(0x10);
        // mov [rbp-8], rdi
        mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0x7D); mc->emit8(0xF8);
        // mov [rbp-16], rsi
        mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0x75); mc->emit8(0xF0);

        // 寄存器分配：用 xmm0-xmm15 存放 SSA 值
        regAlloc_.clear();

        // 编译基本块
        for (auto& bb : fn.blocks) {
            blockOffsets_[bb.id] = mc->here();
            for (auto& inst : bb.instructions) {
                compileInst(*mc, inst, fn);
            }
        }

        // 函数尾声（如果还没 return）
        // movq xmm0 已包含返回值
        // leave; ret
        mc->emit8(0xC9);  // leave
        mc->emit8(0xC3);  // ret

        if (!mc->finalize()) return nullptr;
        result->fn = (JITCompiledFn::NumericFn)mc->ptr();

        compiled_[fn.name] = result;
        return result;
    }

    // 查找已编译函数
    std::shared_ptr<JITCompiledFn> find(const std::string& name) const {
        auto it = compiled_.find(name);
        return it != compiled_.end() ? it->second : nullptr;
    }

    // JIT 执行纯数值函数
    static double callNumeric(JITCompiledFn::NumericFn fn,
                              const std::vector<double>& args) {
        return fn(const_cast<double*>(args.data()), (int)args.size());
    }

    // 统计信息
    size_t compiledCount() const { return compiled_.size(); }

    void dumpStats() const {
        std::cout << "\033[36m[JIT] Compiled functions: " << compiled_.size() << "\033[0m\n";
        for (auto& [name, fn] : compiled_) {
            std::cout << "  " << name << " (" << fn->code->size() << " bytes)\n";
        }
    }

private:
    std::unordered_map<std::string, std::shared_ptr<JITCompiledFn>> compiled_;
    std::unordered_map<int, int> regAlloc_;   // SSA value → xmm register
    std::unordered_map<int, size_t> blockOffsets_;  // block id → code offset

    bool canJIT(const MIRFunction& fn) const {
        // 只 JIT 纯数值函数（无 Call、AsyncCall、字符串操作等）
        for (auto& bb : fn.blocks) {
            for (auto& inst : bb.instructions) {
                switch (inst.op) {
                case MIROp::Const:
                case MIROp::Add: case MIROp::Sub:
                case MIROp::Mul: case MIROp::Div: case MIROp::Mod:
                case MIROp::Eq: case MIROp::Ne:
                case MIROp::Lt: case MIROp::Le:
                case MIROp::Gt: case MIROp::Ge:
                case MIROp::Negate:
                case MIROp::Load: case MIROp::Store:
                case MIROp::Branch: case MIROp::Jump:
                case MIROp::Return:
                    break;  // 支持
                default:
                    return false;  // 含有不支持的指令
                }
            }
        }
        return true;
    }

    int allocReg(int valueId) {
        auto it = regAlloc_.find(valueId);
        if (it != regAlloc_.end()) return it->second;
        int reg = (int)regAlloc_.size() % 16;
        regAlloc_[valueId] = reg;
        return reg;
    }

    void compileInst(MachineCode& mc, const MIRInst& inst, const MIRFunction& fn) {
        switch (inst.op) {
        case MIROp::Const: {
            if (inst.dest < 0) break;
            int reg = allocReg(inst.dest);
            // movsd xmm<reg>, [rip+offset] — 加载立即数
            // 简化：使用 mov rax, imm64; movq xmm, rax
            uint64_t bits;
            double val = inst.constNum;
            memcpy(&bits, &val, 8);
            // mov rax, imm64
            mc.emit8(0x48); mc.emit8(0xB8);
            mc.emit64(bits);
            // movq xmm<reg>, rax  (66 48 0F 6E C0 + reg encoding)
            mc.emit8(0x66); mc.emit8(0x48); mc.emit8(0x0F); mc.emit8(0x6E);
            mc.emit8(0xC0 | (reg << 3));
            break;
        }
        case MIROp::Add: case MIROp::Sub:
        case MIROp::Mul: case MIROp::Div: {
            if (inst.operands.size() < 2 || inst.dest < 0) break;
            int dst = allocReg(inst.dest);
            int lhs = allocReg(inst.operands[0]);
            int rhs = allocReg(inst.operands[1]);
            // movsd xmm<dst>, xmm<lhs> if dst != lhs
            if (dst != lhs) {
                mc.emit8(0xF2); mc.emit8(0x0F); mc.emit8(0x10);
                mc.emit8(0xC0 | (dst << 3) | lhs);
            }
            // op xmm<dst>, xmm<rhs>
            mc.emit8(0xF2); mc.emit8(0x0F);
            switch (inst.op) {
                case MIROp::Add: mc.emit8(0x58); break;
                case MIROp::Sub: mc.emit8(0x5C); break;
                case MIROp::Mul: mc.emit8(0x59); break;
                case MIROp::Div: mc.emit8(0x5E); break;
                default: break;
            }
            mc.emit8(0xC0 | (dst << 3) | rhs);
            break;
        }
        case MIROp::Return: {
            if (!inst.operands.empty()) {
                int src = allocReg(inst.operands[0]);
                if (src != 0) {
                    // movsd xmm0, xmm<src>
                    mc.emit8(0xF2); mc.emit8(0x0F); mc.emit8(0x10);
                    mc.emit8(0xC0 | src);
                }
            }
            mc.emit8(0xC9);  // leave
            mc.emit8(0xC3);  // ret
            break;
        }
        case MIROp::Load: {
            if (inst.dest < 0) break;
            int reg = allocReg(inst.dest);
            // 从参数数组加载：查找参数索引
            int paramIdx = -1;
            for (int i = 0; i < (int)fn.params.size(); i++) {
                if (fn.params[i].first == inst.name) { paramIdx = i; break; }
            }
            if (paramIdx >= 0) {
                // mov rax, [rbp-8]  (args ptr)
                mc.emit8(0x48); mc.emit8(0x8B); mc.emit8(0x45); mc.emit8(0xF8);
                // movsd xmm<reg>, [rax + paramIdx*8]
                mc.emit8(0xF2); mc.emit8(0x0F); mc.emit8(0x10);
                mc.emit8(0x40 | (reg << 3));
                mc.emit8((uint8_t)(paramIdx * 8));
            }
            break;
        }
        default:
            break;  // 其他指令暂不支持，fallback 到 VM
        }
    }
};
