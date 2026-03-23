# Flux 语言

一门以热重载为核心的编程语言，支持持久化状态、模块隔离、
监督式崩溃恢复，以及可选的字节码虚拟机。

使用 **约 5,000 行 C++17** 构建，除 pthreads 外无任何外部依赖。

> **Flux 不是脚本语言。**
> 它拥有完整的 HM 类型推断系统、字节码编译器与栈式虚拟机、
> 多层 IR 优化管线（HIR → MIR → JIT/Codegen）、模块隔离与监督式崩溃恢复，
> 以及完整的工具链（类型检查器、格式化器、包管理器、LSP、调试器）。
> 热重载是设计选择，而非能力上限。

---

## 快速开始

```bash
# 构建
cd flux && mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)

# 运行脚本（带热重载文件监听）
./flux examples/hello.flux

# 单次运行（不启动监听）
./flux run examples/hello.flux

# 交互式 REPL
./flux
```

## 你好，Flux

```flux
var name = "World"
print("Hello, \(name)!")

func factorial(n) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
print("10! = \(factorial(10))")
```

## 语言亮点

**变量、常量与类型**
```flux
var x = 42                    // 自动推断
var name: String = "Flux"     // 类型标注
conf MAX = 100                // 常量（运行时只读）
var nums = [1, 2, 3, 4, 5]   // 数组
var m = Map()                 // 哈希表
```

**枚举（仅允许自然数值）**
```flux
enum Direction {
    North = 0,
    South = 1,
    East = 2,
    West = 3
}
var heading = Direction.North
```

**结构体与接口**
```flux
var Shape: interface = { func area() }

var Circle = Shape {
    radius: 1,
    func area() { return 3.14159 * self.radius * self.radius }
}

var c = Circle(radius: 5)
print(c.area())   // 78.54
```

**带持久化状态的模块** — 状态在热重载后依然保留
```flux
module Counter {
    persistent { count: 0 }
    func increment() { state.count = state.count + 1 }
    func getValue()  { return state.count }
}

Counter.increment()
print(Counter.getValue())
```

**监督式崩溃恢复**
```flux
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    func charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}
```

**线程池并发**
```flux
@threadpool(name: "cpu-pool", size: 4)

@concurrent(pool: "cpu-pool")
module ImageProcessor {
    func resize(w, h) { return w * h }
}

var future = ImageProcessor.resize.async(1920, 1080)
var result = future.await()
```

**数组与拼接**
```flux
var a = [1, 2, 3]
var b = [4, 5, 6]
var c = a + b              // [1, 2, 3, 4, 5, 6]
a.push(99)                 // [1, 2, 3, 99]
print(a.len())             // 4
```

**AI 原生类型（`specify`）**
```flux
var validator = specify {
    intent: "验证用户支付数据的合法性",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "currency in [USD, EUR, CNY]"],
    examples: ["amount=100 currency=USD -> true"]
}

Specify.describe(validator)   // 人类可读摘要
Specify.schema(validator)     // 结构化 Map
Specify.validate(validator, input)  // true/false
```

**标准库**
```flux
File.write("/tmp/out.txt", "hello\n")
var content = File.read("/tmp/out.txt")

var obj = Json.parse("{\"x\": 1}")
print(Json.pretty(obj))

Http.download("https://example.com/file.bin", "/tmp/file.bin")

var t0 = Time.now()
Time.sleep(100)
print("elapsed: \(Time.diff(t0, Time.now())) ms")
```

## 工具链

| 命令 | 描述 |
|------|------|
| `flux <file>` | 以热重载模式运行（文件监听） |
| `flux dev <file>` | 开发模式 — 热重载 + 文件监听 |
| `flux run <file>` | 单次运行 |
| `flux --vm <file>` | 使用字节码虚拟机运行 |
| `flux check <file>` | 仅进行类型检查 |
| `flux fmt <file>` | 格式化源码并输出到标准输出 |
| `flux fmt -w <file>` | 原地格式化 |
| `flux repl` | 支持多行与历史记录的 REPL |
| `flux compile <file>` | 编译为原生二进制文件（x86_64、arm64、riscv64） |
| `flux profile <file>` | 以性能分析模式运行所有函数 |

### 包管理器

```bash
flux new myapp          # 创建项目并生成 flux.toml
flux add mathlib        # 添加依赖
flux install            # 安装所有依赖
flux build              # 构建并运行
flux publish            # 发布到本地仓库
```

### AI 友好工具链

Flux 为 AI Agent 工作流提供一等支持，所有输出均可结构化：

| 命令 | 描述 |
|------|------|
| `flux check <file> --json` | 类型检查，JSON 格式输出错误 |
| `flux inspect <file>` | 列出符号、签名和合约（人类可读） |
| `flux inspect <file> --json` | 同上，JSON 格式 |
| `flux eval "<code>" --json` | 执行代码片段，返回 JSON 结果 |

**`specify` — 结构化规格声明**

将意图、约束和示例声明为一等值：

```flux
var paymentValidator = specify {
    intent: "验证用户支付数据的合法性",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "currency in [USD, EUR, CNY]"],
    examples: ["amount=100 currency=USD -> true"]
}

Specify.describe(paymentValidator)  // 人类可读摘要
Specify.schema(paymentValidator)    // 结构化 Map
Specify.intent(paymentValidator)    // → "验证用户支付数据的合法性"
Specify.validate(paymentValidator, input)  // → true/false
```

