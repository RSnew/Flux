// ai_demo.flux — Flux AI 友好特性演示
// 演示：AI 原生类型、合约（requires/ensures）、AI stdlib

// ══════════════════════════════════════════════════════════
// 1. AI 原生类型 — 向 AI 工具暴露意图、约束和示例
// ══════════════════════════════════════════════════════════

// 定义一个 AI 类型：支付验证器的规格说明
var paymentValidator = ai {
    intent: "验证用户支付数据的合法性",
    input: "amount: Int, currency: String",
    output: "Bool",
    constraints: ["amount > 0", "amount <= 1000000", "currency in [USD, EUR, CNY]"],
    examples: ["valid: amount=100 currency=USD -> true", "invalid: amount=-1 -> false"]
}

// 使用 AI stdlib 内省 AI 类型
print("=== AI 类型内省 ===")
print(AI.describe(paymentValidator))
print("Intent: " + AI.intent(paymentValidator))

var schema = AI.schema(paymentValidator)
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
// 3. AI 类型 + 合约 结合使用
// ══════════════════════════════════════════════════════════

// AI 类型定义转账规则
var transferSpec = ai {
    intent: "在两个账户之间安全转账",
    input: "from: String, to: String, amount: Int",
    output: "Bool",
    constraints: ["from != to", "amount > 0"]
}

// 基于 AI 约束实现的转账函数
func transfer(from, to, amount)
requires { from != to, amount > 0 }
{
    print("  Transfer " + str(amount) + " from " + from + " to " + to)
    return true
}

print("\n=== AI + 合约 结合 ===")
print("Transfer spec intent: " + AI.intent(transferSpec))
transfer("Alice", "Bob", 100)

print("\n=== 类型检查 ===")
print("paymentValidator type: " + type(paymentValidator))
print("transferSpec type: " + type(transferSpec))

print("\n✅ AI 友好特性演示完成!")
