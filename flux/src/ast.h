#pragma once
#include <string>
#include <vector>
#include <memory>

// ── 基类 ──────────────────────────────────────────────────
struct ASTNode { virtual ~ASTNode() = default; };
using NodePtr = std::unique_ptr<ASTNode>;

// ── 表达式节点 ────────────────────────────────────────────
struct NumberLit : ASTNode {
    double value;
    explicit NumberLit(double v) : value(v) {}
};

struct StringLit : ASTNode {
    std::string value;
    explicit StringLit(std::string v) : value(std::move(v)) {}
};

struct BoolLit : ASTNode {
    bool value;
    explicit BoolLit(bool v) : value(v) {}
};

struct NilLit : ASTNode {};

struct Identifier : ASTNode {
    std::string name;
    explicit Identifier(std::string n) : name(std::move(n)) {}
};

struct BinaryExpr : ASTNode {
    std::string op;
    NodePtr     left, right;
    BinaryExpr(std::string op, NodePtr l, NodePtr r)
        : op(std::move(op)), left(std::move(l)), right(std::move(r)) {}
};

struct UnaryExpr : ASTNode {
    std::string op;
    NodePtr     operand;
    UnaryExpr(std::string op, NodePtr o)
        : op(std::move(op)), operand(std::move(o)) {}
};

struct CallExpr : ASTNode {
    std::string          name;
    std::vector<NodePtr> args;
    CallExpr(std::string n, std::vector<NodePtr> a)
        : name(std::move(n)), args(std::move(a)) {}
};

// ── 语句节点 ──────────────────────────────────────────────
struct VarDecl : ASTNode {
    bool        forceOverride = false;  // !var: always overwrite on hot-reload
    bool        isInterface   = false;  // var S: interface = { ... }
    std::string name;
    std::string typeAnnotation; // 可选，如 "Int" "String"
    NodePtr     initializer;
    VarDecl(std::string n, std::string ta, NodePtr init,
            bool fo = false, bool iface = false)
        : forceOverride(fo), isInterface(iface)
        , name(std::move(n))
        , typeAnnotation(std::move(ta)), initializer(std::move(init)) {}
};

struct Assign : ASTNode {
    std::string name;
    NodePtr     value;
    Assign(std::string n, NodePtr v)
        : name(std::move(n)), value(std::move(v)) {}
};

struct ReturnStmt : ASTNode {
    NodePtr value;
    explicit ReturnStmt(NodePtr v) : value(std::move(v)) {}
};

struct IfStmt : ASTNode {
    NodePtr              condition;
    std::vector<NodePtr> thenBlock;
    std::vector<NodePtr> elseBlock;
    IfStmt(NodePtr cond, std::vector<NodePtr> th, std::vector<NodePtr> el)
        : condition(std::move(cond)), thenBlock(std::move(th)), elseBlock(std::move(el)) {}
};

struct WhileStmt : ASTNode {
    NodePtr              condition;
    std::vector<NodePtr> body;
    WhileStmt(NodePtr cond, std::vector<NodePtr> b)
        : condition(std::move(cond)), body(std::move(b)) {}
};

struct ExprStmt : ASTNode {
    NodePtr expr;
    explicit ExprStmt(NodePtr e) : expr(std::move(e)) {}
};

// ── 顶层节点 ──────────────────────────────────────────────
struct Param {
    std::string name;
    std::string type; // 暂时存为字符串，类型系统后续扩展
};

struct FnDecl : ASTNode {
    std::string          name;
    std::vector<Param>   params;
    std::string          returnType;
    std::vector<NodePtr> body;
    bool                 forceOverride = false;  // !func: always replace on hot-reload
    FnDecl(std::string n, std::vector<Param> p, std::string rt, std::vector<NodePtr> b,
           bool fo = false)
        : name(std::move(n)), params(std::move(p)), returnType(std::move(rt)), body(std::move(b))
        , forceOverride(fo) {}
};

struct Program : ASTNode {
    std::vector<NodePtr> statements;
};

// ── persistent { name: defaultValue, ... } ───────────────
struct PersistentField {
    std::string name;
    NodePtr     defaultValue;   // 仅首次运行时使用
};

struct PersistentBlock : ASTNode {
    std::vector<PersistentField> fields;
};

// ── state.fieldName（读取持久状态）────────────────────────
struct StateAccess : ASTNode {
    std::string field;
    explicit StateAccess(std::string f) : field(std::move(f)) {}
};

// ── state.fieldName = value（写入持久状态）────────────────
struct StateAssign : ASTNode {
    std::string field;
    NodePtr     value;
    StateAssign(std::string f, NodePtr v)
        : field(std::move(f)), value(std::move(v)) {}
};

// ── 监督策略 ──────────────────────────────────────────────
enum class RestartPolicy {
    None,       // 无监督（默认）
    Always,     // 崩溃后总是重启
    Never,      // 崩溃后停止，不重启
};

// ── @threadpool(name: "io-pool", size: 4) 声明 ───────────
struct ThreadPoolDecl : ASTNode {
    std::string name;   // pool 名称
    int         size;   // worker 线程数
    explicit ThreadPoolDecl(std::string n, int s) : name(std::move(n)), size(s) {}
};

