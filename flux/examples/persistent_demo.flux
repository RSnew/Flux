// examples/persistent_demo.flux
// 验证 persistent 状态在热更新后是否保留

persistent {
    visits: 0,
    totalAdded: 0
}

var resetOnReload = 0

func add(a, b) {
    var result = a + b
    state.totalAdded = state.totalAdded + result
    return result
}

// 每次热更新都会重新执行这里
state.visits = state.visits + 1
resetOnReload = resetOnReload + 1

print("=================================")
print("visits (persistent): " + str(state.visits))
print("resetOnReload (var):  " + str(resetOnReload))
print("totalAdded:           " + str(state.totalAdded))

var r = add(10, 5)
print("add(10, 5) =          " + str(r))
print("totalAdded after:     " + str(state.totalAdded))
print("=================================")
