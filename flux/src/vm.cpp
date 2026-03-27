// vm.cpp — Flux 字节码虚拟机实现
#include "vm.h"
#include "compiler.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <future>

// ═══════════════════════════════════════════════════════════
// BytecodeJIT — Chunk 直接编译为 x86-64 原生代码
// ═══════════════════════════════════════════════════════════

bool BytecodeJIT::canJIT(Chunk& chunk, const std::string& selfName) const {
    for (auto& inst : chunk.code) {
        switch (inst.op) {
        case OpCode::PUSH_CONST:
            // 仅允许数值常量
            if (inst.a >= 0 && inst.a < (int)chunk.constants.size() &&
                chunk.constants[inst.a].type != Value::Type::Number)
                return false;
            break;
        case OpCode::PUSH_NIL:
        case OpCode::PUSH_TRUE:
        case OpCode::PUSH_FALSE:
        case OpCode::LOAD:
        case OpCode::STORE:
        case OpCode::DEFINE:
        case OpCode::ADD: case OpCode::SUB:
        case OpCode::MUL: case OpCode::DIV: case OpCode::MOD:
        case OpCode::NEG: case OpCode::NOT:
        case OpCode::EQ: case OpCode::NEQ:
        case OpCode::LT: case OpCode::GT:
        case OpCode::LEQ: case OpCode::GEQ:
        case OpCode::JUMP:
        case OpCode::JUMP_IF_FALSE: case OpCode::JUMP_IF_TRUE:
        case OpCode::RETURN: case OpCode::RETURN_NIL:
        case OpCode::POP:
        case OpCode::PUSH_SCOPE: case OpCode::POP_SCOPE:
            break;
        case OpCode::CALL:
            // 仅允许自递归调用
            if (chunk.names[inst.a] != selfName) return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

// ── 累积递归 → 循环变换 ──
// 检测模式: if (n OP const) return base; return LOAD combine CALL(LOAD - step)
// 编译为: while (!(n OP const)) { result combine= n; n -= step; } return result;
std::shared_ptr<NativeFunc> BytecodeJIT::compileAsLoop(
    const std::string& name, Chunk& chunk, FnDecl* fnDecl) {

    if (fnDecl->params.size() != 1) return nullptr;

    auto& code = chunk.code;
    if (code.size() < 10) return nullptr;

    // 调试: 打印字节码 (禁用)
    // for (size_t ii = 0; ii < code.size(); ii++)
    //     std::cerr << "[" << ii << "] op=" << (int)code[ii].op << " a=" << code[ii].a << " b=" << code[ii].b << "\n";

    size_t pc = 0;
    while (pc < code.size() && code[pc].op == OpCode::PUSH_SCOPE) pc++;

    // 1. LOAD n
    if (pc >= code.size() || code[pc].op != OpCode::LOAD) return nullptr;
    pc++;

    // 2. PUSH_CONST threshold
    if (pc >= code.size() || code[pc].op != OpCode::PUSH_CONST) return nullptr;
    double threshold = chunk.constants[code[pc].a].number;
    pc++;

    // 3. Comparison (LEQ, LT, EQ, etc.)
    if (pc >= code.size()) return nullptr;
    OpCode cmpOp = code[pc].op;
    if (cmpOp != OpCode::LEQ && cmpOp != OpCode::LT && cmpOp != OpCode::EQ &&
        cmpOp != OpCode::GEQ && cmpOp != OpCode::GT) return nullptr;
    pc++;

    // 4. JUMP_IF_FALSE <else>
    if (pc >= code.size() || code[pc].op != OpCode::JUMP_IF_FALSE) return nullptr;
    pc++;

    // Skip PUSH_SCOPE before base case
    while (pc < code.size() && code[pc].op == OpCode::PUSH_SCOPE) pc++;

    // 5. Base case: PUSH_CONST base; RETURN
    if (pc >= code.size() || code[pc].op != OpCode::PUSH_CONST) return nullptr;
    double baseVal = chunk.constants[code[pc].a].number;
    pc++;
    if (pc >= code.size() || code[pc].op != OpCode::RETURN) return nullptr;
    pc++;

    // Skip POP_SCOPE/PUSH_SCOPE between base and recursive case
    while (pc < code.size() && (code[pc].op == OpCode::POP_SCOPE || code[pc].op == OpCode::PUSH_SCOPE)) pc++;

    // 6. Recursive case: LOAD n; LOAD n; PUSH_CONST step; SUB; CALL self 1; MUL; RETURN
    // First LOAD (for the combine operation)
    if (pc >= code.size() || code[pc].op != OpCode::LOAD) return nullptr;
    pc++;

    // Second LOAD (for the recursive arg)
    if (pc >= code.size() || code[pc].op != OpCode::LOAD) return nullptr;
    pc++;

    // PUSH_CONST step
    if (pc >= code.size() || code[pc].op != OpCode::PUSH_CONST) return nullptr;
    double step = chunk.constants[code[pc].a].number;
    pc++;

    // SUB
    if (pc >= code.size() || code[pc].op != OpCode::SUB) return nullptr;
    pc++;

    // CALL self 1
    if (pc >= code.size() || code[pc].op != OpCode::CALL) return nullptr;
    if (chunk.names[code[pc].a] != name || code[pc].b != 1) return nullptr;
    pc++;

    // Combine op (MUL, ADD, etc.)
    if (pc >= code.size()) return nullptr;
    OpCode combineOp = code[pc].op;
    if (combineOp != OpCode::MUL && combineOp != OpCode::ADD) return nullptr;
    pc++;

    // RETURN
    if (pc >= code.size() || code[pc].op != OpCode::RETURN) return nullptr;

    // ── 模式匹配成功! 编译为循环 ──
    auto mc = std::make_shared<MachineCode>();
    auto result = std::make_shared<NativeFunc>();
    result->code = mc;

    // 函数签名: double f(double* args, int nargs)
    // rdi = args, 参数 n = [rdi + 0]

    // ── 函数序言 ──
    mc->emit8(0x55);                                    // push rbp
    mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0xE5);  // mov rbp, rsp

    // 加载 n → xmm0
    // movsd xmm0, [rdi + 0]
    mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x10); mc->emit8(0x07);

    // 加载常量: threshold → xmm1, step → xmm2, base → xmm3
    auto loadImm = [&](int reg, double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, 8);
        mc->emit8(0x48); mc->emit8(0xB8);
        mc->emit64(bits);
        mc->emit8(0x66); mc->emit8(0x48); mc->emit8(0x0F); mc->emit8(0x6E);
        mc->emit8(0xC0 | (reg << 3));
    };

    loadImm(1, threshold);   // xmm1 = threshold
    loadImm(2, step);        // xmm2 = step
    loadImm(3, baseVal);     // xmm3 = base (result accumulator)
    // xmm0 = n (loop variable)
    // xmm3 = result accumulator

    // ── 循环: while (!(n cmpOp threshold)) { result combine= n; n -= step; } ──
    size_t loopTop = mc->here();

    // ucomisd xmm0, xmm1 (compare n with threshold)
    mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x2E); mc->emit8(0xC1);

    // 条件跳转到循环结束 (如果基本条件为真)
    uint8_t jcc;
    switch (cmpOp) {
    case OpCode::LEQ: jcc = 0x86; break;  // jbe (n <= threshold → done)
    case OpCode::LT:  jcc = 0x82; break;  // jb
    case OpCode::EQ:  jcc = 0x84; break;  // je
    case OpCode::GEQ: jcc = 0x83; break;  // jae
    case OpCode::GT:  jcc = 0x87; break;  // ja
    default:          jcc = 0x86; break;
    }
    mc->emit8(0x0F); mc->emit8(jcc);
    size_t exitPatch = mc->here();
    mc->emit32(0);  // placeholder

    // result combine= n
    if (combineOp == OpCode::MUL) {
        // mulsd xmm3, xmm0
        mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x59); mc->emit8(0xD8);
    } else {
        // addsd xmm3, xmm0
        mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x58); mc->emit8(0xD8);
    }

    // n -= step: subsd xmm0, xmm2
    mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x5C); mc->emit8(0xC2);

    // jmp loopTop
    mc->emit8(0xE9);
    int32_t loopRel = (int32_t)(loopTop - (mc->here() + 4));
    mc->emit32(*(uint32_t*)&loopRel);

    // ── 循环结束: return result ──
    size_t exitTarget = mc->here();
    int32_t exitRel = (int32_t)(exitTarget - (exitPatch + 4));
    mc->patch32(exitPatch, exitRel);

    // movsd xmm0, xmm3 (result → return value)
    mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x10); mc->emit8(0xC3);
    // pop rbp; ret
    mc->emit8(0x5D); mc->emit8(0xC3);

    if (!mc->finalize()) return nullptr;
    result->fn = (NativeFunc::Fn)mc->ptr();
    compiled_[name] = result;

    std::cerr << "\033[36m[JIT] " << name << " → native loop ("
              << mc->size() << " bytes)\033[0m\n";
    return result;
}

