// examples/module_demo.flux
// 验证：模块系统 + 独立 persistent 状态 + 跨模块调用 + 热更新隔离

module Counter {
    persistent {
        count: 0,
        total: 0
    }

    fn increment() {
        state.count = state.count + 1
        state.total = state.total + 1
    }

    fn decrement() {
        state.count = state.count - 1
    }

    fn reset() {
        state.count = 0
    }

    fn getValue() {
        return state.count
    }

    fn getTotal() {
        return state.total
    }
}

module Logger {
    persistent {
        logs: 0
    }

    fn log(msg) {
        state.logs = state.logs + 1
        print("[LOG #" + str(state.logs) + "] " + msg)
    }

    fn getLogs() {
        return state.logs
    }
}

// ── 顶层程序（热更新时重新执行）──────────────────────────
Logger.log("=== 程序启动 ===")

Counter.increment()
Counter.increment()
Counter.increment()

Logger.log("count = " + str(Counter.getValue()))
Logger.log("total = " + str(Counter.getTotal()))

Counter.decrement()
Logger.log("after decrement: " + str(Counter.getValue()))

// 修改这行触发热更新，观察 logs 和 total 的变化
let note = "first run"
Logger.log("note: " + note)

Logger.log("total log count: " + str(Logger.getLogs()))
