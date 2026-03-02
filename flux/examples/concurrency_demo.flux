// concurrency_demo.flux — Feature K: 并发模型演示
// 展示 async/await、Chan 通道、spawn 三种并发原语

// ── 1. async / await ──────────────────────────────────────
// async 关键字将函数调用包装为 Future，在独立线程中运行
// await 等待 Future 完成并取得返回值

fn slow_add(a, b) {
    Time.sleep(50)
    return a + b
}

fn slow_mul(a, b) {
    Time.sleep(30)
    return a * b
}

print("=== async / await 演示 ===")

var t0 = Time.now()

// 并发启动两个任务
var f1 = async slow_add(10, 20)
var f2 = async slow_mul(6, 7)

// 等待结果（两任务并行运行，总耗时约 50ms 而非 80ms）
var sum = await f1
var prod = await f2

var elapsed = Time.diff(t0, Time.now())
print("slow_add(10, 20) = " + sum)
print("slow_mul(6, 7)   = " + prod)
print("并行耗时约: " + elapsed + "s (串行需约 0.08s)")


// ── 2. Chan 通道 ──────────────────────────────────────────
// Chan.make()     — 创建无界通道
// Chan.make(cap)  — 创建有界通道（满时发送者阻塞）
// chan.send(v)    — 发送值
// chan.recv()     — 阻塞接收
// chan.close()    — 关闭通道

print("")
print("=== Chan 通道演示 ===")

var ch = Chan.make(3)

spawn {
    var i = 1
    while i <= 5 {
        ch.send(i)
        i = i + 1
    }
    ch.close()
}

var received = ch.recv()
while received != nil {
    print("收到: " + received)
    received = ch.recv()
}
print("通道已关闭，接收完毕")


// ── 3. spawn 并发任务 ─────────────────────────────────────
// spawn { ... } 启动 fire-and-forget 后台任务

print("")
print("=== spawn 后台任务演示 ===")

var result_ch = Chan.make()

spawn {
    Time.sleep(20)
    result_ch.send(42)
}

spawn {
    Time.sleep(10)
    result_ch.send(100)
}

var r1 = result_ch.recv()
var r2 = result_ch.recv()
print("spawn 任务结果 1: " + r1)
print("spawn 任务结果 2: " + r2)


// ── 4. Future.isReady() ────────────────────────────────────
print("")
print("=== Future.isReady() 轮询演示 ===")

fn compute_heavy() {
    Time.sleep(40)
    return 9999
}

var fut = async compute_heavy()
print("任务已启动，isReady = " + fut.isReady())
Time.sleep(60)
print("等待 60ms 后，isReady = " + fut.isReady())
var val = await fut
print("最终结果: " + val)

print("")
print("Feature K 并发演示完成")
