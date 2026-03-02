// stdlib_demo.flux — File IO、JSON、Http 标准库演示

// ══════════════════════════════════════════════════════════
// 1. File 模块
// ══════════════════════════════════════════════════════════
print("=== 1. File 模块 ===")

let path = "/tmp/flux_stdlib_test.txt"

// 写入
File.write(path, "Hello, Flux!\nLine 2\nLine 3\n")
print("exists:", File.exists(path))

// 读取整个文件
let content = File.read(path)
print("content:", content)

// 逐行读取
let lines = File.lines(path)
print("line count:", lines.len())
for line in lines {
    print("  |", line)
}

// 追加
File.append(path, "Line 4\n")
print("after append:", File.lines(path).len(), "lines")

// 删除
File.delete(path)
print("after delete, exists:", File.exists(path))

// ══════════════════════════════════════════════════════════
// 2. Json 模块
// ══════════════════════════════════════════════════════════
print("")
print("=== 2. Json 模块 ===")

// 解析 JSON 字符串（对象 → Map）
let raw = "{\"name\": \"Flux\", \"version\": 4, \"active\": true, \"tags\": [\"fast\", \"hot-reload\"]}"
let obj = Json.parse(raw)

print("name:", obj["name"])
print("version:", obj["version"])
print("active:", obj["active"])
print("tags:", obj["tags"])
print("first tag:", obj["tags"][0])

// Map 方法
print("has 'name':", obj.has("name"))
print("has 'missing':", obj.has("missing"))
print("keys:", obj.keys())

// 修改 / 新增字段
obj["version"] = 5
obj["language"] = "Flux"

// 序列化
print("stringify:", Json.stringify(obj))
print("pretty:")
print(Json.pretty(obj))

// 用 Map() 创建空 Map 并填充
let m = Map()
m["x"] = 10
m["y"] = 20
print("Map:", m)
print("Json.stringify(Map):", Json.stringify(m))

// 遍历 Map（迭代 key）
for key in m {
    print("  key:", key, "val:", m[key])
}

// ══════════════════════════════════════════════════════════
// 3. Http 模块（取消注释以测试，需要网络）
// ══════════════════════════════════════════════════════════
// print("")
// print("=== 3. Http 模块 ===")
// let body = Http.get("http://httpbin.org/get")
// print("GET response:", body)
// let posted = Http.post("http://httpbin.org/post", Json.stringify(m))
// print("POST response:", posted)
