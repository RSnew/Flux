// fluz.h — Flux 字节码打包格式 (.fluz)
// 二进制格式：序列化 Chunk + 常量池 + 名字池 + AST 节点引用
#pragma once
#include "vm.h"
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════
// .fluz 文件头
// ═══════════════════════════════════════════════════════════
// Magic: "FLUZ" (4 bytes)
// Version: uint32_t
// Flags: uint32_t (0 = normal, 1 = stripped tests)
// NumFunctions: uint32_t
// Each function unit:
//   Name length: uint32_t + Name bytes
//   Version hash: uint64_t (FNV-1a of source)
//   Instruction count: uint32_t
//   Constants count: uint32_t
//   Names count: uint32_t
//   Instructions: [op(1) + a(4) + b(4)] * count
//   Constants: type(1) + data
//   Names: len(4) + bytes

static constexpr uint32_t FLUZ_MAGIC   = 0x5A554C46;  // "FLUZ" little-endian
static constexpr uint32_t FLUZ_VERSION = 1;

// ═══════════════════════════════════════════════════════════
// 函数单元（编译后的可替换单元）
// ═══════════════════════════════════════════════════════════
struct FluzUnit {
    std::string              name;        // 函数名 ("__main__" = 顶层)
    uint64_t                 versionHash; // 源码 FNV-1a 哈希
    std::vector<Instruction> code;
    std::vector<Value>       constants;
    std::vector<std::string> names;
};

struct FluzPackage {
    uint32_t version = FLUZ_VERSION;
    uint32_t flags   = 0;
    std::vector<FluzUnit> units;
};

// ═══════════════════════════════════════════════════════════
// FNV-1a 哈希（与 flux dev 使用的相同算法）
// ═══════════════════════════════════════════════════════════
inline uint64_t fnvHash(const std::string& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ═══════════════════════════════════════════════════════════
// 序列化 / 反序列化
// ═══════════════════════════════════════════════════════════

// 写入字节
inline void writeU8(std::ofstream& f, uint8_t v) { f.write((char*)&v, 1); }
inline void writeU32(std::ofstream& f, uint32_t v) { f.write((char*)&v, 4); }
inline void writeU64(std::ofstream& f, uint64_t v) { f.write((char*)&v, 8); }
inline void writeF64(std::ofstream& f, double v)   { f.write((char*)&v, 8); }
inline void writeStr(std::ofstream& f, const std::string& s) {
    writeU32(f, (uint32_t)s.size());
    f.write(s.data(), s.size());
}

// 读取字节
inline uint8_t readU8(std::ifstream& f) { uint8_t v; f.read((char*)&v, 1); return v; }
inline uint32_t readU32(std::ifstream& f) { uint32_t v; f.read((char*)&v, 4); return v; }
inline uint64_t readU64(std::ifstream& f) { uint64_t v; f.read((char*)&v, 8); return v; }
inline double readF64(std::ifstream& f) { double v; f.read((char*)&v, 8); return v; }
inline std::string readStr(std::ifstream& f) {
    uint32_t len = readU32(f);
    std::string s(len, '\0');
    f.read(s.data(), len);
    return s;
}

// 写入常量值
inline void writeValue(std::ofstream& f, const Value& v) {
    writeU8(f, (uint8_t)v.type);
    switch (v.type) {
    case Value::Type::Number:  writeF64(f, v.number);  break;
    case Value::Type::String:  writeStr(f, v.string);  break;
    case Value::Type::Bool:    writeU8(f, v.boolean ? 1 : 0); break;
    case Value::Type::Nil:     break;
    default:  break;  // 复杂类型不序列化（运行时重建）
    }
}

// 读取常量值
inline Value readValue(std::ifstream& f) {
    auto type = (Value::Type)readU8(f);
    switch (type) {
    case Value::Type::Number:  return Value::Num(readF64(f));
    case Value::Type::String:  return Value::Str(readStr(f));
    case Value::Type::Bool:    return Value::Bool(readU8(f) != 0);
    case Value::Type::Nil:     return Value::Nil();
    default: return Value::Nil();
    }
}

// ── 序列化完整 .fluz 包 ───────────────────────────────────
inline void writeFluz(const std::string& path, const FluzPackage& pkg) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot create file: " + path);

    writeU32(f, FLUZ_MAGIC);
    writeU32(f, pkg.version);
    writeU32(f, pkg.flags);
    writeU32(f, (uint32_t)pkg.units.size());

    for (auto& unit : pkg.units) {
        writeStr(f, unit.name);
        writeU64(f, unit.versionHash);
        writeU32(f, (uint32_t)unit.code.size());
        writeU32(f, (uint32_t)unit.constants.size());
        writeU32(f, (uint32_t)unit.names.size());

        // Instructions
        for (auto& inst : unit.code) {
            writeU8(f, (uint8_t)inst.op);
            writeU32(f, (uint32_t)inst.a);
            writeU32(f, (uint32_t)inst.b);
        }

        // Constants
        for (auto& c : unit.constants) writeValue(f, c);

        // Names
        for (auto& n : unit.names) writeStr(f, n);
    }
}

