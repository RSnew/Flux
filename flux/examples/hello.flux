// examples/hello.flux
// Flux 语言 Demo — 验证变量、函数、热更新

// ── 变量 ──
var version = "1.0"
var count = 0

// ── 函数 ──
func greet(name) {
    print("Hello, " + name + "! Flux v" + version)
}

func add(a, b) {
    return a + b
}

func factorial(n) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

// ── 主逻辑 ──
greet("World")

var result = add(10, 32)
print("10 + 32 = " + str(result))

var f5 = factorial(5)
print("5! = " + str(f5))

// 修改这里的数字，保存，看看热更新效果 👇
var magic = 42
print("Magic number: " + str(magic))
