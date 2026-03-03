// ═══════════════════════════════════════════════════════════
// Flux 新特性演示 — conf, enum, log
// ═══════════════════════════════════════════════════════════

// ── conf: 常量声明（运行时只读）────────────────────────────
conf MAX_RETRIES = 3
conf APP_NAME = "Flux Demo"
conf PI = 3.14159

print("APP_NAME = \(APP_NAME)")
print("MAX_RETRIES = \(MAX_RETRIES)")
print("PI = \(PI)")

// ── enum: 枚举类型（Natural-only 值）───────────────────────
enum Direction {
    North = 0,
    South = 1,
    East = 2,
    West = 3
}

enum Color {
    Red,
    Green,
    Blue
}

print("Direction.North = \(Direction.North)")
print("Direction.West = \(Direction.West)")
print("Color.Red = \(Color.Red)")
print("Color.Blue = \(Color.Blue)")

// 枚举值用于条件判断
var heading = Direction.North
if heading == Direction.North {
    print("Heading North!")
}

// ── log: 内置结构化日志 ────────────────────────────────────
log.info("Application started: \(APP_NAME)")
log.debug("Debug mode active, max retries: \(MAX_RETRIES)")
log.warn("Low disk space warning")
log.error("Connection timeout after \(MAX_RETRIES) retries")

print("\nAll new features working!")
