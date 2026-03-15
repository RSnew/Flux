// 纯数值基准测试（无数组/字符串操作）
func factorial(n) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

var start = Time.now()
var i = 0
while i < 5000 {
    factorial(12)
    i = i + 1
}
var elapsed = Time.now() - start
print("5000x factorial(12): " + str(elapsed) + "s")
