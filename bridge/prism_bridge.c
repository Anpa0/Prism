/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PrismCapture.dll - the Winelib side of Prism.
 *
 * This translation unit is the only place where the two worlds meet. It is
 * compiled by winegcc, so it can call Win32 (CreateThread, event objects) and
 * native Linux (GLib, PipeWire) in the same function. The capture thread is
 * deliberately a Win32 thread: Wine registers it with the PE loader, which is
 * what makes it legal for the PipeWire callback to call straight back into
 * Prism.exe without any marshalling.
 */

#include "prism_log.h"

#include "PrismCapture.h"
#include "screencast.h"
#include "pipewire.h"

static struct
{
    PrismFrameCallback  on_frame;
    PrismStatusCallback on_status;
    void*               context;

    HANDLE thread;
    HANDLE loop_ready;

    volatile LONG initialised;
    volatile LONG active;
    volatile LONG state;

    volatile unsigned int source_width;
    volatile unsigned int source_height;
    volatile unsigned int source_format;
    volatile unsigned int source_fps_num;
    volatile unsigned int source_fps_den;
} bridge;

/* --------------------------------------------------- capture-thread side -- */

static void bridge_on_frame(const PrismFrame* frame)
{
    PrismFrameCallback callback = bridge.on_frame;

    if(!InterlockedCompareExchange(&bridge.active, 1, 1))
        return;
    if(callback)
        callback(frame, bridge.context);
}

static void bridge_on_format(unsigned int width, unsigned int height, unsigned int format, unsigned int fps_num,
                             unsigned int fps_den)
{
    bridge.source_width   = width;
    bridge.source_height  = height;
    bridge.source_format  = format;
    bridge.source_fps_num = fps_num;
    bridge.source_fps_den = fps_den;
}

static void bridge_on_state(unsigned int state, const char* message)
{
    PrismStatusCallback callback = bridge.on_status;

    InterlockedExchange(&bridge.state, (LONG)state);
    if(callback)
        callback(state, message ? message : "", bridge.context);
}

static const struct prism_pw_callbacks bridge_callbacks = {
    bridge_on_frame,
    bridge_on_format,
    bridge_on_state,
};

static DWORD WINAPI capture_thread_proc(LPVOID param)
{
    (void)param;
    SetEvent(bridge.loop_ready);
    prism_screencast_run();
    prism_debug("capture thread exiting");
    return 0;
}

static int ensure_capture_thread(void)
{
    if(bridge.thread)
        return 0;

    bridge.loop_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!bridge.loop_ready)
        return -1;

    bridge.thread = CreateThread(NULL, 0, capture_thread_proc, NULL, 0, NULL);
    if(!bridge.thread)
    {
        CloseHandle(bridge.loop_ready);
        bridge.loop_ready = NULL;
        return -1;
    }

    /* g_idle_add() is safe to call before the loop runs, but waiting keeps the
     * logs in a sane order and costs nothing on a cold start. */
    WaitForSingleObject(bridge.loop_ready, 2000);
    return 0;
}

/* ---------------------------------------------------------- PE-side API -- */

unsigned int __stdcall PrismCaptureVersion(void)
{
    return PRISM_CAPTURE_ABI_VERSION;
}

long __stdcall PrismCaptureInit(void)
{
    if(InterlockedCompareExchange(&bridge.initialised, 1, 1))
        return S_OK;

    if(prism_screencast_init() < 0)
    {
        prism_warn("ScreenCast portal unavailable - is xdg-desktop-portal running in this session?");
        return E_FAIL;
    }

    if(ensure_capture_thread() < 0)
    {
        prism_warn("could not start the capture thread");
        return E_FAIL;
    }

    InterlockedExchange(&bridge.initialised, 1);
    prism_info("bridge initialised (ABI %u)", PRISM_CAPTURE_ABI_VERSION);
    return S_OK;
}

long __stdcall PrismCaptureStart(unsigned int source_types, unsigned int cursor_mode, unsigned int max_fps,
                                 PrismFrameCallback on_frame, PrismStatusCallback on_status, void* context)
{
    if(!InterlockedCompareExchange(&bridge.initialised, 1, 1))
        return E_FAIL;
    if(InterlockedCompareExchange(&bridge.active, 1, 1))
        return E_FAIL;
    if(!on_frame)
        return E_INVALIDARG;

    bridge.on_frame       = on_frame;
    bridge.on_status      = on_status;
    bridge.context        = context;
    bridge.source_width   = 0;
    bridge.source_height  = 0;
    bridge.source_format  = 0;
    bridge.source_fps_num = 0;
    bridge.source_fps_den = 0;

    prism_pipewire_set_max_fps(max_fps);
    InterlockedExchange(&bridge.active, 1);

    prism_screencast_start(source_types ? source_types : PRISM_SOURCE_ANY, cursor_mode, &bridge_callbacks);
    return S_OK;
}

long __stdcall PrismCaptureStop(void)
{
    if(!InterlockedCompareExchange(&bridge.active, 1, 1))
        return S_FALSE;

    /* Clear `active` first: the flag is what stops in-flight PipeWire callbacks
     * from re-entering Prism.exe while it tears its renderer down. */
    InterlockedExchange(&bridge.active, 0);
    prism_screencast_stop();

    bridge.on_frame  = NULL;
    bridge.on_status = NULL;
    bridge.context   = NULL;
    InterlockedExchange(&bridge.state, PRISM_STATE_IDLE);
    return S_OK;
}

void __stdcall PrismCaptureSetMaxFps(unsigned int max_fps)
{
    prism_pipewire_set_max_fps(max_fps);
}

void __stdcall PrismCaptureGetStats(PrismBridgeStats* out)
{
    if(!out)
        return;

    memset(out, 0, sizeof(*out));
    prism_pipewire_get_counters(&out->frames_received, &out->frames_delivered, &out->frames_throttled,
                                &out->frames_corrupt);
    out->source_width  = bridge.source_width;
    out->source_height = bridge.source_height;
    out->source_format = bridge.source_format;
    out->source_fps_num = bridge.source_fps_num;
    out->source_fps_den = bridge.source_fps_den;
    out->state          = (unsigned int)bridge.state;
}

void __stdcall PrismCaptureShutdown(void)
{
    if(InterlockedCompareExchange(&bridge.active, 1, 1))
        PrismCaptureStop();

    prism_screencast_quit();

    if(bridge.thread)
    {
        WaitForSingleObject(bridge.thread, 2000);
        CloseHandle(bridge.thread);
        bridge.thread = NULL;
    }
    if(bridge.loop_ready)
    {
        CloseHandle(bridge.loop_ready);
        bridge.loop_ready = NULL;
    }
    InterlockedExchange(&bridge.initialised, 0);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;

    switch(reason)
    {
    case DLL_PROCESS_ATTACH: break;
    case DLL_PROCESS_DETACH:
        /* Only quit the loop; joining a thread from DllMain would deadlock, so
         * Prism.exe is expected to call PrismCaptureShutdown() first. */
        prism_screencast_quit();
        break;
    default: break;
    }
    return TRUE;
}