std::shared_ptr<NativeFunc> BytecodeJIT::compile(
    const std::string& name, Chunk& chunk, FnDecl* fnDecl) {

    // BytecodeJIT 生成 x86-64 机器码，在非 x86-64 平台上禁用
#if !defined(__x86_64__) && !defined(_M_X64)
    return nullptr;
#endif

    if (!canJIT(chunk, name)) return nullptr;

    // ── 快速路径: 检测简单累积递归并转换为循环 ──
    // 模式: if (n OP const) return base; return LOAD * f(LOAD - const)
    // 编译为: result = base; while (!(n OP const)) { result *= n; n -= const; }
    auto loopResult = compileAsLoop(name, chunk, fnDecl);
    if (loopResult) return loopResult;

    // ── 计算最大模拟栈深度 ──
    int maxDepth = 0, curDepth = 0;
    for (auto& inst : chunk.code) {
        switch (inst.op) {
        case OpCode::PUSH_CONST: case OpCode::PUSH_NIL:
        case OpCode::PUSH_TRUE: case OpCode::PUSH_FALSE:
        case OpCode::LOAD:
            curDepth++; break;
        case OpCode::POP: case OpCode::DEFINE:
        case OpCode::STORE:
            break;  // STORE peeks, DEFINE pops
        case OpCode::ADD: case OpCode::SUB:
        case OpCode::MUL: case OpCode::DIV: case OpCode::MOD:
        case OpCode::EQ: case OpCode::NEQ:
        case OpCode::LT: case OpCode::GT:
        case OpCode::LEQ: case OpCode::GEQ:
            curDepth--; break;  // pop 2, push 1
        case OpCode::JUMP_IF_FALSE: case OpCode::JUMP_IF_TRUE:
            curDepth--; break;
        case OpCode::CALL:
            curDepth -= inst.b; curDepth++; break;  // pop args, push result
        default: break;
        }
        if (inst.op == OpCode::POP || inst.op == OpCode::DEFINE) curDepth--;
        if (curDepth > maxDepth) maxDepth = curDepth;
    }

    auto mc = std::make_shared<MachineCode>();
    auto result = std::make_shared<NativeFunc>();
    result->code = mc;

    // ── 寄存器栈 JIT ──
    // 模拟栈直接映射到 XMM 寄存器 (xmm2-xmm9, 共 8 级)
    // xmm0-xmm1 为临时寄存器, xmm10+ 保留
    // 局部变量存放在 [rbp - 16 - slot*8]
    // CALL 前将活跃寄存器溢出到栈帧内存

    int numLocals = (int)chunk.names.size();
    int localBase = 16;  // [rbp - 16 - slot*8] = local slot
    int spillBase = localBase + numLocals * 8;

    // ── 参数槽映射 ──
    std::vector<int> paramSlots;  // 参数对应的 local slot 编号
    for (size_t i = 0; i < fnDecl->params.size(); ++i) {
        auto nit = chunk.nameIdx_.find(fnDecl->params[i].name);
        paramSlots.push_back(nit != chunk.nameIdx_.end() ? nit->second : -1);
    }

    // ── 栈空间计算 ──
    int totalSlots = numLocals + maxDepth + 2;
    int stackSpace = ((totalSlots * 8) + 15) & ~15;

    // 辅助: 生成序言 (push rbp; mov rbp, rsp; sub rsp, N)
    auto emitPrologue = [&]() {
        mc->emit8(0x55);                                    // push rbp
        mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0xE5);  // mov rbp, rsp
        if (stackSpace <= 127) {
            mc->emit8(0x48); mc->emit8(0x83); mc->emit8(0xEC); mc->emit8((uint8_t)stackSpace);
        } else {
            mc->emit8(0x48); mc->emit8(0x81); mc->emit8(0xEC); mc->emit32(stackSpace);
        }
    };

    // 辅助: 将 xmm0, xmm1, ... 存到局部变量槽
    auto emitStoreParams = [&]() {
        for (size_t i = 0; i < paramSlots.size(); ++i) {
            if (paramSlots[i] >= 0) {
                int off = -(localBase + paramSlots[i] * 8);
                int r = (int)i;
                mc->emit8(0xF2);
                if (r >= 8) mc->emit8(0x44);
                mc->emit8(0x0F); mc->emit8(0x11);
                mc->emit8(0x85 | ((r & 7) << 3)); mc->emit32(off);
            }
        }
    };

    // ── 外部入口 entry0: 从 double* args 加载参数 ──
    emitPrologue();
    // 从 args 数组加载到 xmm0, xmm1, ...
    for (size_t i = 0; i < fnDecl->params.size(); ++i) {
        int r = (int)i;
        mc->emit8(0xF2);
        if (r >= 8) mc->emit8(0x44);
        mc->emit8(0x0F); mc->emit8(0x10);
        mc->emit8(0x47 | ((r & 7) << 3)); mc->emit8((uint8_t)(i * 8));
    }
    emitStoreParams();
    // jmp body (跳过 fast entry 的序言)
    mc->emit8(0xE9); // near jmp
    size_t jmpBodyPatch = mc->here();
    mc->emit32(0); // placeholder

    // ── 快速入口 entry1: 参数已在 xmm0, xmm1, ... ──
    size_t fastEntry = mc->here();
    emitPrologue();
    emitStoreParams();

    // 回填 body 跳转
    int32_t jmpRel = (int32_t)(mc->here() - (jmpBodyPatch + 4));
    mc->patch32(jmpBodyPatch, jmpRel);

    // ── 编译状态 ──
    int sp = 0;  // 寄存器栈深度 (0-based index, 对应 xmm(sp+2))
    std::vector<size_t> instOffsets(chunk.code.size() + 1);
    struct PatchInfo { size_t patchOffset; int targetInst; };
    std::vector<PatchInfo> patches;

    // ── 辅助: XMM 寄存器编码 ──
    // 栈位置 pos → xmm(pos+2), 寄存器号 = pos+2
    // movsd 编码: F2 [REX] 0F 10/11 ModRM
    // REX prefix needed for xmm8-xmm15 (reg >= 8)

    auto emitMovsdReg2Reg = [&](int dst, int src) {
        // movsd xmm<dst>, xmm<src>
        uint8_t rex = 0;
        if (dst >= 8) rex |= 0x44;  // REX.R
        if (src >= 8) rex |= 0x41;  // REX.B
        mc->emit8(0xF2);
        if (rex) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(0x10);
        mc->emit8(0xC0 | ((dst & 7) << 3) | (src & 7));
    };

    auto emitMovsdLoadMem = [&](int reg, int rbpOff) {
        // movsd xmm<reg>, [rbp + rbpOff]
        uint8_t rex = 0;
        if (reg >= 8) rex |= 0x44;  // REX.R
        mc->emit8(0xF2);
        if (rex) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(0x10);
        mc->emit8(0x85 | ((reg & 7) << 3));
        mc->emit32(rbpOff);
    };

    auto emitMovsdStoreMem = [&](int rbpOff, int reg) {
        // movsd [rbp + rbpOff], xmm<reg>
        uint8_t rex = 0;
        if (reg >= 8) rex |= 0x44;  // REX.R
        mc->emit8(0xF2);
        if (rex) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(0x11);
        mc->emit8(0x85 | ((reg & 7) << 3));
        mc->emit32(rbpOff);
    };

    // 栈位置 → XMM 寄存器号
    auto xr = [](int pos) -> int { return pos + 2; };

    auto emitLoadLocal = [&](int slot) {
        int off = -(localBase + slot * 8);
        emitMovsdLoadMem(xr(sp), off);
        sp++;
    };

    auto emitStoreLocal = [&](int slot) {
        // peek top → store to local (don't pop)
        int off = -(localBase + slot * 8);
        emitMovsdStoreMem(off, xr(sp - 1));
    };

    auto emitLoadConst = [&](double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, 8);
        // mov rax, imm64; movq xmm<sp+2>, rax
        mc->emit8(0x48); mc->emit8(0xB8);
        mc->emit64(bits);
        int reg = xr(sp);
        // movq xmm<reg>, rax
        mc->emit8(0x66);
        if (reg >= 8) { mc->emit8(0x4C); } else { mc->emit8(0x48); }
        mc->emit8(0x0F); mc->emit8(0x6E);
        mc->emit8(0xC0 | ((reg & 7) << 3));
        sp++;
    };

    auto emitBinaryOp = [&](uint8_t sseOp) {
        // xmm[sp-2] = xmm[sp-2] op xmm[sp-1]
        int dst = xr(sp - 2), rhs = xr(sp - 1);
        mc->emit8(0xF2);
        uint8_t rex = 0;
        if (dst >= 8) rex |= 0x44;
        if (rhs >= 8) rex |= 0x41;
        if (rex) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(sseOp);
        mc->emit8(0xC0 | ((dst & 7) << 3) | (rhs & 7));
        sp--;
    };

    auto emitReturn = [&]() {
        if (sp > 0) {
            int src = xr(sp - 1);
            if (src != 0) emitMovsdReg2Reg(0, src);
        }
        // leave + ret (mov rsp, rbp; pop rbp; ret)
        mc->emit8(0xC9);  // leave
        mc->emit8(0xC3);  // ret
    };

    auto emitCompare = [&](uint8_t cmpType) {
        // ucomisd xmm[sp-2], xmm[sp-1]
        int lhs = xr(sp - 2), rhs = xr(sp - 1);
        mc->emit8(0x66);
        uint8_t rex = 0;
        if (lhs >= 8) rex |= 0x44;
        if (rhs >= 8) rex |= 0x41;
        if (rex) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(0x2E);
        mc->emit8(0xC0 | ((lhs & 7) << 3) | (rhs & 7));
        sp--;
        // setXX al
        switch (cmpType) {
        case 0: mc->emit8(0x0F); mc->emit8(0x94); mc->emit8(0xC0); break; // sete
        case 1: mc->emit8(0x0F); mc->emit8(0x95); mc->emit8(0xC0); break; // setne
        case 2: mc->emit8(0x0F); mc->emit8(0x92); mc->emit8(0xC0); break; // setb
        case 3: mc->emit8(0x0F); mc->emit8(0x97); mc->emit8(0xC0); break; // seta
        case 4: mc->emit8(0x0F); mc->emit8(0x96); mc->emit8(0xC0); break; // setbe
        case 5: mc->emit8(0x0F); mc->emit8(0x93); mc->emit8(0xC0); break; // setae
        }
        // movzx eax, al; cvtsi2sd xmm[sp-1], eax
        mc->emit8(0x0F); mc->emit8(0xB6); mc->emit8(0xC0);
        int dst = xr(sp - 1);
        mc->emit8(0xF2);
        if (dst >= 8) mc->emit8(0x44);
        mc->emit8(0x0F); mc->emit8(0x2A);
        mc->emit8(0xC0 | ((dst & 7) << 3));
    };

    // ── 编译每条指令 ──
    for (size_t i = 0; i < chunk.code.size(); ++i) {
        instOffsets[i] = mc->here();
        const auto& inst = chunk.code[i];

        switch (inst.op) {
        case OpCode::PUSH_CONST:
            emitLoadConst(chunk.constants[inst.a].number);
            break;
        case OpCode::PUSH_NIL:   emitLoadConst(0.0); break;
        case OpCode::PUSH_TRUE:  emitLoadConst(1.0); break;
        case OpCode::PUSH_FALSE: emitLoadConst(0.0); break;
        case OpCode::LOAD:  emitLoadLocal(inst.a); break;
        case OpCode::STORE: emitStoreLocal(inst.a); break;
        case OpCode::DEFINE: {
            // pop → local slot: movsd [rbp+off], xmm[sp-1]; sp--
            sp--;
            int off = -(localBase + inst.a * 8);
            emitMovsdStoreMem(off, xr(sp));
            break;
        }
        case OpCode::ADD:  emitBinaryOp(0x58); break;
        case OpCode::SUB:  emitBinaryOp(0x5C); break;
        case OpCode::MUL:  emitBinaryOp(0x59); break;
        case OpCode::DIV:  emitBinaryOp(0x5E); break;
        case OpCode::NEG: {
            // negate top: xorpd xmm1,xmm1; subsd xmm1,xmm[sp-1]; movsd xmm[sp-1],xmm1
            int r = xr(sp - 1);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9); // xorpd xmm1,xmm1
            // subsd xmm1, xmm[r]
            mc->emit8(0xF2);
            if (r >= 8) mc->emit8(0x41);
            mc->emit8(0x0F); mc->emit8(0x5C); mc->emit8(0xC8 | (r & 7)); // subsd xmm1, xmm[r]
            emitMovsdReg2Reg(r, 1); // movsd xmm[r], xmm1
            break;
        }
        case OpCode::EQ: case OpCode::NEQ:
        case OpCode::LT: case OpCode::GT:
        case OpCode::LEQ: case OpCode::GEQ: {
            // 窥孔优化：比较 + 条件跳转融合
            bool fused = false;
            if (i + 1 < chunk.code.size()) {
                auto nextOp = chunk.code[i+1].op;
                if (nextOp == OpCode::JUMP_IF_FALSE || nextOp == OpCode::JUMP_IF_TRUE) {
                    // 融合: ucomisd + jcc (跳过中间的 setXX/cvtsi2sd)
                    int lhs = xr(sp - 2), rhs = xr(sp - 1);
                    mc->emit8(0x66);
                    uint8_t rex = 0;
                    if (lhs >= 8) rex |= 0x44;
                    if (rhs >= 8) rex |= 0x41;
                    if (rex) mc->emit8(rex);
                    mc->emit8(0x0F); mc->emit8(0x2E);
                    mc->emit8(0xC0 | ((lhs & 7) << 3) | (rhs & 7));
                    sp -= 2;  // 弹出两个操作数

                    // 确定条件跳转码
                    // JUMP_IF_FALSE = "如果比较为假则跳"
                    // JUMP_IF_TRUE  = "如果比较为真则跳"
                    bool jumpOnTrue = (nextOp == OpCode::JUMP_IF_TRUE);
                    uint8_t jcc;
                    switch (inst.op) {
                    case OpCode::EQ:  jcc = jumpOnTrue ? 0x84 : 0x85; break; // je/jne
                    case OpCode::NEQ: jcc = jumpOnTrue ? 0x85 : 0x84; break; // jne/je
                    case OpCode::LT:  jcc = jumpOnTrue ? 0x82 : 0x83; break; // jb/jae
                    case OpCode::GT:  jcc = jumpOnTrue ? 0x87 : 0x86; break; // ja/jbe
                    case OpCode::LEQ: jcc = jumpOnTrue ? 0x86 : 0x87; break; // jbe/ja
                    case OpCode::GEQ: jcc = jumpOnTrue ? 0x83 : 0x82; break; // jae/jb
                    default: jcc = 0x84; break;
                    }
                    mc->emit8(0x0F); mc->emit8(jcc);
                    // 记录下一条指令的偏移 (用于 patch)
                    i++;  // 跳过下一条 JUMP_IF_xxx
                    instOffsets[i] = mc->here() - 2;  // 指向 jcc 开头
                    patches.push_back({mc->here(), chunk.code[i].a});
                    mc->emit32(0);
                    fused = true;
                }
            }
            if (!fused) {
                int cmpType;
                switch (inst.op) {
                case OpCode::EQ:  cmpType = 0; break;
                case OpCode::NEQ: cmpType = 1; break;
                case OpCode::LT:  cmpType = 2; break;
                case OpCode::GT:  cmpType = 3; break;
                case OpCode::LEQ: cmpType = 4; break;
                default:          cmpType = 5; break;
                }
                emitCompare(cmpType);
            }
            break;
        }
        case OpCode::JUMP:
            mc->emit8(0xE9);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        case OpCode::JUMP_IF_FALSE: {
            sp--;
            int r = xr(sp);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9);
            mc->emit8(0x66);
            if (r >= 8) mc->emit8(0x44);
            mc->emit8(0x0F); mc->emit8(0x2E);
            mc->emit8(0xC1 | ((r & 7) << 3));
            mc->emit8(0x0F); mc->emit8(0x84);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        }
        case OpCode::JUMP_IF_TRUE: {
            sp--;
            int r = xr(sp);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9);
            mc->emit8(0x66);
            if (r >= 8) mc->emit8(0x44);
            mc->emit8(0x0F); mc->emit8(0x2E);
            mc->emit8(0xC1 | ((r & 7) << 3));
            mc->emit8(0x0F); mc->emit8(0x85);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        }
        case OpCode::CALL: {
            int argc = inst.b;
            // 溢出活跃的寄存器栈到内存 (CALL 破坏所有 XMM)
            int liveBelow = sp - argc;
            for (int s = 0; s < liveBelow; ++s) {
                int off = -(spillBase + s * 8);
                emitMovsdStoreMem(off, xr(s));
            }

            // 将参数移到 xmm0, xmm1, ... (快速入口约定)
            // 注意避免覆盖：如果 src == dst 则跳过
            for (int a = 0; a < argc; ++a) {
                int src = xr(sp - argc + a);
                if (src != a) emitMovsdReg2Reg(a, src);
            }
            sp -= argc;

            // call 快速入口 (参数已在 xmm0, xmm1, ...)
            mc->emit8(0xE8);
            patches.push_back({mc->here(), -2});  // -2 = fast entry
            mc->emit32(0);

            // 恢复溢出的寄存器
            for (int s = 0; s < liveBelow; ++s) {
                int off = -(spillBase + s * 8);
                emitMovsdLoadMem(xr(s), off);
            }

            // 结果在 xmm0 → xmm[sp+2]
            int dst = xr(sp);
            if (dst != 0) emitMovsdReg2Reg(dst, 0);
            sp++;
            break;
        }
        case OpCode::RETURN:
            emitReturn();
            break;
        case OpCode::RETURN_NIL:
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC0); // xorpd xmm0,xmm0
            sp = 0;
            emitReturn();
            break;
        case OpCode::POP:
            sp--;
            break;
        case OpCode::PUSH_SCOPE:
        case OpCode::POP_SCOPE:
            break;
        case OpCode::NOT: {
            int r = xr(sp - 1);
            // ucomisd xmm[r], zero
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9); // xorpd xmm1,xmm1
            mc->emit8(0x66);
            if (r >= 8) mc->emit8(0x44);
            mc->emit8(0x0F); mc->emit8(0x2E);
            mc->emit8(0xC1 | ((r & 7) << 3));
            mc->emit8(0x0F); mc->emit8(0x94); mc->emit8(0xC0); // sete al
            mc->emit8(0x0F); mc->emit8(0xB6); mc->emit8(0xC0); // movzx eax, al
            // cvtsi2sd xmm[r], eax
            mc->emit8(0xF2);
            if (r >= 8) mc->emit8(0x44);
            mc->emit8(0x0F); mc->emit8(0x2A);
            mc->emit8(0xC0 | ((r & 7) << 3));
            break;
        }
        case OpCode::MOD: {
            // Move operands to xmm0, xmm1 for fmod call
            int lhs = xr(sp - 2), rhs = xr(sp - 1);
            if (lhs != 0) emitMovsdReg2Reg(0, lhs);
            if (rhs != 1) emitMovsdReg2Reg(1, rhs);
            sp--;
            // 溢出活跃寄存器
            for (int s = 0; s < sp - 1; ++s) {
                int off = -(spillBase + s * 8);
                emitMovsdStoreMem(off, xr(s));
            }
            mc->emit8(0x48); mc->emit8(0xB8);
            mc->emit64(reinterpret_cast<uint64_t>(static_cast<double(*)(double,double)>(std::fmod)));
            mc->emit8(0xFF); mc->emit8(0xD0); // call rax
            // 恢复
            for (int s = 0; s < sp - 1; ++s) {
                int off = -(spillBase + s * 8);
                emitMovsdLoadMem(xr(s), off);
            }
            // 结果 xmm0 → xmm[sp-1]
            int dst = xr(sp - 1);
            if (dst != 0) emitMovsdReg2Reg(dst, 0);
            break;
        }
        default:
            return nullptr;
        }
    }
    instOffsets[chunk.code.size()] = mc->here();

    // ── 回填跳转偏移 ──
    for (auto& p : patches) {
        size_t target;
        if (p.targetInst == -2) {
            target = fastEntry;  // 快速入口（自递归）
        } else if (p.targetInst == -1) {
            target = 0;  // 外部入口
        } else {
            target = instOffsets[p.targetInst];
        }
        int32_t rel = (int32_t)(target - (p.patchOffset + 4));
        mc->patch32(p.patchOffset, rel);
    }

    if (!mc->finalize()) return nullptr;
    result->fn = (NativeFunc::Fn)mc->ptr();
    compiled_[name] = result;
    std::cerr << "\033[36m[JIT] " << name << " → native x86-64 ("
              << mc->size() << " bytes)\033[0m\n";
    return result;
}



