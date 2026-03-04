// mir.h — Flux Mid-level Intermediate Representation
// HIR → MIR: 控制流图 + SSA 形式 + 优化 Pass
#pragma once
#include "hir.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

// ═══════════════════════════════════════════════════════════
// MIR 值 — SSA 形式的虚拟寄存器
// ═══════════════════════════════════════════════════════════
struct MIRValue {
    int id;          // 唯一 SSA 编号 (%0, %1, ...)
    HIRType type;    // 类型（从 HIR 继承）
};

// ═══════════════════════════════════════════════════════════
// MIR 指令
// ═══════════════════════════════════════════════════════════
enum class MIROp {
    // Constants
    Const,          // %r = const <value>
    // Arithmetic
    Add, Sub, Mul, Div, Mod,
    // Comparison
    Eq, Ne, Lt, Le, Gt, Ge,
    // Logical
    And, Or, Not, Negate,
    // Memory / Variables
    Load,           // %r = load <name>
    Store,          // store %val → <name>
    // Control Flow
    Branch,         // branch %cond → bb_true, bb_false
    Jump,           // jump → bb_target
    Return,         // return %val
    // Function
    Call,           // %r = call <name>(%args...)
    AsyncCall,      // %r = async_call <name>(%args...)
    Await,          // %r = await %future
    // Data structures
    MakeArray,      // %r = make_array [%elems...]
    MakeMap,        // %r = make_map {%keys: %vals}
    IndexGet,       // %r = index_get %obj, %idx
    IndexSet,       // index_set %obj, %idx, %val
    FieldGet,       // %r = field_get %obj, <field>
    FieldSet,       // field_set %obj, <field>, %val
    // Struct
    StructCreate,   // %r = struct_create <type> {fields...}
    MethodCall,     // %r = method_call %obj.<method>(%args...)
    // Phi
    Phi,            // %r = phi [bb1: %v1, bb2: %v2]
};

struct MIRInst {
    MIROp op;
    int dest = -1;              // 目标寄存器 (-1 = 无返回值)
    std::vector<int> operands;  // 操作数寄存器
    std::string name;           // 变量名/函数名/字段名
    double constNum = 0;
    std::string constStr;
    bool constBool = false;
    int targetBlock = -1;       // Branch/Jump 目标
    int falseBlock = -1;        // Branch false 目标
    // Phi node sources: (blockId, valueId) pairs
    std::vector<std::pair<int, int>> phiSources;
};

// ═══════════════════════════════════════════════════════════
// 基本块
// ═══════════════════════════════════════════════════════════
struct BasicBlock {
    int id;
    std::string label;
    std::vector<MIRInst> instructions;
    std::vector<int> predecessors;
    std::vector<int> successors;

    bool isTerminated() const {
        if (instructions.empty()) return false;
        auto& last = instructions.back();
        return last.op == MIROp::Branch || last.op == MIROp::Jump ||
               last.op == MIROp::Return;
    }
};

// ═══════════════════════════════════════════════════════════
// MIR 函数
// ═══════════════════════════════════════════════════════════
struct MIRFunction {
    std::string name;
    std::vector<std::pair<std::string, HIRType>> params;
    HIRType returnType;
    std::vector<BasicBlock> blocks;
    int nextValueId = 0;
    int nextBlockId = 0;

    int newValue() { return nextValueId++; }

    int addBlock(const std::string& label = "") {
        int id = nextBlockId++;
        blocks.push_back({id, label.empty() ? "bb" + std::to_string(id) : label, {}, {}, {}});
        return id;
    }

    BasicBlock& getBlock(int id) {
        for (auto& b : blocks) if (b.id == id) return b;
        throw std::runtime_error("MIR: block not found: " + std::to_string(id));
    }

    void addEdge(int from, int to) {
        getBlock(from).successors.push_back(to);
        getBlock(to).predecessors.push_back(from);
    }
};

// ═══════════════════════════════════════════════════════════
// MIR Program
// ═══════════════════════════════════════════════════════════
struct MIRProgram {
    std::vector<MIRFunction> functions;
    MIRFunction* mainFn = nullptr;  // 顶层语句编译到 main
};

