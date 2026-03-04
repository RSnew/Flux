// ═══════════════════════════════════════════════════════════
// default {} 错误恢复测试
// ═══════════════════════════════════════════════════════════

print("=== default {} error recovery tests ===")

// ── Test 1: 函数 panic 恢复 ─────────────────────────────
fn divide(a, b) {
    if (b == 0) { panic("division by zero") }
    return a / b
}

// 正常调用 — 不触发 default
var r1 = divide(10, 2) default { -1 }
print("Test 1a: " + str(r1))
assert(r1 == 5, "normal call should return 5")

// 异常调用 — 触发 default
var r2 = divide(10, 0) default { -1 }
print("Test 1b: " + str(r2))
assert(r2 == -1, "panic should recover to -1")

// ── Test 2: exception + default 组合 ────────────────────
exception divide { "Denominator must be non-zero" }

var r3 = divide(100, 0) default { 0 }
print("Test 2: " + str(r3))
assert(r3 == 0, "exception + default should recover to 0")

// ── Test 3: default 块中有多条语句 ──────────────────────
fn fail() {
    panic("always fails")
}

var r4 = fail() default {
    var fallback = 42
    fallback
}
print("Test 3: " + str(r4))
assert(r4 == 42, "multi-statement default block")

// ── Test 4: 嵌套 default ───────────────────────────────
var r5 = fail() default {
    fail() default { 99 }
}
print("Test 4: " + str(r5))
assert(r5 == 99, "nested default recovery")

// ── Test 5: default 中使用字符串 ────────────────────────
fn mustFail() {
    panic("oops")
}

var r6 = mustFail() default { "recovered" }
print("Test 5: " + str(r6))
assert(r6 == "recovered", "string recovery")

// ── Test 6: 正常表达式不受影响 ──────────────────────────
var r7 = 1 + 2 + 3 default { 0 }
print("Test 6: " + str(r7))
assert(r7 == 6, "normal expr should not trigger default")

print("\n=== All default {} tests passed! ===")