// ── 热循环 JIT: 将 chunk 中的 while 循环编译为原生 x86-64 ──
// 仅编译循环部分 (loopStart..loopEnd)
// 签名: void hotLoop(JitFn* jitCache, int cacheSize, NanVal* locals)
// locals 是 turbo VM 的 NanVal 局部变量数组
bool BytecodeJIT::compileHotLoop(Chunk& chunk, HotLoop& loop) {
    auto& code = chunk.code;
    int n = (int)code.size();

    // 1. 检测循环: 找最后一条 JUMP 回跳指令 (back-edge)
    int loopStart = -1, loopEnd = -1;
    for (int i = n - 1; i >= 0; --i) {
        if (code[i].op == OpCode::JUMP && code[i].a < i) {
            loopEnd = i;
            loopStart = code[i].a;
            break;
        }
    }
    if (loopStart < 0) return false;

    // 2. 找循环出口
    int exitTarget = -1;
    for (int i = loopStart; i <= loopEnd; ++i) {
        if (code[i].op == OpCode::JUMP_IF_FALSE && code[i].a > loopEnd) {
            exitTarget = code[i].a;
            break;
        }
    }
    if (exitTarget < 0) return false;

    // 3. 检查循环体是否可以 JIT
    for (int i = loopStart; i <= loopEnd; ++i) {
        auto op = code[i].op;
        switch (op) {
        case OpCode::LOAD: case OpCode::STORE: case OpCode::DEFINE:
        case OpCode::PUSH_CONST: case OpCode::PUSH_NIL:
        case OpCode::PUSH_TRUE: case OpCode::PUSH_FALSE:
        case OpCode::ADD: case OpCode::SUB:
        case OpCode::MUL: case OpCode::DIV: case OpCode::MOD:
        case OpCode::NEG: case OpCode::NOT:
        case OpCode::EQ: case OpCode::NEQ:
        case OpCode::LT: case OpCode::GT:
        case OpCode::LEQ: case OpCode::GEQ:
        case OpCode::JUMP: case OpCode::JUMP_IF_FALSE: case OpCode::JUMP_IF_TRUE:
        case OpCode::POP:
        case OpCode::PUSH_SCOPE: case OpCode::POP_SCOPE:
            break;
        case OpCode::CALL:
            if (code[i].a >= (int)chunk.jitCache.size() || !chunk.jitCache[code[i].a])
                return false;
            break;
        default:
            return false;
        }
        if (op == OpCode::PUSH_CONST) {
            if (code[i].a >= 0 && code[i].a < (int)chunk.constants.size() &&
                chunk.constants[code[i].a].type != Value::Type::Number)
                return false;
        }
    }

    // ── 编译原生代码 ──
    auto mc = std::make_shared<MachineCode>();

    // 序言: 保存 callee-saved, 设置 rbx=jitCache, r14=locals
    mc->emit8(0x55);                                    // push rbp
    mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0xE5);  // mov rbp, rsp
    mc->emit8(0x53);                                    // push rbx
    mc->emit8(0x41); mc->emit8(0x54);                   // push r12
    mc->emit8(0x41); mc->emit8(0x55);                   // push r13
    mc->emit8(0x41); mc->emit8(0x56);                   // push r14
    mc->emit8(0x48); mc->emit8(0x89); mc->emit8(0xFB);  // mov rbx, rdi (jitCache)
    mc->emit8(0x49); mc->emit8(0x89); mc->emit8(0xD6);  // mov r14, rdx (locals)
    // 栈空间: 8 sim stack + 8 arg slots = 136 bytes (16-aligned with 5 pushes)
    mc->emit8(0x48); mc->emit8(0x81); mc->emit8(0xEC);
    mc->emit32(136);

    int sp = 0;
    std::vector<size_t> instOffsets(n + 1, 0);
    struct PatchInfo { size_t patchOffset; int targetInst; };
    std::vector<PatchInfo> patches;

    // ── 编码辅助: r14 = NanVal* locals (uint64_t array) ──
    // NanVal::fromDouble stores double bits directly, so movsd works
    auto emitLoadFromR14 = [&](int xmmReg, int slot) {
        int disp = slot * 8;
        mc->emit8(0xF2);
        mc->emit8(xmmReg >= 8 ? 0x45 : 0x41);
        mc->emit8(0x0F); mc->emit8(0x10);
        if (disp == 0)      { mc->emit8(0x06 | ((xmmReg & 7) << 3)); }
        else if (disp < 128){ mc->emit8(0x46 | ((xmmReg & 7) << 3)); mc->emit8((uint8_t)disp); }
        else                { mc->emit8(0x86 | ((xmmReg & 7) << 3)); mc->emit32(disp); }
    };
    auto emitStoreToR14 = [&](int slot, int xmmReg) {
        int disp = slot * 8;
        mc->emit8(0xF2);
        mc->emit8(xmmReg >= 8 ? 0x45 : 0x41);
        mc->emit8(0x0F); mc->emit8(0x11);
        if (disp == 0)      { mc->emit8(0x06 | ((xmmReg & 7) << 3)); }
        else if (disp < 128){ mc->emit8(0x46 | ((xmmReg & 7) << 3)); mc->emit8((uint8_t)disp); }
        else                { mc->emit8(0x86 | ((xmmReg & 7) << 3)); mc->emit32(disp); }
    };
    auto emitLoadFromStack = [&](int xmmReg, int stackPos) {
        int disp = stackPos * 8;
        mc->emit8(0xF2);
        if (xmmReg >= 8) mc->emit8(0x44);
        mc->emit8(0x0F); mc->emit8(0x10);
        if (disp == 0)       { mc->emit8(0x04 | ((xmmReg & 7) << 3)); mc->emit8(0x24); }
        else if (disp < 128) { mc->emit8(0x44 | ((xmmReg & 7) << 3)); mc->emit8(0x24); mc->emit8((uint8_t)disp); }
        else                 { mc->emit8(0x84 | ((xmmReg & 7) << 3)); mc->emit8(0x24); mc->emit32(disp); }
    };
    auto emitStoreToStack = [&](int stackPos, int xmmReg) {
        int disp = stackPos * 8;
        mc->emit8(0xF2);
        if (xmmReg >= 8) mc->emit8(0x44);
        mc->emit8(0x0F); mc->emit8(0x11);
        if (disp == 0)       { mc->emit8(0x04 | ((xmmReg & 7) << 3)); mc->emit8(0x24); }
        else if (disp < 128) { mc->emit8(0x44 | ((xmmReg & 7) << 3)); mc->emit8(0x24); mc->emit8((uint8_t)disp); }
        else                 { mc->emit8(0x84 | ((xmmReg & 7) << 3)); mc->emit8(0x24); mc->emit32(disp); }
    };
    auto emitLoadImm = [&](int xmmReg, double val) {
        uint64_t bits; std::memcpy(&bits, &val, 8);
        mc->emit8(0x48); mc->emit8(0xB8); mc->emit64(bits);
        mc->emit8(0x66); mc->emit8(xmmReg >= 8 ? 0x4C : 0x48);
        mc->emit8(0x0F); mc->emit8(0x6E); mc->emit8(0xC0 | ((xmmReg & 7) << 3));
    };
    auto emitSSEBinop = [&](int dst, int src, uint8_t opcode) {
        mc->emit8(0xF2);
        uint8_t rex = 0x40;
        if (dst >= 8) rex |= 0x04;
        if (src >= 8) rex |= 0x01;
        if (rex != 0x40) mc->emit8(rex);
        mc->emit8(0x0F); mc->emit8(opcode);
        mc->emit8(0xC0 | ((dst & 7) << 3) | (src & 7));
    };

    // ── 编译循环指令 (loopStart..loopEnd) ──
    for (int i = loopStart; i <= loopEnd; ++i) {
        instOffsets[i] = mc->here();
        const auto& inst = code[i];

        switch (inst.op) {
        case OpCode::PUSH_CONST:
            emitLoadImm(0, chunk.constants[inst.a].number);
            emitStoreToStack(sp, 0);
            sp++;
            break;
        case OpCode::PUSH_NIL: case OpCode::PUSH_FALSE:
            emitLoadImm(0, 0.0);
            emitStoreToStack(sp, 0);
            sp++;
            break;
        case OpCode::PUSH_TRUE:
            emitLoadImm(0, 1.0);
            emitStoreToStack(sp, 0);
            sp++;
            break;
        case OpCode::LOAD:
            emitLoadFromR14(0, inst.a);
            emitStoreToStack(sp, 0);
            sp++;
            break;
        case OpCode::STORE:
            emitLoadFromStack(0, sp - 1);
            emitStoreToR14(inst.a, 0);
            break;
        case OpCode::DEFINE:
            sp--;
            emitLoadFromStack(0, sp);
            emitStoreToR14(inst.a, 0);
            break;
        case OpCode::ADD: case OpCode::SUB:
        case OpCode::MUL: case OpCode::DIV: {
            emitLoadFromStack(0, sp - 2);
            emitLoadFromStack(1, sp - 1);
            uint8_t opc = 0x58;
            switch (inst.op) {
                case OpCode::ADD: opc = 0x58; break;
                case OpCode::SUB: opc = 0x5C; break;
                case OpCode::MUL: opc = 0x59; break;
                case OpCode::DIV: opc = 0x5E; break;
                default: break;
            }
            emitSSEBinop(0, 1, opc);
            sp--;
            emitStoreToStack(sp - 1, 0);
            break;
        }
        case OpCode::LT: case OpCode::GT: case OpCode::LEQ:
        case OpCode::GEQ: case OpCode::EQ: case OpCode::NEQ: {
            bool fused = false;
            if (i + 1 <= loopEnd) {
                auto nextOp = code[i+1].op;
                if (nextOp == OpCode::JUMP_IF_FALSE || nextOp == OpCode::JUMP_IF_TRUE) {
                    emitLoadFromStack(0, sp - 2);
                    emitLoadFromStack(1, sp - 1);
                    sp -= 2;
                    mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x2E); mc->emit8(0xC1);
                    bool jumpOnTrue = (nextOp == OpCode::JUMP_IF_TRUE);
                    uint8_t jcc;
                    switch (inst.op) {
                    case OpCode::EQ:  jcc = jumpOnTrue ? 0x84 : 0x85; break;
                    case OpCode::NEQ: jcc = jumpOnTrue ? 0x85 : 0x84; break;
                    case OpCode::LT:  jcc = jumpOnTrue ? 0x82 : 0x83; break;
                    case OpCode::GT:  jcc = jumpOnTrue ? 0x87 : 0x86; break;
                    case OpCode::LEQ: jcc = jumpOnTrue ? 0x86 : 0x87; break;
                    case OpCode::GEQ: jcc = jumpOnTrue ? 0x83 : 0x82; break;
                    default: jcc = 0x84; break;
                    }
                    mc->emit8(0x0F); mc->emit8(jcc);
                    i++;
                    instOffsets[i] = mc->here() - 2;
                    patches.push_back({mc->here(), code[i].a});
                    mc->emit32(0);
                    fused = true;
                }
            }
            if (!fused) {
                emitLoadFromStack(0, sp - 2);
                emitLoadFromStack(1, sp - 1);
                sp--;
                mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x2E); mc->emit8(0xC1);
                uint8_t setcc;
                switch (inst.op) {
                case OpCode::EQ:  setcc = 0x94; break;
                case OpCode::NEQ: setcc = 0x95; break;
                case OpCode::LT:  setcc = 0x92; break;
                case OpCode::GT:  setcc = 0x97; break;
                case OpCode::LEQ: setcc = 0x96; break;
                case OpCode::GEQ: setcc = 0x93; break;
                default: setcc = 0x94; break;
                }
                mc->emit8(0x0F); mc->emit8(setcc); mc->emit8(0xC0);
                mc->emit8(0x0F); mc->emit8(0xB6); mc->emit8(0xC0);
                mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x2A); mc->emit8(0xC0);
                emitStoreToStack(sp - 1, 0);
            }
            break;
        }
        case OpCode::JUMP:
            mc->emit8(0xE9);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        case OpCode::JUMP_IF_FALSE: {
            sp--;
            emitLoadFromStack(0, sp);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x2E); mc->emit8(0xC1);
            mc->emit8(0x0F); mc->emit8(0x84);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        }
        case OpCode::JUMP_IF_TRUE: {
            sp--;
            emitLoadFromStack(0, sp);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x57); mc->emit8(0xC9);
            mc->emit8(0x66); mc->emit8(0x0F); mc->emit8(0x2E); mc->emit8(0xC1);
            mc->emit8(0x0F); mc->emit8(0x85);
            patches.push_back({mc->here(), inst.a});
            mc->emit32(0);
            break;
        }
        case OpCode::CALL: {
            int argc = inst.b;
            int argsBase = 64;
            for (int a = 0; a < argc; ++a) {
                emitLoadFromStack(0, sp - argc + a);
                int disp = argsBase + a * 8;
                mc->emit8(0xF2); mc->emit8(0x0F); mc->emit8(0x11);
                mc->emit8(0x84); mc->emit8(0x24); mc->emit32(disp);
            }
            sp -= argc;
            int fnDisp = inst.a * 8;
            mc->emit8(0x48); mc->emit8(0x8B);
            if (fnDisp < 128) { mc->emit8(0x43); mc->emit8((uint8_t)fnDisp); }
            else              { mc->emit8(0x83); mc->emit32(fnDisp); }
            mc->emit8(0x48); mc->emit8(0x8D); mc->emit8(0x7C); mc->emit8(0x24);
            mc->emit8((uint8_t)argsBase);
            mc->emit8(0xBE); mc->emit32(argc);
            mc->emit8(0xFF); mc->emit8(0xD0);
            emitStoreToStack(sp, 0);
            sp++;
            break;
        }
        case OpCode::POP:
            sp--;
            break;
        case OpCode::PUSH_SCOPE: case OpCode::POP_SCOPE:
            break;
        default:
            return false;
        }
    }

    // 出口标签
    instOffsets[exitTarget] = mc->here();
    instOffsets[loopEnd + 1] = mc->here();

    // 尾声
    mc->emit8(0x48); mc->emit8(0x81); mc->emit8(0xC4); mc->emit32(136);
    mc->emit8(0x41); mc->emit8(0x5E);  // pop r14
    mc->emit8(0x41); mc->emit8(0x5D);  // pop r13
    mc->emit8(0x41); mc->emit8(0x5C);  // pop r12
    mc->emit8(0x5B);                    // pop rbx
    mc->emit8(0x5D);                    // pop rbp
    mc->emit8(0xC3);                    // ret

    // 回填跳转
    for (auto& p : patches) {
        size_t target;
        if (p.targetInst >= 0 && p.targetInst <= n && instOffsets[p.targetInst] != 0)
            target = instOffsets[p.targetInst];
        else if (p.targetInst > loopEnd)
            target = instOffsets[exitTarget];
        else
            target = instOffsets[loopStart];
        int32_t rel = (int32_t)(target - (p.patchOffset + 4));
        mc->patch32(p.patchOffset, rel);
    }

    if (!mc->finalize()) return false;

    static std::vector<std::shared_ptr<MachineCode>> hotLoopCodes;
    hotLoopCodes.push_back(mc);

    loop.loopStart = loopStart;
    loop.loopExitTarget = exitTarget;
    loop.fn = (HotLoopFn)mc->ptr();

    std::cerr << "\033[36m[JIT] hot loop [" << loopStart << ".." << loopEnd
              << "] → native x86-64 (" << mc->size() << " bytes)\033[0m\n";
    return true;
}