// ═══════════════════════════════════════════════════════════
// MIR Builder — HIR → MIR 转换
// ═══════════════════════════════════════════════════════════
class MIRBuilder {
public:
    MIRProgram build(const HIRProgram& hir) {
        MIRProgram prog;

        // 预分配足够空间避免 vector reallocation 使指针失效
        prog.functions.reserve(hir.decls.size() + 4);

        // 创建 main 函数用于顶层语句
        prog.functions.push_back({"__main__", {}, HIRType::make(HIRType::Nil), {}, 0, 0});
        prog.mainFn = &prog.functions[0];
        currentFn_ = prog.mainFn;
        currentBlock_ = currentFn_->addBlock("entry");

        for (auto& decl : hir.decls) {
            if (auto* fn = dynamic_cast<HIRFnDecl*>(decl.get())) {
                buildFunction(prog, fn);
                // 恢复 mainFn 指针（push_back 后可能失效）
                prog.mainFn = &prog.functions[0];
                currentFn_ = prog.mainFn;
            } else {
                buildNode(decl.get());
            }
        }

        // Terminate main if not already
        auto& mainBlock = currentFn_->getBlock(currentBlock_);
        if (!mainBlock.isTerminated()) {
            MIRInst ret;
            ret.op = MIROp::Return;
            ret.dest = -1;
            mainBlock.instructions.push_back(ret);
        }

        return prog;
    }

private:
    MIRFunction* currentFn_ = nullptr;
    int currentBlock_ = 0;

    void buildFunction(MIRProgram& prog, HIRFnDecl* fn) {
        MIRFunction mirFn;
        mirFn.name = fn->name;
        for (auto& p : fn->params)
            mirFn.params.push_back({p.name, p.type});
        mirFn.returnType = fn->returnType;

        auto* savedFn = currentFn_;
        int savedBlock = currentBlock_;

        prog.functions.push_back(std::move(mirFn));
        currentFn_ = &prog.functions.back();
        currentBlock_ = currentFn_->addBlock("entry");

        for (auto& s : fn->body)
            buildNode(s.get());

        // Ensure termination
        auto& block = currentFn_->getBlock(currentBlock_);
        if (!block.isTerminated()) {
            MIRInst ret;
            ret.op = MIROp::Return;
            ret.dest = -1;
            block.instructions.push_back(ret);
        }

        currentFn_ = savedFn;
        currentBlock_ = savedBlock;
    }

