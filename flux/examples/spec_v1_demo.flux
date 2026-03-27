// ============================================================
// Flux Language Spec v1.0 — Feature Showcase
// ============================================================

// ── 1. func keyword ──────────────────────────────────────────
func greet(name) {
    return "Hello, " + name + "!"
}
print(greet("Flux"))

// ── 2. ?? null-coalescing operator ──────────────────────────
var missing = null
var value   = missing ?? "default"
print("null coalesce: " + value)

var found  = "real"
var result = found ?? "fallback"
print("null coalesce found: " + result)

// ── 3. Struct literals with methods ────────────────────────
var Point = {
    x: 0,
    y: 0,
    func distance() {
        return (self.x * self.x + self.y * self.y)
    },
    func move(dx, dy) {
        self.x = self.x + dx
        self.y = self.y + dy
    }
}

// Named construction: Point(x: 3, y: 4)
var p = Point(x: 3, y: 4)
print("Point: x=" + str(p.x) + " y=" + str(p.y))
print("dist²=" + str(p.distance()))

// ── 4. Interface declaration ────────────────────────────────
var Shape: interface = {
    func area()
    func perimeter()
}

// ── 5. Struct implementing interface ───────────────────────
var Circle = Shape {
    radius: 1,
    func area() {
        return 3.14159 * self.radius * self.radius
    },
    func perimeter() {
        return 2 * 3.14159 * self.radius
    }
}

var c = Circle(radius: 5)
print("Circle area: " + str(c.area()))
print("Circle perim: " + str(c.perimeter()))

var Rect = Shape {
    w: 1,
    h: 1,
    func area() {
        return self.w * self.h
    },
    func perimeter() {
        return 2 * (self.w + self.h)
    }
}

var r = Rect(w: 4, h: 6)
print("Rect area: " + str(r.area()))
print("Rect perim: " + str(r.perimeter()))

// ── 6. Math interval loops ──────────────────────────────────
// Closed interval [1, 5]
var sum_closed = 0
for i in [1, 5] {
    sum_closed = sum_closed + i
}
print("sum [1,5] = " + str(sum_closed))   // 1+2+3+4+5 = 15

// Half-open interval [1, 5)
var sum_half = 0
for i in [1, 5) {
    sum_half = sum_half + i
}
print("sum [1,5) = " + str(sum_half))     // 1+2+3+4 = 10

// ── 7. struct(s) — iterate field names ─────────────────────
var fields = struct(p)
print("Point fields: " + str(fields))

// ── 8. exception — error descriptions ──────────────────────
exception divide {
    "Division by zero is not allowed"
    "Ensure denominator is non-zero before calling divide()"
}

func divide(a, b) {
    if b == 0 {
        panic("division by zero")
    }
    return a / b
}

print("10 / 4 = " + str(divide(10, 4)))

// ── 9. Struct method dispatch via ModuleCall syntax ─────────
// p.move(dx, dy) calls the move method
p.move(10, 20)
print("After move: x=" + str(p.x) + " y=" + str(p.y))

print("All spec v1.0 features OK!")