// ── 构造 ─────────────────────────────────────────────────
VM::VM(Interpreter& interp) : interp_(interp) {
    stack_.reserve(256);
    locals_.resize(TURBO_MAX_LOCALS);
    frames_.reserve(TURBO_MAX_FRAMES);
    nanStack_.reserve(1024);
}

// ── 预编译所有用户函数 ────────────────────────────────────
void VM::compileFunctions() {
    for (auto& [name, fn] : interp_.functions_) {
        if (!fn || compiledFns_.count(name)) continue;
        Chunk fnChunk;
        Compiler compiler(fnChunk, interp_);
        compiler.compileFunctionBody(fn->body);
        fnChunk.prepareNanConstants();
        compiledFns_[name] = std::move(fnChunk);
    }
    // 尝试 JIT 编译数值函数为原生代码
    for (auto& [name, chunk] : compiledFns_) {
        auto fit = interp_.functions_.find(name);
        if (fit != interp_.functions_.end() && fit->second) {
            jit_.compile(name, chunk, fit->second);
        }
    }
    // 构建 JIT 缓存：为每个 chunk 的 names 数组预查 JIT 函数指针
    for (auto& [name, chunk] : compiledFns_) {
        chunk.jitCache.resize(chunk.names.size(), nullptr);
        for (size_t i = 0; i < chunk.names.size(); ++i) {
            chunk.jitCache[i] = jit_.find(chunk.names[i]);
        }
    }
}