    int buildNode(HIRNode* node) {
        if (!node) return -1;

        if (auto* n = dynamic_cast<HIRLiteral*>(node)) {
            int r = currentFn_->newValue();
            MIRInst inst;
            inst.op = MIROp::Const;
            inst.dest = r;
            inst.constNum = n->numVal;
            inst.constStr = n->strVal;
            inst.constBool = n->boolVal;
            emit(inst);
            return r;
        }

        if (auto* n = dynamic_cast<HIRVarRef*>(node)) {
            int r = currentFn_->newValue();
            MIRInst inst;
            inst.op = MIROp::Load;
            inst.dest = r;
            inst.name = n->name;
            emit(inst);
            return r;
        }

        if (auto* n = dynamic_cast<HIRBinary*>(node)) {
            int l = buildNode(n->left.get());
            int r = buildNode(n->right.get());
            int d = currentFn_->newValue();
            MIRInst inst;
            inst.dest = d;
            inst.operands = {l, r};
            if (n->op == "+")       inst.op = MIROp::Add;
            else if (n->op == "-")  inst.op = MIROp::Sub;
            else if (n->op == "*")  inst.op = MIROp::Mul;
            else if (n->op == "/")  inst.op = MIROp::Div;
            else if (n->op == "%")  inst.op = MIROp::Mod;
            else if (n->op == "==") inst.op = MIROp::Eq;
            else if (n->op == "!=") inst.op = MIROp::Ne;
            else if (n->op == "<")  inst.op = MIROp::Lt;
            else if (n->op == "<=") inst.op = MIROp::Le;
            else if (n->op == ">")  inst.op = MIROp::Gt;
            else if (n->op == ">=") inst.op = MIROp::Ge;
            else if (n->op == "&&") inst.op = MIROp::And;
            else if (n->op == "||") inst.op = MIROp::Or;
            else inst.op = MIROp::Add;  // fallback
            emit(inst);
            return d;
        }

        if (auto* n = dynamic_cast<HIRVarDecl*>(node)) {
            int val = n->init ? buildNode(n->init.get()) : -1;
            MIRInst inst;
            inst.op = MIROp::Store;
            inst.name = n->name;
            if (val >= 0) inst.operands = {val};
            emit(inst);
            return -1;
        }

        if (auto* n = dynamic_cast<HIRAssign*>(node)) {
            int val = buildNode(n->value.get());
            MIRInst inst;
            inst.op = MIROp::Store;
            inst.name = n->name;
            inst.operands = {val};
            emit(inst);
            return -1;
        }

        if (auto* n = dynamic_cast<HIRReturn*>(node)) {
            int val = n->value ? buildNode(n->value.get()) : -1;
            MIRInst inst;
            inst.op = MIROp::Return;
            if (val >= 0) inst.operands = {val};
            emit(inst);
            return -1;
        }

        if (auto* n = dynamic_cast<HIRIf*>(node)) {
            int cond = buildNode(n->condition.get());
            int thenBB = currentFn_->addBlock("if.then");
            int elseBB = currentFn_->addBlock("if.else");
            int mergeBB = currentFn_->addBlock("if.merge");

            MIRInst br;
            br.op = MIROp::Branch;
            br.operands = {cond};
            br.targetBlock = thenBB;
            br.falseBlock = elseBB;
            emit(br);
            currentFn_->addEdge(currentBlock_, thenBB);
            currentFn_->addEdge(currentBlock_, elseBB);

            // Then
            currentBlock_ = thenBB;
            for (auto& s : n->thenBranch) buildNode(s.get());
            if (!currentFn_->getBlock(currentBlock_).isTerminated()) {
                MIRInst jmp; jmp.op = MIROp::Jump; jmp.targetBlock = mergeBB;
                emit(jmp);
                currentFn_->addEdge(currentBlock_, mergeBB);
            }

            // Else
            currentBlock_ = elseBB;
            for (auto& s : n->elseBranch) buildNode(s.get());
            if (!currentFn_->getBlock(currentBlock_).isTerminated()) {
                MIRInst jmp; jmp.op = MIROp::Jump; jmp.targetBlock = mergeBB;
                emit(jmp);
                currentFn_->addEdge(currentBlock_, mergeBB);
            }

            currentBlock_ = mergeBB;
            return -1;
        }

        if (auto* n = dynamic_cast<HIRWhile*>(node)) {
            int condBB = currentFn_->addBlock("while.cond");
            int bodyBB = currentFn_->addBlock("while.body");
            int exitBB = currentFn_->addBlock("while.exit");

            MIRInst jmp; jmp.op = MIROp::Jump; jmp.targetBlock = condBB;
            emit(jmp);
            currentFn_->addEdge(currentBlock_, condBB);

            currentBlock_ = condBB;
            int cond = buildNode(n->condition.get());
            MIRInst br;
            br.op = MIROp::Branch;
            br.operands = {cond};
            br.targetBlock = bodyBB;
            br.falseBlock = exitBB;
            emit(br);
            currentFn_->addEdge(condBB, bodyBB);
            currentFn_->addEdge(condBB, exitBB);

            currentBlock_ = bodyBB;
            for (auto& s : n->body) buildNode(s.get());
            if (!currentFn_->getBlock(currentBlock_).isTerminated()) {
                MIRInst back; back.op = MIROp::Jump; back.targetBlock = condBB;
                emit(back);
                currentFn_->addEdge(currentBlock_, condBB);
            }

            currentBlock_ = exitBB;
            return -1;
        }

        if (auto* n = dynamic_cast<HIRCall*>(node)) {
            std::vector<int> argRegs;
            for (auto& a : n->args) argRegs.push_back(buildNode(a.get()));
            int r = currentFn_->newValue();
            MIRInst inst;
            inst.op = n->isAsync ? MIROp::AsyncCall : MIROp::Call;
            inst.dest = r;
            inst.operands = argRegs;
            if (auto* ref = dynamic_cast<HIRVarRef*>(n->callee.get()))
                inst.name = ref->name;
            emit(inst);
            return r;
        }

        if (auto* n = dynamic_cast<HIRAwait*>(node)) {
            int val = buildNode(n->expr.get());
            int r = currentFn_->newValue();
            MIRInst inst;
            inst.op = MIROp::Await;
            inst.dest = r;
            inst.operands = {val};
            emit(inst);
            return r;
        }

        return -1;
    }

