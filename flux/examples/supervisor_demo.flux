@supervised(restart: .always, maxRetries: 3)
module PaymentService {
    persistent {
        processed: 0
    }

    fn charge(amount) {
        if amount < 0 {
            panic("negative amount: " + str(amount))
        }
        state.processed = state.processed + 1
        print("  [Payment] v2 charged " + str(amount) + "  total=" + str(state.processed))
    }

    fn getStats() {
        return state.processed
    }
}

module Logger {
    fn log(msg) { print("  [LOG] " + msg) }
}

Logger.log("=== hot reload! processed so far: PRESERVED ===")
PaymentService.charge(999)
Logger.log("total=" + str(PaymentService.getStats()))
