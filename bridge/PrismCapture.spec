@ stdcall -private PrismCaptureVersion() PrismCaptureVersion
@ stdcall -private PrismCaptureInit() PrismCaptureInit
@ stdcall -private PrismCaptureStart(long long long ptr ptr ptr) PrismCaptureStart
@ stdcall -private PrismCaptureStop() PrismCaptureStop
@ stdcall -private PrismCaptureSetMaxFps(long) PrismCaptureSetMaxFps
@ stdcall -private PrismCaptureGetStats(ptr) PrismCaptureGetStats
@ stdcall -private PrismCaptureShutdown() PrismCaptureShutdown
@ stdcall -private PrismShortcutsStart(ptr long ptr ptr) PrismShortcutsStart
@ stdcall -private PrismShortcutsGetStatus(ptr long) PrismShortcutsGetStatus
@ stdcall -private PrismShortcutsGetBindings(ptr long) PrismShortcutsGetBindings
@ stdcall -private PrismShortcutsConfigure() PrismShortcutsConfigure
@ stdcall -private PrismShortcutsStop() PrismShortcutsStop
@ stdcall -private PrismSystemInfoQuery(ptr) PrismSystemInfoQuery