**合约设计 — `requires` / `ensures`**

```flux
func divide(a, b)
requires { b != 0 }
ensures  { result != null }
{
    return a / b
}
```

AI Agent 可通过 `flux inspect --json` 发现合约，无需解析源码。

## 功能状态

| 编号 | 功能 | 描述 | 状态 |
|------|------|------|------|
| H  | 语言核心 | 数组、字符串插值 `\()`、for-in、方法调用 | 已完成 |
| I  | 标准库 | 文件 IO、JSON、HTTP、Time、Map | 已完成 |
| G  | 字节码虚拟机 | 28 条指令的栈式虚拟机，`--vm` 标志 | 已完成 |
| J  | 工具链 | `check` / `fmt` / `run` / REPL / VSCode 扩展 | 已完成 |
| K  | 并发 | 线程池、`@concurrent`、`.async()` / `.await()`、通道 | 已完成 |
| L  | 包管理器 | `flux.toml`、依赖解析、本地仓库 | 已完成 |
| v1 | 规范 v1.0 | 结构体、接口、`func`、`!var`、区间、`exception` | 已完成 |
| v2 | 常量与枚举 | `conf` 常量、`enum` 自然数枚举 | 已完成 |
| v2 | 执行前检查 | 接口完整性验证、枚举值验证 | 已完成 |
| v2 | AI 原生类型 | `specify` 声明、`Specify.validate/describe/schema`、合约编程 | 已完成 |
| v2 | 原生编译 | `flux compile` 编译为 x86_64/arm64/riscv64 二进制、`flux profile` | 已完成 |
| M  | 自举 | Flux 编译 Flux | 计划中 |

## 设计哲学 — 为什么不用 Rust / C++？

Flux 不是系统编程语言。每一项"缺失"的特性，都是为热重载优先的开发体验做出的有意取舍。
完整对比见 [`docs/design-comparison.md`](docs/design-comparison.md)。

| 设计选择 | Rust / C++ | Flux | 原因 |
|---------|-----------|------|------|
| 类型系统 | 静态类型、泛型 / 模板 | 动态类型 + HM 推断（不暴露 `<T>` 语法） | 更快的保存→生效循环；类型复杂度对用户透明 |
| 内存管理 | 所有权 / RAII | GC（shared_ptr + 循环检测） | 热重载场景下 GC 开销远小于借用检查的编译成本 |
| 并发模型 | 无 GIL，真并行 | GIL + 线程池 + 通道 | GIL 从结构上消除数据竞争；I/O 并行不受影响 |
| 错误处理 | `Result<T,E>` / 异常 | `exception` + `default` + `@supervised` | 错误在模块边界隔离；supervisor 自动恢复 |
| 多态 | Trait / 虚函数 | 鸭子类型 interface | 无需 `impl` 样板代码；对热重载友好 |
| 分支匹配 | `match` / `switch` | `if value { else: pattern { } }` | 覆盖值匹配场景；持久化状态处理状态机 |
| AI 集成 | 外部工具 / 提示词 | `specify` 一等类型 + `flux inspect --json` | 合约与意图是语言的一部分，而非注释 |
| 开发反馈 | 修改 → 编译 → 运行 | 修改 → 保存 → **立即生效** | Flux 存在的理由 |

## 项目结构

```
Flux/
├── flux/                       # 语言实现
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp            CLI 入口
│   │   ├── token.h             TokenType 枚举（关键字、运算符、字面量）
│   │   ├── lexer.h/.cpp        词法分析器（字符串插值，所有词法单元）
│   │   ├── parser.h/.cpp       递归下降解析器
│   │   ├── ast.h               33 种 AST 节点类型
│   │   ├── typechecker.h/.cpp  类型推断与标注检查
│   │   ├── interpreter.h/.cpp  树遍历解释器（热重载，模块）
│   │   ├── compiler.h/.cpp     AST → 字节码编译器
│   │   ├── vm.h/.cpp           基于栈的字节码虚拟机
│   │   ├── stdlib.cpp          File, Json, Http, Time, Chan, Math, Specify
│   │   ├── concurrency.h       GIL + 线程安全
│   │   ├── threadpool.h        线程池与溢出策略
│   │   ├── formatter.h         AST → 源码美化输出
│   │   ├── watcher.h/.cpp      inotify 文件监听
│   │   ├── toml.h              零依赖 TOML 解析器
│   │   ├── pkgmgr.h/.cpp      包管理器
│   │   ├── lsp.h               Language Server Protocol 支持
│   │   ├── debugger.h          调试器（断点、单步、查看）
│   │   ├── profiler.h          性能分析器
│   │   ├── gc.h                垃圾回收器
│   │   ├── hir.h / mir.h       高级/中级 IR 优化管线
│   │   ├── jit.h               JIT 编译器
│   │   ├── codegen.h           原生代码生成
│   │   └── fluz.h              二进制格式保护（.fluz）
│   └── examples/               23 个示例脚本
├── vscode-flux/                VSCode 扩展（语法、代码片段、折叠）
├── Flux Language Spec.docx     语言规范文档
└── README.md                   ← 英文版
```

参见 [`flux/README.md`](flux/README.md) 获取详细文档，包括字节码指令集、
格式化器内部实现、REPL 命令以及包管理器架构。

## 构建要求

- C++17 编译器（GCC 7+ / Clang 5+）
- CMake 3.16+
- pthreads（用于文件监听与并发）

## 许可证

请参阅仓库获取许可证信息。
