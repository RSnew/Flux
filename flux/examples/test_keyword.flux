// test_keyword.flux — test 关键字演示
// test 声明会替换已有的同名函数或变量
// 使用 --no-test 运行时，所有 test 声明会被跳过

func add(a, b) {
    return a + b
}

var greeting = "Hello, World!"

// test 覆盖：替换 add 函数
test func add(a, b) {
    print("  [test mode] add called")
    return a + b + 100
}

// test 覆盖：替换 greeting 变量
test var greeting = "Hello from TEST!"

print("greeting = \(greeting)")
print("add(1, 2) = \(add(1, 2))")

// 运行方式：
//   flux run examples/test_keyword.flux
//     → greeting = Hello from TEST!
//     → add(1, 2) = 103
//
//   flux --no-test run examples/test_keyword.flux
//     → greeting = Hello, World!
//     → add(1, 2) = 3
