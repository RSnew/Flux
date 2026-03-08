// VM 性能基准测试

func factorial(n) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

// 测试1: 5000次 factorial(12)
var start = Time.now()
var i = 0
while i < 5000 {
    factorial(12)
    i = i + 1
}
var elapsed = Time.now() - start
print("5000x factorial(12): " + str(elapsed) + "s")

// 测试2: 10000元素数组
start = Time.now()
var arr = []
var j = 0
while j < 10000 {
    arr.push(j)
    j = j + 1
}
var sum = 0
for v in arr {
    sum = sum + v
}
elapsed = Time.now() - start
print("10000-element array build+sum: " + str(elapsed) + "s")
print("sum = " + str(sum))