// ── 反序列化 .fluz 包 ────────────────────────────────────
inline FluzPackage readFluz(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);

    uint32_t magic = readU32(f);
    if (magic != FLUZ_MAGIC)
        throw std::runtime_error("invalid .fluz file: bad magic");

    FluzPackage pkg;
    pkg.version = readU32(f);
    pkg.flags   = readU32(f);
    uint32_t numUnits = readU32(f);

    for (uint32_t u = 0; u < numUnits; ++u) {
        FluzUnit unit;
        unit.name        = readStr(f);
        unit.versionHash = readU64(f);
        uint32_t numInsts  = readU32(f);
        uint32_t numConsts = readU32(f);
        uint32_t numNames  = readU32(f);

        // Instructions
        unit.code.resize(numInsts);
        for (uint32_t i = 0; i < numInsts; ++i) {
            unit.code[i].op = (OpCode)readU8(f);
            unit.code[i].a  = (int32_t)readU32(f);
            unit.code[i].b  = (int32_t)readU32(f);
        }

        // Constants
        unit.constants.resize(numConsts);
        for (uint32_t i = 0; i < numConsts; ++i)
            unit.constants[i] = readValue(f);

        // Names
        unit.names.resize(numNames);
        for (uint32_t i = 0; i < numNames; ++i)
            unit.names[i] = readStr(f);

        pkg.units.push_back(std::move(unit));
    }

    return pkg;
}

