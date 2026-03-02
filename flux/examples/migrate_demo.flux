module Analytics {
    persistent {
        pageViews: 0,
        sessions:  0,
        errors:    0,
        lastEvent: "none",
        userId:    "anonymous"
    }
    fn track(event) {
        state.pageViews = state.pageViews + 1
    }
    fn report() {
        print("  v3 report: views=" + str(state.pageViews))
    }
}
Analytics.track("checkout")
Analytics.report()