// ═══════════════════════════════════════════════════════════
// 主执行循环
// ═══════════════════════════════════════════════════════════
Value VM::run(Chunk& chunk,
              std::shared_ptr<Environment> env,
              ModuleRuntime* mod) {

    // 为主 chunk 构建 JIT 缓存
    if (chunk.jitCache.empty() && !chunk.names.empty()) {
        chunk.jitCache.resize(chunk.names.size(), nullptr);
        for (size_t i = 0; i < chunk.names.size(); ++i)
            chunk.jitCache[i] = jit_.find(chunk.names[i]);
    }

    // 尝试 Turbo 模式执行主 chunk（仅当无数组/复杂操作时）
    {
        bool canTurbo = true;
        for (auto& inst : chunk.code) {
            if (inst.op == OpCode::MAKE_ARRAY || inst.op == OpCode::MAKE_MAP ||
                inst.op == OpCode::INDEX_SET) {
                canTurbo = false;
                break;
            }
        }
        if (canTurbo) {
            chunk.prepareNanConstants();
            if (chunk.jitCache.empty() && !chunk.names.empty()) {
                chunk.jitCache.resize(chunk.names.size(), nullptr);
                for (size_t i = 0; i < chunk.names.size(); ++i)
                    chunk.jitCache[i] = jit_.find(chunk.names[i]);
            }

            // 尝试热循环 JIT: 编译 while 循环为原生代码
            BytecodeJIT::HotLoop hotLoop;
            if (jit_.compileHotLoop(chunk, hotLoop)) {
                chunk.hotLoopFn = (Chunk::HotLoopFn)hotLoop.fn;
                chunk.hotLoopStart = hotLoop.loopStart;
                chunk.hotLoopExit = hotLoop.loopExitTarget;
            }

            FnDecl dummyFn("__main__", {}, "", {});
            std::vector<Value> emptyArgs;
            return runTurbo(chunk, &dummyFn, emptyArgs, mod);
        }
    }

    // 当前 environment 指针（scope push/pop 会改变它）
    std::shared_ptr<Environment> curEnv = env;

    size_t ip = 0;
    const size_t codeSize = chunk.code.size();

    while (ip < codeSize) {
        const Instruction& inst = chunk.code[ip++];

        switch (inst.op) {

        // ── 常量 ─────────────────────────────────────────
        case OpCode::PUSH_NIL:   push(Value::Nil());  break;
        case OpCode::PUSH_TRUE:  push(Value::Bool(true));  break;
        case OpCode::PUSH_FALSE: push(Value::Bool(false)); break;
        case OpCode::PUSH_CONST: push(chunk.constants[inst.a]); break;

        // ── 变量 ─────────────────────────────────────────
        case OpCode::LOAD: {
            const std::string& name = chunk.names[inst.a];
            push(curEnv->get(name));
            break;
        }
        case OpCode::STORE: {
            const std::string& name = chunk.names[inst.a];
            Value v = pop();
            if (!curEnv->assign(name, v))
                throw std::runtime_error("assignment to undefined variable: " + name);
            push(v);   // store 的返回值
            break;
        }
        case OpCode::DEFINE: {
            const std::string& name = chunk.names[inst.a];
            Value v = pop();
            // 特殊：如果是 __iter_N，需要把 iterable 转为 Array
            if (name.rfind("__iter_", 0) == 0) {
                if (v.type == Value::Type::String) {
                    auto arr = std::make_shared<std::vector<Value>>();
                    for (char c : v.string)
                        arr->push_back(Value::Str(std::string(1, c)));
                    v = Value::Arr(arr);
                } else if (v.type == Value::Type::Map && v.map) {
                    auto arr = std::make_shared<std::vector<Value>>();
                    for (auto& kv : *v.map)
                        arr->push_back(Value::Str(kv.first));
                    v = Value::Arr(arr);
                }
                // Array stays as-is
            }
            curEnv->set(name, v);
            break;
        }

        // ── 持久状态 ─────────────────────────────────────
        case OpCode::LOAD_STATE: {
            const std::string& field = chunk.names[inst.a];
            auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
            auto it = store.find(field);
            if (it == store.end())
                throw std::runtime_error("undefined persistent field: state." + field);
            push(it->second);
            break;
        }
        case OpCode::STORE_STATE: {
            const std::string& field = chunk.names[inst.a];
            auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
            auto it = store.find(field);
            if (it == store.end())
                throw std::runtime_error("undefined persistent field: state." + field
                    + " (declare in persistent { })");
            Value v = pop();
            it->second = v;
            push(v);
            break;
        }

        // ── 算术 ─────────────────────────────────────────
        case OpCode::ADD: {
            Value r = pop(), l = pop();
            if (l.type == Value::Type::String || r.type == Value::Type::String) {
                push(Value::Str(l.toString() + r.toString()));
            } else if (l.type == Value::Type::Number && r.type == Value::Type::Number) {
                push(Value::Num(l.number + r.number));
            } else {
                throw std::runtime_error("VM ADD: incompatible types");
            }
            break;
        }
        case OpCode::SUB: {
            Value& r = peek(0); Value& l = peek(1);
            l.type = Value::Type::Number;
            l.number = l.number - r.number;
            stack_.pop_back();
            break;
        }
        case OpCode::MUL: {
            Value& r = peek(0); Value& l = peek(1);
            l.type = Value::Type::Number;
            l.number = l.number * r.number;
            stack_.pop_back();
            break;
        }
        case OpCode::DIV: {
            Value& r = peek(0); Value& l = peek(1);
            if (r.number == 0) throw std::runtime_error("division by zero");
            l.type = Value::Type::Number;
            l.number = l.number / r.number;
            stack_.pop_back();
            break;
        }
        case OpCode::MOD: {
            Value& r = peek(0); Value& l = peek(1);
            l.type = Value::Type::Number;
            l.number = std::fmod(l.number, r.number);
            stack_.pop_back();
            break;
        }
        case OpCode::NEG: { peek(0).number = -peek(0).number; break; }
        case OpCode::NOT: {
            bool t = peek(0).isTruthy();
            Value& top = peek(0);
            top.type = Value::Type::Bool;
            top.boolean = !t;
            break;
        }

        // ── 比较 ─────────────────────────────────────────
        case OpCode::EQ:  {
            Value r=pop(),l=pop();
            if (l.type==Value::Type::Number && r.type==Value::Type::Number)
                push(Value::Bool(l.number == r.number));
            else if (l.type==Value::Type::Bool && r.type==Value::Type::Bool)
                push(Value::Bool(l.boolean == r.boolean));
            else if (l.type==Value::Type::String && r.type==Value::Type::String)
                push(Value::Bool(l.string == r.string));
            else if (l.type==Value::Type::Nil && r.type==Value::Type::Nil)
                push(Value::Bool(true));
            else if (l.type != r.type)
                push(Value::Bool(false));
            else
                push(Value::Bool(l.toString() == r.toString()));
            break;
        }
        case OpCode::NEQ: {
            Value r=pop(),l=pop();
            if (l.type==Value::Type::Number && r.type==Value::Type::Number)
                push(Value::Bool(l.number != r.number));
            else if (l.type==Value::Type::Bool && r.type==Value::Type::Bool)
                push(Value::Bool(l.boolean != r.boolean));
            else if (l.type==Value::Type::String && r.type==Value::Type::String)
                push(Value::Bool(l.string != r.string));
            else if (l.type==Value::Type::Nil && r.type==Value::Type::Nil)
                push(Value::Bool(false));
            else if (l.type != r.type)
                push(Value::Bool(true));
            else
                push(Value::Bool(l.toString() != r.toString()));
            break;
        }
        case OpCode::LT:  {
            bool res = peek(1).number < peek(0).number;
            stack_.pop_back();
            Value& top = peek(0); top.type = Value::Type::Bool; top.boolean = res;
            break;
        }
        case OpCode::GT:  {
            bool res = peek(1).number > peek(0).number;
            stack_.pop_back();
            Value& top = peek(0); top.type = Value::Type::Bool; top.boolean = res;
            break;
        }
        case OpCode::LEQ: {
            bool res = peek(1).number <= peek(0).number;
            stack_.pop_back();
            Value& top = peek(0); top.type = Value::Type::Bool; top.boolean = res;
            break;
        }
        case OpCode::GEQ: {
            bool res = peek(1).number >= peek(0).number;
            stack_.pop_back();
            Value& top = peek(0); top.type = Value::Type::Bool; top.boolean = res;
            break;
        }

        // ── 作用域 ───────────────────────────────────────
        case OpCode::PUSH_SCOPE:
            curEnv = std::make_shared<Environment>(curEnv);
            break;
        case OpCode::POP_SCOPE:
            if (curEnv->parent()) curEnv = curEnv->parent();
            break;
        case OpCode::POP:
            pop();
            break;

        // ── 控制流 ───────────────────────────────────────
        case OpCode::JUMP:
            ip = (size_t)inst.a;
            break;
        case OpCode::JUMP_IF_FALSE: {
            Value cond = pop();
            if (!cond.isTruthy()) ip = (size_t)inst.a;
            break;
        }
        case OpCode::JUMP_IF_TRUE: {
            Value cond = pop();
            if (cond.isTruthy()) ip = (size_t)inst.a;
            break;
        }

        // ── 函数调用 ─────────────────────────────────────
        case OpCode::CALL: {
            const std::string& name = chunk.names[inst.a];
            int argc = inst.b;

            // 最快路径：JIT 原生代码
            NativeFunc::Fn jitFn = nullptr;
            if (inst.a < (int)chunk.jitCache.size())
                jitFn = chunk.jitCache[inst.a];
            if (jitFn && argc > 0) {
                double dargs[8];
                for (int i = argc - 1; i >= 0; --i) {
                    Value v = pop();
                    dargs[i] = (v.type == Value::Type::Number) ? v.number : 0.0;
                }
                push(Value::Num(jitFn(dargs, argc)));
                break;
            }

            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();

            // Turbo 模式执行编译后的函数
            auto cit = compiledFns_.find(name);
            if (cit != compiledFns_.end()) {
                FnDecl* fn = nullptr;
                if (mod) {
                    auto fit = mod->functions.find(name);
                    if (fit != mod->functions.end()) fn = fit->second;
                }
                if (!fn) {
                    auto fit = interp_.functions_.find(name);
                    if (fit != interp_.functions_.end()) fn = fit->second;
                }
                if (fn) {
                    push(runTurbo(cit->second, fn, args, mod));
                    break;
                }
            }
            // 回退到解释器（内置函数、结构体构造等）
            Value result = interp_.callFunction(name, std::move(args), mod);
            push(result);
            break;
        }

        // ── 模块调用 Module.fn(args) ─────────────────────
        case OpCode::CALL_MODULE: {
            const std::string& combined = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();

            // 拆分 "Module.fn"
            auto dot = combined.find('.');
            if (dot == std::string::npos)
                throw std::runtime_error("VM CALL_MODULE: bad name: " + combined);
            std::string modName = combined.substr(0, dot);
            std::string fnName  = combined.substr(dot + 1);

            // 优先检查标准库模块和用户模块；否则回退为变量方法调用
            bool isKnownModule = interp_.stdlibModules_.count(modName) > 0
                              || interp_.modules_.count(modName) > 0;
            Value result;
            if (isKnownModule) {
                result = interp_.callModuleFunction(modName, fnName, std::move(args));
            } else {
                // 变量方法调用（如 arr.push(), str.upper(), map.keys()）
                Value obj = curEnv->get(modName);
                result = dispatchMethod(obj, fnName, std::move(args), mod);
            }
            push(result);
            break;
        }

        // ── 方法调用 obj.method(args) ────────────────────
        case OpCode::CALL_METHOD: {
            const std::string& method = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value obj = pop();
            Value result = dispatchMethod(obj, method, std::move(args), mod);
            push(result);
            break;
        }

        // ── 返回 ─────────────────────────────────────────
        case OpCode::RETURN:
            return pop();
        case OpCode::RETURN_NIL:
            return Value::Nil();

        // ── 集合 ─────────────────────────────────────────
        case OpCode::MAKE_ARRAY: {
            int count = inst.b;
            auto arr = std::make_shared<std::vector<Value>>(count);
            for (int i = count - 1; i >= 0; --i) (*arr)[i] = pop();
            push(Value::Arr(arr));
            break;
        }
        case OpCode::MAKE_MAP: {
            int pairs = inst.b;
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            // 每对：先 key 后 val（编译时按 key0,val0,key1,val1... 顺序入栈）
            std::vector<std::pair<std::string,Value>> entries(pairs);
            for (int i = pairs - 1; i >= 0; --i) {
                entries[i].second = pop();
                entries[i].first  = pop().toString();
            }
            for (auto& e : entries) (*map)[e.first] = e.second;
            push(Value::MapOf(map));
            break;
        }
        case OpCode::INDEX_GET: {
            Value idx = pop();
            Value obj = pop();
            if (obj.type == Value::Type::Array) {
                if (!obj.array) throw std::runtime_error("null array");
                int i = (int)idx.number;
                if (i < 0) i = (int)obj.array->size() + i;
                if (i < 0 || i >= (int)obj.array->size())
                    throw std::runtime_error("array index out of bounds: " + std::to_string(i));
                push((*obj.array)[i]);
            } else if (obj.type == Value::Type::String) {
                int i = (int)idx.number;
                if (i < 0 || i >= (int)obj.string.size())
                    throw std::runtime_error("string index out of bounds");
                push(Value::Str(std::string(1, obj.string[i])));
            } else if (obj.type == Value::Type::Map) {
                if (!obj.map) { push(Value::Nil()); break; }
                auto it = obj.map->find(idx.toString());
                push(it != obj.map->end() ? it->second : Value::Nil());
            } else {
                throw std::runtime_error("cannot index into " + obj.toString());
            }
            break;
        }
        case OpCode::INDEX_SET: {
            Value val = pop();
            Value idx = pop();
            Value obj = pop();
            if (obj.type == Value::Type::Map) {
                if (!obj.map) throw std::runtime_error("null map");
                (*obj.map)[idx.toString()] = val;
            } else if (obj.type == Value::Type::Array) {
                if (!obj.array) throw std::runtime_error("null array");
                int i = (int)idx.number;
                if (i < 0) i = (int)obj.array->size() + i;
                if (i < 0 || i >= (int)obj.array->size())
                    throw std::runtime_error("array index out of bounds: " + std::to_string(i));
                (*obj.array)[i] = val;
            } else {
                throw std::runtime_error("cannot index-assign into " + obj.toString());
            }
            push(val);
            break;
        }

        // ── 并发 ───────────────────────────────────────────
        case OpCode::ASYNC_CALL: {
            const std::string& name = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();

            auto promise = std::make_shared<std::promise<Value>>();
            auto sfuture = promise->get_future().share();
            Interpreter* self = &interp_;
            std::thread t([self, promise, name, captArgs = std::move(args)]() mutable {
                GILGuard gil;
                try {
                    Value result = self->callFunction(name, std::move(captArgs));
                    promise->set_value(std::move(result));
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            t.detach();
            interp_.pendingTasks_.push_back(sfuture);
            push(Value::Future(std::make_shared<FutureVal>(std::move(sfuture))));
            break;
        }
        case OpCode::ASYNC_MODULE: {
            const std::string& combined = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();

            auto dot = combined.find('.');
            std::string modName = combined.substr(0, dot);
            std::string fnName  = combined.substr(dot + 1);

            auto promise = std::make_shared<std::promise<Value>>();
            auto sfuture = promise->get_future().share();
            Interpreter* self = &interp_;

            // Check if module has a bound thread pool
            ThreadPool* pool = interp_.getModulePool(modName);
            if (pool) {
                pool->submit([self, promise, modName, fnName,
                               captArgs = std::move(args)]() mutable {
                    GILGuard gil;
                    try {
                        Value result = self->callModuleFunction(
                            modName, fnName, std::move(captArgs));
                        promise->set_value(std::move(result));
                    } catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
            } else {
                std::thread t([self, promise, modName, fnName,
                               captArgs = std::move(args)]() mutable {
                    GILGuard gil;
                    try {
                        Value result = self->callModuleFunction(
                            modName, fnName, std::move(captArgs));
                        promise->set_value(std::move(result));
                    } catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
                t.detach();
            }
            interp_.pendingTasks_.push_back(sfuture);
            push(Value::Future(std::make_shared<FutureVal>(std::move(sfuture))));
            break;
        }
        case OpCode::AWAIT: {
            Value futVal = pop();
            if (futVal.type != Value::Type::Future || !futVal.future)
                throw std::runtime_error("await requires a Future value");
            push(futVal.future->get());
            break;
        }

        // ── 原生结构体/闭包/spawn 指令 ───────────────────────
        case OpCode::MAKE_CLOSURE: {
            ASTNode* node = chunk.ast_nodes[inst.a];
            auto* fe = dynamic_cast<FuncExpr*>(node);
            if (!fe) throw std::runtime_error("VM MAKE_CLOSURE: expected FuncExpr");
            auto fv = std::make_shared<FuncVal>();
            fv->params  = fe->params;
            fv->closure = curEnv;
            for (auto& s : fe->body)
                fv->ownedBody.push_back(std::shared_ptr<ASTNode>(s.get(), [](ASTNode*){}));
            push(Value::FuncV(fv));
            break;
        }

        case OpCode::FIELD_GET: {
            const std::string& field = chunk.names[inst.a];
            Value obj = pop();
            if (obj.type == Value::Type::StructInst && obj.structInst) {
                auto it = obj.structInst->fields.find(field);
                if (it != obj.structInst->fields.end())
                    push(it->second);
                else
                    throw std::runtime_error("VM FIELD_GET: no field '" + field + "'");
            } else if (obj.type == Value::Type::Map && obj.map) {
                auto it = obj.map->find(field);
                push(it != obj.map->end() ? it->second : Value::Nil());
            } else {
                throw std::runtime_error("VM FIELD_GET: not a struct/map");
            }
            break;
        }

        case OpCode::FIELD_SET: {
            const std::string& field = chunk.names[inst.a];
            Value val = pop();
            Value obj = pop();
            if (obj.type == Value::Type::StructInst && obj.structInst) {
                obj.structInst->fields[field] = val;
            } else if (obj.type == Value::Type::Map && obj.map) {
                (*obj.map)[field] = val;
            } else {
                throw std::runtime_error("VM FIELD_SET: not a struct/map");
            }
            push(val);
            break;
        }

        case OpCode::STRUCT_CREATE: {
            const std::string& typeName = chunk.names[inst.a];
            int fieldCount = inst.b;
            // 从栈弹出 field name/value pairs (reverse order)
            std::vector<std::pair<std::string, Value>> fields(fieldCount);
            for (int i = fieldCount - 1; i >= 0; --i) {
                fields[i].second = pop();
                fields[i].first  = pop().string;
            }
            // Lookup struct type from env
            Value typeVal = curEnv->get(typeName);
            if (typeVal.type != Value::Type::StructType || !typeVal.structType)
                throw std::runtime_error("VM STRUCT_CREATE: '" + typeName + "' is not a struct type");
            push(interp_.createStructInst(typeVal.structType, fields, curEnv, mod));
            break;
        }

        case OpCode::SPAWN_TASK: {
            ASTNode* node = chunk.ast_nodes[inst.a];
            // Delegate spawn to interpreter (it manages the thread)
            interp_.evalNode(node, curEnv, mod);
            push(Value::Nil());
            break;
        }

        // ── AST 委托（结构体定义/接口/exception 等声明性节点）──
        case OpCode::EVAL_AST: {
            ASTNode* node = chunk.ast_nodes[inst.a];
            Value result = interp_.evalNode(node, curEnv, mod);
            push(result);
            break;
        }

        default:
            throw std::runtime_error("VM: unhandled opcode " +
                                     std::to_string((int)inst.op));
        } // switch
    } // while

    return stack_.empty() ? Value::Nil() : pop();
}

// ═══════════════════════════════════════════════════════════
// Turbo 模式执行引擎
// NaN-boxing + 平坦局部变量 + 内联调用栈 + computed goto
// ═══════════════════════════════════════════════════════════
Value VM::runTurbo(Chunk& topChunk, FnDecl* topFn,
                   std::vector<Value>& topArgs, ModuleRuntime* mod) {

    // ── NaN 栈操作宏 ──
    NanVal* ns = nanStack_.data();
    int nsp = 0;  // NaN stack pointer

    #define NPUSH(v)  (ns[nsp++] = (v))
    #define NPOP()    (ns[--nsp])
    #define NPEEK(off) (ns[nsp - 1 - (off)])

    // ── 需要堆上 Value 对象池（给 NaN-boxed object pointer 用）──
    std::vector<std::unique_ptr<Value>> objPool;
    auto allocObj = [&](Value v) -> NanVal {
        objPool.push_back(std::make_unique<Value>(std::move(v)));
        return NanVal::fromObj(objPool.back().get());
    };

    // NanVal → Value 转换
    auto toValue = [](NanVal n) -> Value {
        if (n.isDouble()) return Value::Num(n.asDouble());
        if (n.isNil() || n.isUnset()) return Value::Nil();
        if (n.isBool()) return Value::Bool(n.asBool());
        if (n.isObj()) return *n.asObj();
        return Value::Nil();
    };

    // Value → NanVal 转换
    auto fromValue = [&](const Value& v) -> NanVal {
        if (v.type == Value::Type::Number) return NanVal::fromDouble(v.number);
        if (v.type == Value::Type::Bool) return NanVal::boolean(v.boolean);
        if (v.type == Value::Type::Nil) return NanVal::nil();
        return allocObj(v);
    };

    // ── 调用栈初始化 ──
    int frameCount = 0;
    TurboFrame* frames = frames_.data();
    NanVal* locals = locals_.data();

    // 设置初始帧
    auto& f0 = frames[frameCount++];
    f0.chunk = &topChunk;
    f0.ip = 0;
    f0.localsBase = 0;
    f0.numLocals = (int)topChunk.names.size();
    f0.stackBase = 0;

    // 初始化所有槽为 UNSET（用于区分"未定义"和"nil"）
    for (int i = 0; i < f0.numLocals; ++i)
        locals[i] = NanVal::unset();

    // 绑定参数到局部变量槽
    for (size_t i = 0; i < topFn->params.size(); ++i) {
        auto nit = topChunk.nameIdx_.find(topFn->params[i].name);
        if (nit != topChunk.nameIdx_.end()) {
            if (i < topArgs.size())
                locals[nit->second] = fromValue(topArgs[i]);
            else
                locals[nit->second] = NanVal::nil();
        }
    }

    // ── Computed goto dispatch table ──
    // GCC/Clang 扩展：用跳转表替代 switch，减少分支预测失败
    #if defined(__GNUC__) || defined(__clang__)
    #define USE_COMPUTED_GOTO 1
    static void* dispatchTable[] = {
        &&op_PUSH_NIL, &&op_PUSH_TRUE, &&op_PUSH_FALSE, &&op_PUSH_CONST,
        &&op_LOAD, &&op_STORE, &&op_DEFINE,
        &&op_LOAD_STATE, &&op_STORE_STATE,
        &&op_ADD, &&op_SUB, &&op_MUL, &&op_DIV, &&op_MOD, &&op_NEG, &&op_NOT,
        &&op_EQ, &&op_NEQ, &&op_LT, &&op_GT, &&op_LEQ, &&op_GEQ,
        &&op_PUSH_SCOPE, &&op_POP_SCOPE, &&op_POP,
        &&op_JUMP, &&op_JUMP_IF_FALSE, &&op_JUMP_IF_TRUE,
        &&op_CALL, &&op_CALL_MODULE, &&op_CALL_METHOD,
        &&op_RETURN, &&op_RETURN_NIL,
        &&op_MAKE_ARRAY, &&op_MAKE_MAP, &&op_INDEX_GET, &&op_INDEX_SET,
        &&op_ASYNC_CALL, &&op_ASYNC_MODULE, &&op_AWAIT,
        &&op_EVAL_AST,
        &&op_MAKE_CLOSURE, &&op_FIELD_GET, &&op_FIELD_SET,
        &&op_STRUCT_CREATE, &&op_SPAWN_TASK,
    };
    #define DISPATCH() do { \
        if (frame->ip >= frame->chunk->code.size()) goto turbo_exit; \
        inst = &frame->chunk->code[frame->ip++]; \
        goto *dispatchTable[static_cast<int>(inst->op)]; \
    } while(0)
    #else
    #define USE_COMPUTED_GOTO 0
    #define DISPATCH() continue
    #endif

    // ── 主循环 ──
    TurboFrame* frame = &frames[0];
    const Instruction* inst;

    #if USE_COMPUTED_GOTO
    DISPATCH();
    #else
    while (frame->ip < frame->chunk->code.size()) {
        inst = &frame->chunk->code[frame->ip++];
        switch (inst->op) {
    #endif

    // ── 常量 ─────────────────────────────────────────
    op_PUSH_NIL:
        NPUSH(NanVal::nil());
        DISPATCH();
    op_PUSH_TRUE:
        NPUSH(NanVal::boolean(true));
        DISPATCH();
    op_PUSH_FALSE:
        NPUSH(NanVal::boolean(false));
        DISPATCH();
    op_PUSH_CONST: {
        // 快速路径：NaN-boxed 常量（数字直接用）
        auto& c = frame->chunk->constants[inst->a];
        if (c.type == Value::Type::Number)
            NPUSH(NanVal::fromDouble(c.number));
        else
            NPUSH(fromValue(c));
        DISPATCH();
    }

    // ── 变量（局部变量槽直接访问，UNSET 时回退全局）──
    op_LOAD: {
        NanVal v = locals[frame->localsBase + inst->a];
        if (__builtin_expect(!v.isUnset(), 1)) {
            NPUSH(v);
        } else {
            // 回退到全局环境（访问全局变量）
            const std::string& name = frame->chunk->names[inst->a];
            try {
                NPUSH(fromValue(interp_.globalEnv_->get(name)));
            } catch (...) {
                NPUSH(NanVal::nil());
            }
        }
        DISPATCH();
    }
    op_STORE: {
        NanVal v = NPEEK(0);  // peek, don't pop (STORE returns value)
        locals[frame->localsBase + inst->a] = v;
        DISPATCH();
    }
    op_DEFINE: {
        NanVal v = NPOP();
        int slot = frame->localsBase + inst->a;
        // __iter_ 转换（字符串/Map → 数组）需要走慢路径
        const std::string& name = frame->chunk->names[inst->a];
        if (name.rfind("__iter_", 0) == 0 && v.isObj()) {
            Value val = toValue(v);
            if (val.type == Value::Type::String) {
                auto arr = std::make_shared<std::vector<Value>>();
                for (char ch : val.string)
                    arr->push_back(Value::Str(std::string(1, ch)));
                v = allocObj(Value::Arr(arr));
            } else if (val.type == Value::Type::Map && val.map) {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *val.map)
                    arr->push_back(Value::Str(kv.first));
                v = allocObj(Value::Arr(arr));
            }
        }
        locals[slot] = v;
        DISPATCH();
    }

    // ── 持久状态（慢路径）─────────────────────────────
    op_LOAD_STATE: {
        const std::string& field = frame->chunk->names[inst->a];
        auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
        auto it = store.find(field);
        if (it == store.end())
            throw std::runtime_error("undefined persistent field: state." + field);
        NPUSH(fromValue(it->second));
        DISPATCH();
    }
    op_STORE_STATE: {
        const std::string& field = frame->chunk->names[inst->a];
        auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
        auto it = store.find(field);
        if (it == store.end())
            throw std::runtime_error("undefined persistent field: state." + field);
        NanVal v = NPEEK(0);
        it->second = toValue(v);
        DISPATCH();
    }

    // ── 算术（纯 NaN-boxing 快速路径）─────────────────
    op_ADD: {
        NanVal r = NPOP();
        NanVal& l = ns[nsp - 1];
        if (__builtin_expect(l.isDouble() && r.isDouble(), 1)) {
            l = NanVal::fromDouble(l.asDouble() + r.asDouble());
        } else {
            // 慢路径：字符串拼接等
            Value lv = toValue(l), rv = toValue(r);
            if (lv.type == Value::Type::String || rv.type == Value::Type::String)
                l = allocObj(Value::Str(lv.toString() + rv.toString()));
            else
                l = NanVal::fromDouble(lv.number + rv.number);
        }
        DISPATCH();
    }
    op_SUB: {
        double r = ns[nsp - 1].asDouble();
        nsp--;
        ns[nsp - 1] = NanVal::fromDouble(ns[nsp - 1].asDouble() - r);
        DISPATCH();
    }
    op_MUL: {
        double r = ns[nsp - 1].asDouble();
        nsp--;
        ns[nsp - 1] = NanVal::fromDouble(ns[nsp - 1].asDouble() * r);
        DISPATCH();
    }
    op_DIV: {
        double r = ns[nsp - 1].asDouble();
        nsp--;
        if (r == 0) throw std::runtime_error("division by zero");
        ns[nsp - 1] = NanVal::fromDouble(ns[nsp - 1].asDouble() / r);
        DISPATCH();
    }
    op_MOD: {
        double r = ns[nsp - 1].asDouble();
        nsp--;
        ns[nsp - 1] = NanVal::fromDouble(std::fmod(ns[nsp - 1].asDouble(), r));
        DISPATCH();
    }
    op_NEG:
        ns[nsp - 1] = NanVal::fromDouble(-ns[nsp - 1].asDouble());
        DISPATCH();
    op_NOT:
        ns[nsp - 1] = NanVal::boolean(!ns[nsp - 1].isTruthy());
        DISPATCH();

    // ── 比较 ─────────────────────────────────────────
    op_EQ: {
        NanVal r = NPOP(), &l = ns[nsp - 1];
        if (l.isDouble() && r.isDouble())
            l = NanVal::boolean(l.asDouble() == r.asDouble());
        else if (l.bits == r.bits)
            l = NanVal::boolean(true);
        else if (l.isObj() && r.isObj())
            l = NanVal::boolean(toValue(l).toString() == toValue(r).toString());
        else
            l = NanVal::boolean(false);
        DISPATCH();
    }
    op_NEQ: {
        NanVal r = NPOP(), &l = ns[nsp - 1];
        if (l.isDouble() && r.isDouble())
            l = NanVal::boolean(l.asDouble() != r.asDouble());
        else if (l.bits == r.bits)
            l = NanVal::boolean(false);
        else if (l.isObj() && r.isObj())
            l = NanVal::boolean(toValue(l).toString() != toValue(r).toString());
        else
            l = NanVal::boolean(true);
        DISPATCH();
    }
    op_LT: {
        double r = ns[--nsp].asDouble();
        ns[nsp - 1] = NanVal::boolean(ns[nsp - 1].asDouble() < r);
        DISPATCH();
    }
    op_GT: {
        double r = ns[--nsp].asDouble();
        ns[nsp - 1] = NanVal::boolean(ns[nsp - 1].asDouble() > r);
        DISPATCH();
    }
    op_LEQ: {
        double r = ns[--nsp].asDouble();
        ns[nsp - 1] = NanVal::boolean(ns[nsp - 1].asDouble() <= r);
        DISPATCH();
    }
    op_GEQ: {
        double r = ns[--nsp].asDouble();
        ns[nsp - 1] = NanVal::boolean(ns[nsp - 1].asDouble() >= r);
        DISPATCH();
    }

    // ── 作用域（Turbo 模式中用局部变量槽，scope 是空操作）──
    op_PUSH_SCOPE:
        DISPATCH();
    op_POP_SCOPE:
        DISPATCH();
    op_POP:
        nsp--;
        DISPATCH();

    // ── 控制流 ─────────────────────────────────────────
    op_JUMP: {
        int target = inst->a;
        // 热循环 JIT: 当回跳到热循环入口时，调用原生循环
        if (target < (int)frame->ip && frame->chunk->hotLoopFn &&
            target == frame->chunk->hotLoopStart) {
            // 调用原生循环: void hotLoop(JitFn*, int, NanVal*)
            frame->chunk->hotLoopFn(
                frame->chunk->jitCache.data(),
                (int)frame->chunk->jitCache.size(),
                &locals[frame->localsBase]);
            // 跳到循环出口
            frame->ip = (size_t)frame->chunk->hotLoopExit;
            DISPATCH();
        }
        frame->ip = (size_t)target;
        DISPATCH();
    }
    op_JUMP_IF_FALSE: {
        NanVal cond = NPOP();
        if (!cond.isTruthy()) frame->ip = (size_t)inst->a;
        DISPATCH();
    }
    op_JUMP_IF_TRUE: {
        NanVal cond = NPOP();
        if (cond.isTruthy()) frame->ip = (size_t)inst->a;
        DISPATCH();
    }

    // ── 函数调用（内联调用栈，零堆分配）─────────────────
    op_CALL: {
        const std::string& name = frame->chunk->names[inst->a];
        int argc = inst->b;

        // 最快路径：JIT 原生代码（从预构建缓存查找，无 hash map）
        NativeFunc::Fn jitFn = nullptr;
        if (inst->a < (int)frame->chunk->jitCache.size())
            jitFn = frame->chunk->jitCache[inst->a];
        if (jitFn && argc > 0) {
            // 收集参数（全部转为 double）
            double args[8];  // 最多 8 个参数
            for (int i = argc - 1; i >= 0; --i) {
                NanVal v = NPOP();
                args[i] = v.isDouble() ? v.asDouble() : (v.isTruthy() ? 1.0 : 0.0);
            }
            double result = jitFn(args, argc);
            NPUSH(NanVal::fromDouble(result));
            DISPATCH();
        }

        // 查找编译后的函数
        auto cit = compiledFns_.find(name);
        if (cit != compiledFns_.end()) {
            FnDecl* fn = nullptr;
            auto fit = interp_.functions_.find(name);
            if (fit != interp_.functions_.end()) fn = fit->second;

            if (fn && frameCount < TURBO_MAX_FRAMES) {
                Chunk& fnChunk = cit->second;
                int newBase = frame->localsBase + frame->numLocals;

                // 检查局部变量空间
                if (newBase + (int)fnChunk.names.size() < TURBO_MAX_LOCALS) {
                    // 初始化新帧槽为 UNSET
                    int nSlots = (int)fnChunk.names.size();
                    for (int s = 0; s < nSlots; ++s)
                        locals[newBase + s] = NanVal::unset();
                    // 绑定参数到新帧的局部变量槽
                    for (int i = argc - 1; i >= 0; --i) {
                        NanVal arg = NPOP();
                        if (i < (int)fn->params.size()) {
                            auto nit = fnChunk.nameIdx_.find(fn->params[i].name);
                            if (nit != fnChunk.nameIdx_.end())
                                locals[newBase + nit->second] = arg;
                        }
                    }
                    // 保存当前帧状态
                    frame->stackBase = (size_t)nsp;
                    // 压入新帧
                    auto& nf = frames[frameCount++];
                    nf.chunk = &fnChunk;
                    nf.ip = 0;
                    nf.localsBase = newBase;
                    nf.numLocals = (int)fnChunk.names.size();
                    nf.stackBase = (size_t)nsp;
                    frame = &frames[frameCount - 1];
                    DISPATCH();
                }
            }
        }

        // 慢路径：内置函数 / 非编译函数
        {
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i)
                args[i] = toValue(NPOP());
            Value result = interp_.callFunction(name, std::move(args), mod);
            NPUSH(fromValue(result));
        }
        DISPATCH();
    }

    // ── 返回 ─────────────────────────────────────────
    op_RETURN: {
        NanVal result = NPOP();
        frameCount--;
        if (frameCount <= 0) {
            return toValue(result);
        }
        frame = &frames[frameCount - 1];
        nsp = (int)frame->stackBase;
        NPUSH(result);
        DISPATCH();
    }
    op_RETURN_NIL: {
        frameCount--;
        if (frameCount <= 0) {
            return Value::Nil();
        }
        frame = &frames[frameCount - 1];
        nsp = (int)frame->stackBase;
        NPUSH(NanVal::nil());
        DISPATCH();
    }

    // ── 以下操作通过转换到 Value 走慢路径 ──────────────
    op_CALL_MODULE:
    op_CALL_METHOD:
    op_MAKE_ARRAY:
    op_MAKE_MAP:
    op_INDEX_GET:
    op_INDEX_SET:
    op_ASYNC_CALL:
    op_ASYNC_MODULE:
    op_AWAIT:
    op_EVAL_AST:
    op_MAKE_CLOSURE:
    op_FIELD_GET:
    op_FIELD_SET:
    op_STRUCT_CREATE:
    op_SPAWN_TASK:
    {
        // 回退到标准 VM：将 NaN 栈转为 Value 栈，执行单条指令
        // 先把 NaN 栈内容转换到 Value 栈
        stack_.clear();
        for (int i = 0; i < nsp; ++i)
            stack_.push_back(toValue(ns[i]));

        {
            // 用旧的 Environment 路径执行这条指令
            auto curEnv = std::make_shared<Environment>(interp_.globalEnv_);
            // 把当前帧的局部变量放入 env
            for (int i = 0; i < frame->numLocals && i < (int)frame->chunk->names.size(); ++i) {
                NanVal lv = locals[frame->localsBase + i];
                curEnv->set(frame->chunk->names[i], toValue(lv));
            }

            // 复用 run() 中的逻辑执行单条复杂指令
            switch (inst->op) {
            case OpCode::CALL_MODULE: {
                const std::string& combined = frame->chunk->names[inst->a];
                int argc = inst->b;
                std::vector<Value> args(argc);
                for (int i = argc - 1; i >= 0; --i) args[i] = pop();
                auto dot = combined.find('.');
                if (dot == std::string::npos)
                    throw std::runtime_error("VM CALL_MODULE: bad name: " + combined);
                std::string modName = combined.substr(0, dot);
                std::string fnName  = combined.substr(dot + 1);
                bool isKnownModule = interp_.stdlibModules_.count(modName) > 0
                                  || interp_.modules_.count(modName) > 0;
                Value result;
                if (isKnownModule) {
                    result = interp_.callModuleFunction(modName, fnName, std::move(args));
                } else {
                    Value obj = curEnv->get(modName);
                    result = dispatchMethod(obj, fnName, std::move(args), mod);
                }
                push(result);
                break;
            }
            case OpCode::CALL_METHOD: {
                const std::string& method = frame->chunk->names[inst->a];
                int argc = inst->b;
                std::vector<Value> args(argc);
                for (int i = argc - 1; i >= 0; --i) args[i] = pop();
                Value obj = pop();
                push(dispatchMethod(obj, method, std::move(args), mod));
                break;
            }
            case OpCode::MAKE_ARRAY: {
                int count = inst->b;
                auto arr = std::make_shared<std::vector<Value>>(count);
                for (int i = count - 1; i >= 0; --i) (*arr)[i] = pop();
                push(Value::Arr(arr));
                break;
            }
            case OpCode::INDEX_GET: {
                Value idx = pop(), obj = pop();
                if (obj.type == Value::Type::Array) {
                    int i = (int)idx.number;
                    if (i < 0) i = (int)obj.array->size() + i;
                    push((*obj.array)[i]);
                } else if (obj.type == Value::Type::String) {
                    int i = (int)idx.number;
                    push(Value::Str(std::string(1, obj.string[i])));
                } else if (obj.type == Value::Type::Map) {
                    auto it = obj.map->find(idx.toString());
                    push(it != obj.map->end() ? it->second : Value::Nil());
                } else {
                    push(Value::Nil());
                }
                break;
            }
            default:
                // 其他复杂操作直接委托给解释器
                push(interp_.evalNode(frame->chunk->ast_nodes[inst->a], curEnv, mod));
                break;
            }
        }

        // 转换回 NaN 栈
        nsp = 0;
        for (auto& v : stack_)
            ns[nsp++] = fromValue(v);
        stack_.clear();
        DISPATCH();
    }

    #if !USE_COMPUTED_GOTO
        default:
            throw std::runtime_error("Turbo VM: unhandled opcode");
        } // switch
    } // while
    #endif

turbo_exit:
    if (nsp > 0)
        return toValue(ns[nsp - 1]);
    return Value::Nil();

    #undef NPUSH
    #undef NPOP
    #undef NPEEK
    #undef DISPATCH
}

// ═══════════════════════════════════════════════════════════
// 方法分发（Array / String / Map 方法，复用解释器逻辑）
// ═══════════════════════════════════════════════════════════
Value VM::dispatchMethod(Value& obj, const std::string& m,
                         std::vector<Value> args, ModuleRuntime* /*mod*/) {
    // ── Array ──────────────────────────────────────────────
    if (obj.type == Value::Type::Array && obj.array) {
        if (m == "push" || m == "append") {
            for (auto& v : args) obj.array->push_back(v);
            return Value::Num((double)obj.array->size());
        }
        if (m == "pop") {
            if (obj.array->empty()) throw std::runtime_error("pop from empty array");
            Value last = obj.array->back();
            obj.array->pop_back();
            return last;
        }
        if (m == "len"  || m == "size")  return Value::Num((double)obj.array->size());
        if (m == "first") return obj.array->empty() ? Value::Nil() : obj.array->front();
        if (m == "last")  return obj.array->empty() ? Value::Nil() : obj.array->back();
        if (m == "contains") {
            if (args.empty()) return Value::Bool(false);
            std::string needle = args[0].toString();
            for (auto& v : *obj.array)
                if (v.toString() == needle) return Value::Bool(true);
            return Value::Bool(false);
        }
        if (m == "join") {
            std::string sep = args.empty() ? "" : args[0].toString();
            std::string result;
            for (size_t i = 0; i < obj.array->size(); ++i) {
                if (i > 0) result += sep;
                result += (*obj.array)[i].toString();
            }
            return Value::Str(result);
        }
        if (m == "reverse") {
            std::reverse(obj.array->begin(), obj.array->end());
            return obj;
        }
    }
    // ── String ─────────────────────────────────────────────
    if (obj.type == Value::Type::String) {
        if (m == "len" || m == "size") return Value::Num((double)obj.string.size());
        if (m == "upper") {
            std::string s = obj.string;
            for (auto& c : s) c = (char)toupper((unsigned char)c);
            return Value::Str(s);
        }
        if (m == "lower") {
            std::string s = obj.string;
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return Value::Str(s);
        }
        if (m == "split") {
            std::string sep = args.empty() ? " " : args[0].toString();
            auto arr = std::make_shared<std::vector<Value>>();
            size_t start = 0, pos = 0;
            while ((pos = obj.string.find(sep, start)) != std::string::npos) {
                arr->push_back(Value::Str(obj.string.substr(start, pos - start)));
                start = pos + sep.size();
            }
            arr->push_back(Value::Str(obj.string.substr(start)));
            return Value::Arr(arr);
        }
        if (m == "contains") {
            if (args.empty()) return Value::Bool(false);
            return Value::Bool(obj.string.find(args[0].toString()) != std::string::npos);
        }
        if (m == "trim") {
            std::string s = obj.string;
            size_t l = s.find_first_not_of(" \t\n\r");
            size_t r = s.find_last_not_of(" \t\n\r");
            return Value::Str(l == std::string::npos ? "" : s.substr(l, r - l + 1));
        }
    }
    // ── Map ────────────────────────────────────────────────
    if (obj.type == Value::Type::Map && obj.map) {
        if (m == "len"  || m == "size") return Value::Num((double)obj.map->size());
        if (m == "has")  return Value::Bool(!args.empty() && obj.map->count(args[0].toString()) > 0);
        if (m == "get") {
            if (args.empty()) return Value::Nil();
            auto it = obj.map->find(args[0].toString());
            return it != obj.map->end() ? it->second : (args.size()>1 ? args[1] : Value::Nil());
        }
        if (m == "set") {
            if (args.size() < 2) return Value::Nil();
            (*obj.map)[args[0].toString()] = args[1];
            return args[1];
        }
        if (m == "delete" || m == "remove")
            return Value::Bool(!args.empty() && obj.map->erase(args[0].toString()) > 0);
        if (m == "keys") {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& kv : *obj.map) arr->push_back(Value::Str(kv.first));
            return Value::Arr(arr);
        }
        if (m == "values") {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& kv : *obj.map) arr->push_back(kv.second);
            return Value::Arr(arr);
        }
        if (m == "entries") {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& kv : *obj.map) {
                auto pair = std::make_shared<std::vector<Value>>();
                pair->push_back(Value::Str(kv.first));
                pair->push_back(kv.second);
                arr->push_back(Value::Arr(pair));
            }
            return Value::Arr(arr);
        }
    }
    // ── StructInst method dispatch ───────────────────────────
    if (obj.type == Value::Type::StructInst && obj.structInst) {
        // Field access via getter (method name matches field name with no args)
        if (args.empty() && obj.structInst->fields.count(m)) {
            return obj.structInst->fields[m];
        }
        // Method dispatch
        if (obj.structInst->type) {
            auto* method = obj.structInst->type->findMethod(m);
            if (method) {
                // Build env with "self" bound to the struct instance
                auto methodEnv = std::make_shared<Environment>(interp_.globalEnv_);
                methodEnv->set("self", obj);
                // Bind fields as local variables for convenience
                for (auto& [fname, fval] : obj.structInst->fields)
                    methodEnv->set(fname, fval);
                // Bind parameters
                for (size_t i = 0; i < method->params.size() && i < args.size(); ++i)
                    methodEnv->set(method->params[i].name, args[i]);
                // Execute method body via interpreter
                Value result = Value::Nil();
                for (auto& stmt : method->body) {
                    try {
                        result = interp_.evalNode(stmt.get(), methodEnv);
                    } catch (ReturnSignal& r) {
                        result = r.value;
                        break;
                    }
                }
                // Write back field changes from self assignments
                for (auto& [fname, fval] : obj.structInst->fields) {
                    if (methodEnv->has(fname))
                        obj.structInst->fields[fname] = methodEnv->get(fname);
                }
                return result;
            }
        }
    }
    // ── Future methods ─────────────────────────────────────────
    if (obj.type == Value::Type::Future && obj.future) {
        if (m == "await" || m == "get" || m == "value") return obj.future->get();
        if (m == "isReady") return Value::Bool(obj.future->isReady());
    }
    // ── Channel methods ────────────────────────────────────────
    if (obj.type == Value::Type::Chan && obj.chan) {
        if (m == "send" && !args.empty()) {
            obj.chan->send(args[0]);
            return Value::Nil();
        }
        if (m == "recv") {
            auto val = obj.chan->recv();
            return val ? *val : Value::Nil();
        }
        if (m == "tryRecv") {
            auto val = obj.chan->tryRecv();
            return val ? *val : Value::Nil();
        }
        if (m == "close") { obj.chan->close(); return Value::Nil(); }
        if (m == "len") {
            std::unique_lock<std::mutex> lk(obj.chan->mu);
            return Value::Num((double)obj.chan->q.size());
        }
        if (m == "isClosed") {
            std::unique_lock<std::mutex> lk(obj.chan->mu);
            return Value::Bool(obj.chan->closed_);
        }
    }
    throw std::runtime_error("VM: unknown method '" + m + "' on " + obj.toString());
}

// ═══════════════════════════════════════════════════════════
// 调试：反汇编
// ═══════════════════════════════════════════════════════════
void Chunk::dump(const std::string& title) const {
    std::cout << "=== Bytecode: " << title << " ===\n";
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        std::string name;
        switch (inst.op) {
#define CASE(X) case OpCode::X: name = #X; break;
            CASE(PUSH_NIL) CASE(PUSH_TRUE) CASE(PUSH_FALSE) CASE(PUSH_CONST)
            CASE(LOAD) CASE(STORE) CASE(DEFINE)
            CASE(LOAD_STATE) CASE(STORE_STATE)
            CASE(ADD) CASE(SUB) CASE(MUL) CASE(DIV) CASE(MOD) CASE(NEG) CASE(NOT)
            CASE(EQ) CASE(NEQ) CASE(LT) CASE(GT) CASE(LEQ) CASE(GEQ)
            CASE(PUSH_SCOPE) CASE(POP_SCOPE) CASE(POP)
            CASE(JUMP) CASE(JUMP_IF_FALSE) CASE(JUMP_IF_TRUE)
            CASE(CALL) CASE(CALL_MODULE) CASE(CALL_METHOD) CASE(RETURN) CASE(RETURN_NIL)
            CASE(MAKE_ARRAY) CASE(MAKE_MAP) CASE(INDEX_GET) CASE(INDEX_SET)
            CASE(ASYNC_CALL) CASE(ASYNC_MODULE) CASE(AWAIT)
            CASE(MAKE_CLOSURE) CASE(FIELD_GET) CASE(FIELD_SET)
            CASE(STRUCT_CREATE) CASE(SPAWN_TASK) CASE(EVAL_AST)
#undef CASE
            default: name = "?"; break;
        }
        std::printf("  %4zu  %-20s  a=%-4d b=%-4d", i, name.c_str(), inst.a, inst.b);
        // 附加注释
        if (inst.op == OpCode::PUSH_CONST && inst.a < (int)constants.size())
            std::cout << "  ; " << constants[inst.a].toString();
        else if ((inst.op==OpCode::LOAD||inst.op==OpCode::STORE||inst.op==OpCode::DEFINE||
                  inst.op==OpCode::LOAD_STATE||inst.op==OpCode::STORE_STATE||
                  inst.op==OpCode::CALL||inst.op==OpCode::CALL_MODULE||
                  inst.op==OpCode::CALL_METHOD) && inst.a < (int)names.size())
            std::cout << "  ; " << names[inst.a];
        std::cout << "\n";
    }
    std::cout << "  (" << code.size() << " instructions, "
              << constants.size() << " constants, "
              << names.size() << " names)\n";
}
