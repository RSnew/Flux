// ai_demo.flux — Flux 规格声明 (specify) 特性演示
// 演示：specify 类型、合约（requires/ensures）、Specify stdlib

// ══════════════════════════════════════════════════════════
// 1. specify 类型 — 向工具暴露意图、约束和示例
// ══════════════════════════════════════════════════════════

// 定义一个规格：支付验证器的规格说明
var paymentValidator = specify {
    intent: "验证用户支付数据的合法性",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "amount <= 1000000", "currency in [USD, EUR, CNY]"],
    examples: ["valid: amount=100 currency=USD -> true", "invalid: amount=-1 -> false"]
}

// 使用 Specify stdlib 内省规格类型
print("=== 规格类型内省 ===")
print(Specify.describe(paymentValidator))
print("Intent: " + Specify.intent(paymentValidator))

var schema = Specify.schema(paymentValidator)
print("Schema name: " + schema["name"])

// ══════════════════════════════════════════════════════════
// 2. 合约（Design by Contract）— requires / ensures
// ══════════════════════════════════════════════════════════

// 带前置/后置条件的函数
func divide(a, b)
requires { b != 0 }
ensures  { result != null }
{
    return a / b
}

print("\n=== 合约演示 ===")
print("divide(10, 3) = " + str(divide(10, 3)))

// 带范围约束的函数
func clamp(value, minVal, maxVal)
requires { minVal <= maxVal }
ensures  { result >= minVal, result <= maxVal }
{
    if (value < minVal) { return minVal }
    if (value > maxVal) { return maxVal }
    return value
}

print("clamp(150, 0, 100) = " + str(clamp(150, 0, 100)))
print("clamp(-5, 0, 100)  = " + str(clamp(-5, 0, 100)))
print("clamp(50, 0, 100)  = " + str(clamp(50, 0, 100)))

// ══════════════════════════════════════════════════════════
// 3. specify + 合约 结合使用
// ══════════════════════════════════════════════════════════

// 规格定义转账规则
var transferSpec = specify {
    intent: "在两个账户之间安全转账",
    input: "from: String, to: String, amount: Int",
    output: "Bool",
    constraints: ["from != to", "amount > 0"]
}

// 基于规格约束实现的转账函数
func transfer(from, to, amount)
requires { from != to, amount > 0 }
{
    print("  Transfer " + str(amount) + " from " + from + " to " + to)
    return true
}

print("\n=== specify + 合约 结合 ===")
print("Transfer spec intent: " + Specify.intent(transferSpec))
transfer("Alice", "Bob", 100)

print("\n=== 类型检查 ===")
print("paymentValidator type: " + type(paymentValidator))
print("transferSpec type: " + type(transferSpec))

print("\n✅ 规格声明特性演示完成!")
