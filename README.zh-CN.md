# Flux 语言

一门以热重载为核心的脚本语言，支持持久化状态、模块隔离、
监督式崩溃恢复，以及可选的字节码虚拟机。

使用 **约 5,000 行 C++17** 构建，除 pthreads 外无任何外部依赖。

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

fn factorial(n) {
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
    fn increment() { state.count = state.count + 1 }
    fn getValue()  { return state.count }
}

Counter.increment()
print(Counter.getValue())
```

**监督式崩溃恢复**
```flux
@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    fn charge(amount) {
        if amount < 0 { panic("negative amount") }
    }
}
```

**线程池并发**
```flux
@threadpool(name: "cpu-pool", size: 4)

@concurrent(pool: "cpu-pool")
module ImageProcessor {
    fn resize(w, h) { return w * h }
}

var future = ImageProcessor.resize.async(1920, 1080)
var result = future.await()
```

**标准库**
```flux
File.write("/tmp/out.txt", "hello\n")
var content = File.read("/tmp/out.txt")

var obj = Json.parse("{\"x\": 1}")
print(Json.pretty(obj))

var t0 = Time.now()
Time.sleep(100)
print("elapsed: \(Time.diff(t0, Time.now())) ms")
```

**内置结构化日志**
```flux
log.info("服务器启动于端口 8080")
log.warn("连接池即将耗尽")
log.error("数据库连接失败")
log.debug("请求内容: \(payload)")
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

### 包管理器

```bash
flux new myapp          # 创建项目并生成 flux.toml
flux add mathlib        # 添加依赖
flux install            # 安装所有依赖
flux build              # 构建并运行
flux publish            # 发布到本地仓库
```

## 功能状态

| 编号 | 功能 | 描述 | 状态 |
|------|------|------|------|
| H  | 语言核心 | 数组、字符串插值 `\()`、for-in、方法调用 | 已完成 |
| I  | 标准库 | 文件 IO、JSON、HTTP、Time、Map | 已完成 |
| G  | 字节码虚拟机 | 28 条指令的栈式虚拟机，`--vm` 标志 | 已完成 |
| J  | 工具链 | `check` / `fmt` / `run` / REPL / VSCode 扩展 | 已完成 |
| K  | 并发 | 线程池、`@concurrent`、`.async()` / `.await()`、通道 | 已完成 |
| L  | 包管理器 | `flux.toml`、依赖解析、本地仓库 | 已完成 |
| v1 | 规范 v1.0 | 结构体、接口、`??`、`func`、`!var`、区间、`exception` | 已完成 |
| v2 | 常量与枚举 | `conf` 常量、`enum` 自然数枚举 | 已完成 |
| v2 | 日志 | 内置 `log.info/warn/error/debug` 命名空间 | 已完成 |
| v2 | 执行前检查 | 接口完整性验证、枚举值验证 | 已完成 |
| M  | 自举 | Flux 编译 Flux | 计划中 |

## 项目结构

```
Flux/
├── flux/                       # 语言实现
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp            CLI 入口
│   │   ├── lexer.h/.cpp        词法分析器（字符串插值，所有词法单元）
│   │   ├── parser.h/.cpp       递归下降解析器
│   │   ├── ast.h               33 种 AST 节点类型
│   │   ├── typechecker.h/.cpp  类型推断与标注检查
│   │   ├── interpreter.h/.cpp  树遍历解释器（热重载，模块）
│   │   ├── compiler.h/.cpp     AST → 字节码编译器
│   │   ├── vm.h/.cpp           基于栈的字节码虚拟机
│   │   ├── stdlib.cpp          File, Json, Http, Time, Chan, log
│   │   ├── concurrency.h       GIL + 线程安全
│   │   ├── threadpool.h        线程池与溢出策略
│   │   ├── formatter.h         AST → 源码美化输出
│   │   ├── watcher.h/.cpp      inotify 文件监听
│   │   ├── toml.h              零依赖 TOML 解析器
│   │   └── pkgmgr.h/.cpp      包管理器
│   └── examples/               11 个示例脚本
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
