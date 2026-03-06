// test_array_interp_forin.flux — 数组 / 字符串插值 / for-in 功能测试
// 每个 assert 失败时会 panic，全部通过则打印 "ALL TESTS PASSED"

// ── 1. 数组字面量 + 下标 ──────────────────────────────────
var a = [1, 2, 3, 4, 5]
assert(a[0] == 1, "a[0] should be 1")
assert(a[4] == 5, "a[4] should be 5")
assert(a[-1] == 5, "negative index: a[-1] should be 5")

// ── 2. 数组方法 ───────────────────────────────────────────
var b = [10, 20, 30]
b.push(40)
assert(b.len() == 4, "after push len should be 4")
assert(b[3] == 40, "pushed element should be 40")

var popped = b.pop()
assert(popped == 40, "popped value should be 40")
assert(b.len() == 3, "after pop len should be 3")

assert(b.first() == 10, "first() should be 10")
assert(b.last() == 30, "last() should be 30")
assert(b.contains(20), "contains(20) should be true")
assert(!b.contains(99), "contains(99) should be false")

var joined = b.join("-")
assert(joined == "10-20-30", "join should produce '10-20-30'")

// ── 3. 下标赋值 ───────────────────────────────────────────
b[1] = 99
assert(b[1] == 99, "index assign: b[1] should be 99")

// ── 4. for-in 数组求和 ───────────────────────────────────
var nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
var sum = 0
for n in nums {
    sum = sum + n
}
assert(sum == 55, "sum 1..10 should be 55")

// ── 5. for-in range ──────────────────────────────────────
var evens = 0
for i in range(20) {
    if i % 2 == 0 {
        evens = evens + 1
    }
}
assert(evens == 10, "evens in range(20) should be 10")

// ── 6. for-in 字符串字符 ──────────────────────────────────
var vowels = 0
for c in "hello world" {
    if c == "a" || c == "e" || c == "i" || c == "o" || c == "u" {
        vowels = vowels + 1
    }
}
assert(vowels == 3, "vowels in 'hello world' should be 3")

// ── 7. 字符串插值 — 基本 ──────────────────────────────────
var name = "Flux"
var ver = 2
var msg = "lang=\(name) ver=\(ver)"
assert(msg == "lang=Flux ver=2", "basic interpolation")

// ── 8. 字符串插值 — 表达式 ───────────────────────────────
var x = 6
var y = 7
var product_msg = "\(x) * \(y) = \(x * y)"
assert(product_msg == "6 * 7 = 42", "expression interpolation")

// ── 9. 字符串插值 — 嵌套方法调用 ────────────────────────
var words = ["alpha", "beta", "gamma"]
var list_msg = "items: \(words.len())"
assert(list_msg == "items: 3", "interpolation with method call")

// ── 10. 字符串方法链 ──────────────────────────────────────
var raw = "  Hello Flux  "
var trimmed = raw.trim()
assert(trimmed == "Hello Flux", "trim() should remove leading/trailing spaces")
assert(trimmed.upper() == "HELLO FLUX", "upper() should uppercase")
assert(trimmed.lower() == "hello flux", "lower() should lowercase")
var parts = trimmed.split(" ")
assert(parts.len() == 2, "split(' ') on 'Hello Flux' should give 2 parts")
assert(parts[0] == "Hello", "first part should be 'Hello'")
assert(parts[1] == "Flux",  "second part should be 'Flux'")

// ── 11. 比较运算符（原来存在 bug 的 <= 和 >=）────────────
assert(3 <= 3, "3 <= 3 should be true")
assert(3 <= 4, "3 <= 4 should be true")
assert(!(4 <= 3), "4 <= 3 should be false")
assert(4 >= 4, "4 >= 4 should be true")
assert(5 >= 4, "5 >= 4 should be true")
assert(!(3 >= 4), "3 >= 4 should be false")

// ── 12. 模块 + 数组参数 ───────────────────────────────────
module Calc {
    func sum(arr: Any) -> Int {
        var total = 0
        for v in arr {
            total = total + v
        }
        return total
    }

    func max(arr: Any) -> Int {
        var m = arr[0]
        for v in arr {
            if v > m { m = v }
        }
        return m
    }
}

var data = [3, 1, 4, 1, 5, 9, 2, 6]
assert(Calc.sum(data) == 31, "sum of [3,1,4,1,5,9,2,6] should be 31")
assert(Calc.max(data) == 9,  "max of [3,1,4,1,5,9,2,6] should be 9")

print("ALL TESTS PASSED")
