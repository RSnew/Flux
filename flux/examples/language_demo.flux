// ── 1. 字符串插值 ─────────────────────────────────────────
let name = "Flux"
let version = 4
let pi = 3.14

print("语言: \(name)  版本: \(version)  pi ≈ \(pi)")
print("计算式: \(version * version) = v²")

// ── 2. 数组字面量 + 下标 ──────────────────────────────────
let nums = [10, 20, 30, 40, 50]
print("nums[0]=\(nums[0])  nums[4]=\(nums[4])")

// 数组方法
nums.push(60)
print("after push: len=\(nums.len())")
let last = nums.pop()
print("popped: \(last)  now len=\(nums.len())")

// 字符串方法
let tags = ["hot-reload", "type-safe", "modules"]
print("joined: " + tags.join(", "))
print("contains 'modules': \(tags.contains("modules"))")

// 字符串方法
let sentence = "  Hello Flux World  "
print("trimmed: '\(sentence.trim())'")
print("upper: \(sentence.trim().upper())")
let words = sentence.trim().split(" ")
print("words: \(words.len()) parts, first='\(words[0])'")

// ── 3. for-in 循环 ────────────────────────────────────────
let scores = [85, 92, 78, 95, 88]
var total = 0
for s in scores {
    total = total + s
}
print("scores avg: \(total / scores.len())")

// range
var evens = 0
for i in range(10) {
    if i % 2 == 0 {
        evens = evens + 1
    }
}
print("evens in range(10): \(evens)")

// for-in 字符串字符
var vowels = 0
for c in "hello flux" {
    if c == "a" || c == "e" || c == "i" || c == "o" || c == "u" {
        vowels = vowels + 1
    }
}
print("vowels in 'hello flux': \(vowels)")

// ── 4. 数组 + 模块组合 ────────────────────────────────────
module Stats {
    persistent {
        history: 0
    }

    fn sum(arr: Any) -> Int {
        var total = 0
        for x in arr {
            total = total + x
        }
        state.history = state.history + 1
        return total
    }

    fn max(arr: Any) -> Int {
        var m = arr[0]
        for x in arr {
            if x > m { m = x }
        }
        return m
    }
}

let data = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3]
print("sum=\(Stats.sum(data))  max=\(Stats.max(data))")
print("Stats called \(Stats.history) times")

// ── 5. 插值嵌套表达式 ─────────────────────────────────────
let a = 6
let b = 7
print("The answer is \(a * b) and that's \(a * b == 42)")