    void emit(MIRInst inst) {
        currentFn_->getBlock(currentBlock_).instructions.push_back(std::move(inst));
    }
};

// ═══════════════════════════════════════════════════════════
// MIR Optimization Passes
// ═══════════════════════════════════════════════════════════

// 常量折叠 Pass
inline void mirPassConstantFold(MIRFunction& fn) {
    std::unordered_map<int, double> constValues;  // valueId → const value

    for (auto& bb : fn.blocks) {
        for (auto& inst : bb.instructions) {
            if (inst.op == MIROp::Const && inst.dest >= 0) {
                constValues[inst.dest] = inst.constNum;
            }
            // Fold binary ops on constants
            if ((inst.op >= MIROp::Add && inst.op <= MIROp::Mod) &&
                inst.operands.size() == 2) {
                auto itL = constValues.find(inst.operands[0]);
                auto itR = constValues.find(inst.operands[1]);
                if (itL != constValues.end() && itR != constValues.end()) {
                    double l = itL->second, r = itR->second, result = 0;
                    switch (inst.op) {
                    case MIROp::Add: result = l + r; break;
                    case MIROp::Sub: result = l - r; break;
                    case MIROp::Mul: result = l * r; break;
                    case MIROp::Div: result = r != 0 ? l / r : 0; break;
                    case MIROp::Mod: result = r != 0 ? (int)l % (int)r : 0; break;
                    default: continue;
                    }
                    inst.op = MIROp::Const;
                    inst.constNum = result;
                    inst.operands.clear();
                    constValues[inst.dest] = result;
                }
            }
        }
    }
}

// 死代码消除 Pass
inline void mirPassDeadCodeElim(MIRFunction& fn) {
    // Collect used values
    std::unordered_set<int> used;
    for (auto& bb : fn.blocks) {
        for (auto& inst : bb.instructions) {
            for (int op : inst.operands) used.insert(op);
        }
    }

    // Remove instructions whose results are never used
    for (auto& bb : fn.blocks) {
        bb.instructions.erase(
            std::remove_if(bb.instructions.begin(), bb.instructions.end(),
                [&](const MIRInst& inst) {
                    // Don't remove side-effectful instructions
                    if (inst.op == MIROp::Store || inst.op == MIROp::Call ||
                        inst.op == MIROp::AsyncCall || inst.op == MIROp::Return ||
                        inst.op == MIROp::Branch || inst.op == MIROp::Jump ||
                        inst.op == MIROp::IndexSet || inst.op == MIROp::FieldSet)
                        return false;
                    return inst.dest >= 0 && used.find(inst.dest) == used.end();
                }),
            bb.instructions.end());
    }
}

// 不可达块消除 Pass
inline void mirPassUnreachableElim(MIRFunction& fn) {
    if (fn.blocks.empty()) return;

    // BFS from entry
    std::unordered_set<int> reachable;
    std::vector<int> queue = {fn.blocks[0].id};
    reachable.insert(fn.blocks[0].id);

    while (!queue.empty()) {
        int bid = queue.back(); queue.pop_back();
        for (int succ : fn.getBlock(bid).successors) {
            if (reachable.insert(succ).second)
                queue.push_back(succ);
        }
    }

    fn.blocks.erase(
        std::remove_if(fn.blocks.begin(), fn.blocks.end(),
            [&](const BasicBlock& bb) { return !reachable.count(bb.id); }),
        fn.blocks.end());
}

// 运行所有优化 Pass
inline void mirOptimize(MIRProgram& prog) {
    for (auto& fn : prog.functions) {
        mirPassConstantFold(fn);
        mirPassDeadCodeElim(fn);
        mirPassUnreachableElim(fn);
    }
}