// ── module MyModule { ... } ───────────────────────────────
// 可附加 @concurrent(pool: ..., queue: ..., overflow: ...)
struct ModuleDecl : ASTNode {
    std::string          name;
    std::vector<NodePtr> body;
    RestartPolicy        restartPolicy = RestartPolicy::None;
    int                  maxRetries    = 3;
    // @concurrent 注解（空 poolName = 无绑定）
    std::string          poolName;
    int                  poolQueue    = 100;
    std::string          poolOverflow = "block";   // "block" | "drop" | "error"

    ModuleDecl(std::string n, std::vector<NodePtr> b,
               RestartPolicy rp = RestartPolicy::None, int mr = 3,
               std::string pn = "", int pq = 100, std::string po = "block")
        : name(std::move(n)), body(std::move(b))
        , restartPolicy(rp), maxRetries(mr)
        , poolName(std::move(pn)), poolQueue(pq), poolOverflow(std::move(po)) {}
};

// ── ModuleName.functionName(args) ─────────────────────────
struct ModuleCall : ASTNode {
    std::string          module;
    std::string          fn;
    std::vector<NodePtr> args;
    ModuleCall(std::string m, std::string f, std::vector<NodePtr> a)
        : module(std::move(m)), fn(std::move(f)), args(std::move(a)) {}
};

// ── migrate { field: value, ... } ────────────────────────
// 当 persistent 新增字段时，必须提供此块，否则热更新被阻断
struct MigrateField {
    std::string name;
    NodePtr     value;
};

struct MigrateBlock : ASTNode {
    std::vector<MigrateField> fields;
};

// ── [1, 2, 3] 数组字面量 ─────────────────────────────────
struct ArrayLit : ASTNode {
    std::vector<NodePtr> elements;
    explicit ArrayLit(std::vector<NodePtr> elems)
        : elements(std::move(elems)) {}
};

// ── arr[i] 下标访问 ──────────────────────────────────────
struct IndexExpr : ASTNode {
    NodePtr object;
    NodePtr index;
    IndexExpr(NodePtr obj, NodePtr idx)
        : object(std::move(obj)), index(std::move(idx)) {}
};

// ── arr[i] = value 下标赋值 ──────────────────────────────
struct IndexAssign : ASTNode {
    NodePtr object;
    NodePtr index;
    NodePtr value;
    IndexAssign(NodePtr obj, NodePtr idx, NodePtr val)
        : object(std::move(obj)), index(std::move(idx)), value(std::move(val)) {}
};

// ── obj.field = value 字段赋值（struct 方法内 self.x = ...）──
struct FieldAssign : ASTNode {
    NodePtr     object;   // 对象表达式（通常是 Identifier "self"）
    std::string field;    // 字段名
    NodePtr     value;
    FieldAssign(NodePtr obj, std::string f, NodePtr val)
        : object(std::move(obj)), field(std::move(f)), value(std::move(val)) {}
};

// ── arr.push(x) / arr.len() 方法调用 ────────────────────
struct MethodCall : ASTNode {
    NodePtr              object;
    std::string          method;
    std::vector<NodePtr> args;
    MethodCall(NodePtr obj, std::string m, std::vector<NodePtr> a)
        : object(std::move(obj)), method(std::move(m)), args(std::move(a)) {}
};

// ── for item in collection { } ───────────────────────────
struct ForIn : ASTNode {
    std::string          var;       // 迭代变量名
    NodePtr              iterable;  // 被迭代的表达式
    std::vector<NodePtr> body;
    ForIn(std::string v, NodePtr it, std::vector<NodePtr> b)
        : var(std::move(v)), iterable(std::move(it)), body(std::move(b)) {}
};

// ── Module.fn.async(args) — 跨 pool 异步调用（Feature K v2）
struct AsyncCall : ASTNode {
    std::string          module;  // 目标模块名
    std::string          fn;      // 函数名
    std::vector<NodePtr> args;    // 实参
    AsyncCall(std::string mod, std::string f, std::vector<NodePtr> a)
        : module(std::move(mod)), fn(std::move(f)), args(std::move(a)) {}
};

// ── async call(args) — 返回 Future 值（Feature K 向后兼容）
struct AsyncExpr : ASTNode {
    NodePtr call;   // CallExpr 或 ModuleCall（被异步执行的调用）
    explicit AsyncExpr(NodePtr c) : call(std::move(c)) {}
};

// ── await future — 等待 Future 完成，返回实际值 ───────────
struct AwaitExpr : ASTNode {
    NodePtr expr;   // Future 类型的表达式
    explicit AwaitExpr(NodePtr e) : expr(std::move(e)) {}
};

// ── spawn { ... } — 后台执行（fire-and-forget）───────────
struct SpawnStmt : ASTNode {
    std::vector<NodePtr> body;
    explicit SpawnStmt(std::vector<NodePtr> b) : body(std::move(b)) {}
};