// ═══════════════════════════════════════════════════════════
// MIR → FluzUnit 编译（将 MIR 函数编译为 VM 字节码单元）
// ═══════════════════════════════════════════════════════════
class FluzCodeGen {
public:
    FluzPackage generate(const MIRProgram& prog, const std::string& source) {
        FluzPackage pkg;
        pkg.version = FLUZ_VERSION;

        for (auto& fn : prog.functions) {
            FluzUnit unit;
            unit.name = fn.name;
            unit.versionHash = fnvHash(source + ":" + fn.name);
            compileMIRFunction(fn, unit);
            pkg.units.push_back(std::move(unit));
        }

        return pkg;
    }

private:
    void compileMIRFunction(const MIRFunction& fn, FluzUnit& unit) {
        // Map basic block IDs to instruction offsets (forward-patching)
        std::unordered_map<int, int> blockOffsets;
        std::vector<std::pair<int, int>> patchList;  // (instruction_idx, target_block_id)
        std::vector<std::pair<int, int>> falsePatchList;

        // Map param names to indices
        std::unordered_map<std::string, int> paramMap;
        for (int i = 0; i < (int)fn.params.size(); i++)
            paramMap[fn.params[i].first] = i;

        // First pass: record block offsets
        // (approximate — we'll do it properly in one pass with patching)

        for (auto& bb : fn.blocks) {
            blockOffsets[bb.id] = (int)unit.code.size();

            for (auto& inst : bb.instructions) {
                switch (inst.op) {
                case MIROp::Const: {
                    int ci = (int)unit.constants.size();
                    unit.constants.push_back(Value::Num(inst.constNum));
                    unit.code.push_back({OpCode::PUSH_CONST, ci, 0});
                    break;
                }
                case MIROp::Add: emitBinOp(unit, OpCode::ADD); break;
                case MIROp::Sub: emitBinOp(unit, OpCode::SUB); break;
                case MIROp::Mul: emitBinOp(unit, OpCode::MUL); break;
                case MIROp::Div: emitBinOp(unit, OpCode::DIV); break;
                case MIROp::Mod: emitBinOp(unit, OpCode::MOD); break;

                case MIROp::Eq:  emitBinOp(unit, OpCode::EQ);  break;
                case MIROp::Ne:  emitBinOp(unit, OpCode::NEQ); break;
                case MIROp::Lt:  emitBinOp(unit, OpCode::LT);  break;
                case MIROp::Le:  emitBinOp(unit, OpCode::LEQ); break;
                case MIROp::Gt:  emitBinOp(unit, OpCode::GT);  break;
                case MIROp::Ge:  emitBinOp(unit, OpCode::GEQ); break;

                case MIROp::Not:    unit.code.push_back({OpCode::NOT, 0, 0}); break;
                case MIROp::Negate: unit.code.push_back({OpCode::NEG, 0, 0}); break;

                case MIROp::Load: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::LOAD, ni, 0});
                    break;
                }
                case MIROp::Store: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::DEFINE, ni, 0});
                    break;
                }

                case MIROp::Call: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::CALL, ni, (int32_t)inst.operands.size()});
                    break;
                }
                case MIROp::AsyncCall: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::ASYNC_CALL, ni, (int32_t)inst.operands.size()});
                    break;
                }
                case MIROp::Await:
                    unit.code.push_back({OpCode::AWAIT, 0, 0});
                    break;

                case MIROp::MakeArray:
                    unit.code.push_back({OpCode::MAKE_ARRAY, 0, (int32_t)inst.operands.size()});
                    break;
                case MIROp::MakeMap:
                    unit.code.push_back({OpCode::MAKE_MAP, 0, (int32_t)inst.operands.size() / 2});
                    break;
                case MIROp::IndexGet:
                    unit.code.push_back({OpCode::INDEX_GET, 0, 0});
                    break;
                case MIROp::IndexSet:
                    unit.code.push_back({OpCode::INDEX_SET, 0, 0});
                    break;

                case MIROp::FieldGet: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::FIELD_GET, ni, 0});
                    break;
                }
                case MIROp::FieldSet: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::FIELD_SET, ni, 0});
                    break;
                }

                case MIROp::StructCreate: {
                    int ni = addName(unit, inst.name);
                    unit.code.push_back({OpCode::STRUCT_CREATE, ni, (int32_t)inst.operands.size()});
                    break;
                }
                case MIROp::MethodCall: {
                    int ni = addName(unit, inst.name);
                    // operands[0] = object, rest = args
                    int argc = inst.operands.size() > 0 ? (int)inst.operands.size() - 1 : 0;
                    unit.code.push_back({OpCode::CALL_METHOD, ni, argc});
                    break;
                }

                case MIROp::Branch: {
                    // Branch %cond → true_block, false_block
                    int idx = (int)unit.code.size();
                    unit.code.push_back({OpCode::JUMP_IF_FALSE, 0, 0});
                    falsePatchList.push_back({idx, inst.falseBlock});
                    int jidx = (int)unit.code.size();
                    unit.code.push_back({OpCode::JUMP, 0, 0});
                    patchList.push_back({jidx, inst.targetBlock});
                    break;
                }
                case MIROp::Jump: {
                    int idx = (int)unit.code.size();
                    unit.code.push_back({OpCode::JUMP, 0, 0});
                    patchList.push_back({idx, inst.targetBlock});
                    break;
                }
                case MIROp::Return:
                    if (!inst.operands.empty())
                        unit.code.push_back({OpCode::RETURN, 0, 0});
                    else
                        unit.code.push_back({OpCode::RETURN_NIL, 0, 0});
                    break;

                case MIROp::Phi:
                    // Phi nodes are resolved during SSA destruction; skip for now
                    break;

                default: break;
                }
            }
        }

        // Patch jump targets
        for (auto& [idx, blockId] : patchList) {
            auto it = blockOffsets.find(blockId);
            if (it != blockOffsets.end())
                unit.code[idx].a = it->second;
        }
        for (auto& [idx, blockId] : falsePatchList) {
            auto it = blockOffsets.find(blockId);
            if (it != blockOffsets.end())
                unit.code[idx].a = it->second;
        }

        // Ensure termination
        if (unit.code.empty() || unit.code.back().op != OpCode::RETURN_NIL)
            unit.code.push_back({OpCode::RETURN_NIL, 0, 0});
    }

    void emitBinOp(FluzUnit& unit, OpCode op) {
        unit.code.push_back({op, 0, 0});
    }

    int addName(FluzUnit& unit, const std::string& name) {
        for (int i = 0; i < (int)unit.names.size(); i++)
            if (unit.names[i] == name) return i;
        unit.names.push_back(name);
        return (int)unit.names.size() - 1;
    }
};
