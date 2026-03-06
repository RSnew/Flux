// concurrency_demo.flux — Feature K v2: 线程池并发模型
// 演示 @threadpool / @concurrent / Module.fn.async() / future.await(timeout)

// ── 1. 声明线程池 ─────────────────────────────────────────
// 一行声明即可，90% 场景默认值足够
@threadpool(name: "io-pool", size: 4)
@threadpool(name: "cpu-pool", size: 2)
@threadpool(name: "order-pool", size: 1)

// ── 2. 绑定模块到线程池 ───────────────────────────────────
// 默认：overflow: .block（安全优先，不丢任务）
@concurrent(pool: "cpu-pool")
module ImageProcessor {
    func resize(w, h) {
        Time.sleep(50)    // 模拟 CPU 密集型：50ms
        return w * h
    }
    func compress(quality) {
        Time.sleep(30)    // 模拟 CPU 密集型：30ms
        return quality * 100
    }
}

// 日志/采样场景：队列满时静默丢弃，不阻塞调用方
@concurrent(pool: "io-pool", queue: 32, overflow: .drop)
module FileService {
    func read(path) {
        Time.sleep(20)    // 模拟 IO：20ms
        return "content of " + path
    }
    func write(path, data) {
        Time.sleep(10)    // 模拟 IO：10ms
        return true
    }
}

// 金融/关键路径：队列满时报错，不能丢
@concurrent(pool: "order-pool", queue: 10000, overflow: .error)
module OrderBook {
    func place(symbol, qty, price) {
        Time.sleep(5)
        return symbol + " x" + qty + " @" + price
    }
}


print("=== Feature K v2: 线程池并发模型 ===")
print("")

// ── 3. Module.fn.async(args) — 跨 pool 异步调用 ──────────
// 调用方不阻塞，立即返回 Future；任务提交到目标模块的线程池
print("--- 3.1 并行执行两个 CPU 密集型任务 ---")
var t0 = Time.now()

var f1 = ImageProcessor.resize.async(1920, 1080)
var f2 = ImageProcessor.compress.async(90)

// future.await() 等待结果
var pixels = f1.await()
var compressed = f2.await()
var elapsed = Time.diff(t0, Time.now())

print("resize(1920, 1080) = " + pixels)
print("compress(90) = " + compressed)
print("并行耗时: " + elapsed + "s (串行需约 0.08s)")

// ── 4. future.await(timeout_ms) — 带超时的等待 ───────────
print("")
print("--- 4.1 future.await(timeout) ---")
var f3 = FileService.read.async("config.json")
var content = f3.await(500)    // 最多等 500ms
print("读取结果: " + content)

// ── 5. 同时运行多个 IO 任务 ──────────────────────────────
print("")
print("--- 5.1 多文件并发读取 ---")
var t1 = Time.now()
var fa = FileService.read.async("a.txt")
var fb = FileService.read.async("b.txt")
var fc = FileService.read.async("c.txt")

var ca = fa.await()
var cb = fb.await()
var cc = fc.await()
var el2 = Time.diff(t1, Time.now())
print(ca)
print(cb)
print(cc)
print("3 个文件并发耗时: " + el2 + "s (串行需约 0.06s)")

// ── 6. 关键路径调用 ──────────────────────────────────────
print("")
print("--- 6.1 OrderBook 串行队列 ---")
var o1 = OrderBook.place.async("AAPL", 100, 182.5)
var o2 = OrderBook.place.async("GOOG", 50, 141.0)
var r1 = o1.await()
var r2 = o2.await()
print("订单1: " + r1)
print("订单2: " + r2)

// ── 7. future.isReady() 轮询 ─────────────────────────────
print("")
print("--- 7.1 isReady() 状态轮询 ---")
var fheavy = ImageProcessor.resize.async(3840, 2160)
print("任务提交，isReady = " + fheavy.isReady())
Time.sleep(80)
print("等待 80ms 后，isReady = " + fheavy.isReady())
var res = fheavy.await()
print("结果: " + res)

// ── 8. 向后兼容：保留 async keyword / await keyword / spawn ──
print("")
print("--- 8. 向后兼容语法（async/await 关键字 + spawn）---")
func slow_fn(x) {
    Time.sleep(15)
    return x * 2
}
var fut_kw = async slow_fn(21)
var val_kw = await fut_kw
print("async keyword 结果: " + val_kw)

var ch = Chan.make(3)
spawn {
    ch.send(42)
    ch.send(99)
    ch.close()
}
print("spawn + Chan: " + ch.recv() + ", " + ch.recv())

print("")
print("Feature K v2 线程池并发演示完成 ✓")