// ══════════════════════════════════════════════════════════
// Spec v1.0 新节点
// ══════════════════════════════════════════════════════════

// ── 结构体字段定义 { x: 0 } ──────────────────────────────
struct StructFieldDef {
    std::string name;
    NodePtr     defaultValue;  // 可为 null（无默认值）
};

// ── 结构体方法定义 { func distance() { ... } } ────────────
struct StructMethodDef {
    std::string          name;
    std::vector<Param>   params;
    std::string          returnType;
    std::vector<NodePtr> body;
};

// ── 结构体字面量 { field: val, ..., func method() {} } ────
// interfaceName 不为空 → var Circle = Shape { ... }（实现接口）
struct StructLit : ASTNode {
    std::string                  interfaceName;  // 实现的接口名，"" = 匿名
    std::vector<StructFieldDef>  fields;
    std::vector<StructMethodDef> methods;
};

// ── 接口方法签名（只有名称和参数，无函数体）────────────────
struct InterfaceMethodSig {
    std::string        name;
    std::vector<Param> params;
    std::string        returnType;
};

// ── 接口字面量 { func area() func perimeter() } ──────────
struct InterfaceLit : ASTNode {
    std::string                      name;  // 保存到变量的名称（后填充）
    std::vector<InterfaceMethodSig>  methods;
};

// ── 结构体具名参数构造 Point(x: 3, y: 4) ──────────────────
struct StructFieldInit {
    std::string name;
    NodePtr     value;
};

struct StructCreate : ASTNode {
    std::string                   typeName;
    std::vector<StructFieldInit>  fields;
};

// ── 区间范围 [1, 5] / [1, 5) ─────────────────────────────
struct IntervalRange : ASTNode {
    NodePtr start, end;
    bool    inclusive;  // true = [a,b]（闭区间），false = [a,b)（半开区间）
    IntervalRange(NodePtr s, NodePtr e, bool inc)
        : start(std::move(s)), end(std::move(e)), inclusive(inc) {}
};

// ── 匿名函数表达式 func(params) { body } ─────────────────
struct FuncExpr : ASTNode {
    std::vector<Param>   params;
    std::string          returnType;
    std::vector<NodePtr> body;
    FuncExpr(std::vector<Param> p, std::string rt, std::vector<NodePtr> b)
        : params(std::move(p)), returnType(std::move(rt)), body(std::move(b)) {}
};

// ── exception 错误描述 ────────────────────────────────────
// 顶层：exception divide { "描述" }   target = "divide"
// 方法：exception Point:move { "描述" } target = "Point:move"
// 内联：exception { "描述" }           target = ""（函数体内部）
struct ExceptionDecl : ASTNode {
    std::string              target;    // "" = 内联, "fn" = 全局, "Type:method" = 方法
    std::vector<std::string> messages;
};

// ══════════════════════════════════════════════════════════
// Phase 5-7 新节点
// ══════════════════════════════════════════════════════════

// ── @profile 装饰的函数声明 ─────────────────────────────
struct ProfiledFnDecl : ASTNode {
    NodePtr fnDecl;   // 包装的 FnDecl
    explicit ProfiledFnDecl(NodePtr fn) : fnDecl(std::move(fn)) {}
};

// ── @platform(target) 声明 ──────────────────────────────
struct PlatformDecl : ASTNode {
    std::string target;     // "arm64" | "riscv64" | "esp32" | "generic" ...
    std::vector<NodePtr> body;
    PlatformDecl(std::string t, std::vector<NodePtr> b)
        : target(std::move(t)), body(std::move(b)) {}
};

// ── enum 枚举定义 ───────────────────────────────────────
// enum Color { Red, Green, Blue }
// enum Status { Ok = 0, Error = 1 }
struct EnumVariant {
    std::string name;
    NodePtr     value;    // 可为 null（自动递增）
};

struct EnumDecl : ASTNode {
    std::string name;
    std::vector<EnumVariant> variants;
};

// ── append 扩展 ─────────────────────────────────────────
// append Array { func sum() { ... } }
struct AppendDecl : ASTNode {
    std::string typeName;       // 被扩展的类型名
    std::vector<StructMethodDef> methods;
};

// ── alloc(size) 表达式 ──────────────────────────────────
struct AllocExpr : ASTNode {
    NodePtr size;
    explicit AllocExpr(NodePtr s) : size(std::move(s)) {}
};

// ── free(ptr) 语句 ──────────────────────────────────────
struct FreeStmt : ASTNode {
    NodePtr ptr;
    explicit FreeStmt(NodePtr p) : ptr(std::move(p)) {}
};

// ── asm { "指令" } 内联汇编 ─────────────────────────────
struct AsmBlock : ASTNode {
    std::vector<std::string> instructions;
};

// ── default { value } 语句级默认值返回 ──────────────────
// 写在 if 条件分支中，与 exception {} 配合使用
// 语义：评估块中最后一个表达式，作为函数返回值
struct DefaultStmt : ASTNode {
    std::vector<NodePtr> body;   // 默认值块
    explicit DefaultStmt(std::vector<NodePtr> b) : body(std::move(b)) {}
};
