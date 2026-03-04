// ═══════════════════════════════════════════════════════════
// exception + default {} 完整错误处理测试
// ═══════════════════════════════════════════════════════════

print("=== exception + default {} tests ===")

// ── Test 1: 经典用法 — exception 描述 + default 默认值 ──
exception divide {
    "除数 b 不能为零"
    "失败时返回默认值 0"
}

func divide(a, b) {
    if (b == 0) {
        exception { "调用方传入了 b=0" }
        default { 0 }
    }
    return a / b
}

var r1 = divide(10, 2)
print("Test 1a: divide(10,2) = " + str(r1))
assert(r1 == 5, "normal divide should return 5")

var r2 = divide(10, 0)
print("Test 1b: divide(10,0) = " + str(r2))
assert(r2 == 0, "div by zero should return default 0")

// ── Test 2: 字符串默认值 ────────────────────────────────
func safe_greet(name) {
    if (name == nil) {
        exception { "name 参数为 nil" }
        default { "Hello, Guest!" }
    }
    return "Hello, " + name + "!"
}

var r3 = safe_greet("Flux")
print("Test 2a: " + r3)
assert(r3 == "Hello, Flux!", "normal greet")

var r4 = safe_greet(nil)
print("Test 2b: " + r4)
assert(r4 == "Hello, Guest!", "nil should return default greeting")

// ── Test 3: 多条件 default ──────────────────────────────
exception clamp {
    "val 必须在 [min, max] 范围内"
}

func clamp(val, lo, hi) {
    if (val < lo) {
        default { lo }
    }
    if (val > hi) {
        default { hi }
    }
    return val
}

print("Test 3a: clamp(5, 0, 10) = " + str(clamp(5, 0, 10)))
assert(clamp(5, 0, 10) == 5, "in range")

print("Test 3b: clamp(-3, 0, 10) = " + str(clamp(-3, 0, 10)))
assert(clamp(-3, 0, 10) == 0, "below min returns lo")

print("Test 3c: clamp(15, 0, 10) = " + str(clamp(15, 0, 10)))
assert(clamp(15, 0, 10) == 10, "above max returns hi")

// ── Test 4: default 块中多条语句 ────────────────────────
func safe_index(arr, i) {
    if (i < 0) {
        exception { "负数索引不允许" }
        default {
            var fallback = "N/A"
            fallback
        }
    }
    if (i >= len(arr)) {
        exception { "索引越界" }
        default { "N/A" }
    }
    return arr[i]
}

var items = ["a", "b", "c"]
print("Test 4a: " + safe_index(items, 1))
assert(safe_index(items, 1) == "b", "normal index")

print("Test 4b: " + safe_index(items, -1))
assert(safe_index(items, -1) == "N/A", "negative index default")

print("Test 4c: " + safe_index(items, 99))
assert(safe_index(items, 99) == "N/A", "overflow index default")

// ── Test 5: exception 方法描述 ──────────────────────────
exception divide {
    "请注意：除数不能为零"
}

print("Test 6: exception target validation OK")

print("\n=== All exception + default {} tests passed! ===")
