# Flux vs Rust / C++ — 设计取舍

Flux 是热重载优先的脚本语言，与 Rust / C++ 的差异不是功能缺失，而是**有意的设计取舍**。
每一项都以热重载开发体验为核心做出选择。

---

## 类型系统：动态 + HM 推断 vs 静态类型

| | Rust / C++ | Flux |
|--|-----------|------|
| 方式 | 静态类型、泛型 / 模板 | 动态类型 + Hindley-Milner 变体推断 |
| 泛型语法 | `<T>` / `template<typename T>` | **不暴露**，通过类型推断隐式支持 |

**为什么**：热重载场景下，静态类型系统会拖慢"保存→生效"的反馈循环。
Flux 选择不暴露 `<T>` 语法，通过 HM 推断在不增加语言复杂度的前提下获得类型安全。
多态通过鸭子类型 + interface 结构化匹配实现，无需显式泛型参数。

---

## 内存管理：GC vs 所有权 / RAII

| | Rust | C++ | Flux |
|--|------|-----|------|
| 方式 | 所有权 + 借用检查 | RAII + 智能指针 | shared_ptr + 循环检测 |
| 编译期保证 | use-after-free 不可能 | 部分 | 无 |

**为什么**：所有权系统的代价是编译时间和代码复杂度。
Flux 面向的是快速迭代的脚本场景——开发者修改代码保存后立即看到效果，
GC 的微小运行时开销远小于等待编译器做借用检查的时间成本。
对于 Flux 的目标场景（服务编排、自动化、IoT 边缘脚本），GC 延迟完全可接受。

---

## 并发模型：GIL vs 无锁并行

| | Rust / C++ | Flux |
|--|-----------|------|
| 方式 | 无 GIL，真并行 | GIL + 线程池 + Channel |
| 数据竞争 | Rust 编译期防止；C++ 靠开发者 | **结构性消除**——GIL 保证同一时刻只有一个 Flux 线程执行 |

**为什么**：GIL 用一把锁换来了并发安全的确定性。
Flux 的 `@threadpool` + `@concurrent` + `@supervised` 让开发者声明式地描述并发意图，
不需要手动管理锁、条件变量、原子操作。I/O 等待期间 GIL 自动释放，
对于 Flux 的典型场景（HTTP 服务、文件处理、设备通信）吞吐量不受影响。
CPU 密集计算不是 Flux 的设计目标。

---

## 错误处理：模块隔离 vs Result / 异常

| | Rust | C++ | Flux |
|--|------|-----|------|
| 方式 | `Result<T, E>` + `?` 传播 | try / catch 异常 | `exception` 描述 + `default` 兜底 + `@supervised` 恢复 |
| 强制处理 | 编译期穷举 | 无 | **模块边界隔离** |

**为什么**：Rust 的 Result 强制每个调用点处理错误，适合库代码但增加了样板代码。
Flux 采用不同思路——错误在模块边界被隔离：

- `exception` 将错误描述绑定到函数，文档化错误语义
- `default` 为函数提供兜底返回值，panic 不扩散到调用方
- `@supervised(restart: .always)` 模块崩溃自动重启，不影响其他模块
- 热重载时接口检查失败 → 拒绝更新，继续运行旧代码

一个模块挂了，其他模块照常运行。这对长期运行的服务比逐层传播错误更实用。

---

## 多态：泛型 / 模板 vs 鸭子类型

| | Rust | C++ | Flux |
|--|------|-----|------|
| 方式 | Trait + 泛型 | 虚函数 + 模板 | interface 结构化匹配（鸭子类型） |

**为什么**：Flux 的 interface 是运行时结构化检查——只要对象拥有接口要求的方法，就自动满足约束。
不需要 `impl Trait for Type` 或 `class Derived : public Base` 的显式声明。
配合热重载，开发者可以随时修改 struct 的方法，保存后立即通过接口检查，无需重新编译整个依赖链。

---

## 分支匹配：if + else: vs match / switch

| | Rust | C++ | Flux |
|--|------|-----|------|
| 方式 | `match` 表达式 + 穷举检查 | `switch` / `if-else` | `if value { else: pattern { } }` 多分支语法 |

**为什么**：Rust 的 `match` 依赖代数数据类型（枚举变体携带关联数据）。
Flux 的枚举只允许 Natural 值，不携带关联数据——因为复杂状态放在 `persistent` 模块里管理更合适。
`if + else:` 多分支语法覆盖了值匹配的场景，配合模块化状态机设计：

```flux
module Door {
    persistent { state: "locked" }

    func transition(event) {
        if state {
            else: "locked" {
                if event == "unlock" { state = "closed" }
            }
            else: "closed" {
                if event == "open" { state = "open" }
                if event == "lock" { state = "locked" }
            }
            else: "open" {
                if event == "close" { state = "closed" }
            }
        }
    }
}
```

状态持久化跨热重载保留，`@supervised` 防止非法转换导致崩溃。

---

## 开发体验：保存即生效 vs 编译等待

| | Rust / C++ | Flux |
|--|-----------|------|
| 反馈循环 | 修改 → 编译 → 运行 | 修改 → 保存 → **立即生效** |
| 状态保留 | 重启丢失 | `persistent` 跨热重载保留 |
| 崩溃恢复 | 进程退出 | `@supervised` 自动重启模块 |
| 灰度更新 | 需要基础设施支持 | `!var` / `!func` 语言级灰度 |

**为什么**：这是 Flux 存在的根本理由。
上述所有取舍（GC、GIL、动态类型、模块化错误隔离）都服务于一个目标：
**让开发者在保存文件的瞬间看到变化，同时不丢失运行时状态。**

---

## 总结

Flux 不试图成为 Rust 或 C++ 的替代品。它们解决不同的问题：

- **Rust / C++**：编译期最大化安全与性能，适合系统软件、基础设施、性能敏感场景
- **Flux**：运行时最大化开发效率与容错，适合服务编排、自动化、IoT 边缘、快速原型

每一项"缺失"的特性，都是为热重载开发体验让出的空间。
