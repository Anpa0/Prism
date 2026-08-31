/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include "FrameMailbox.h"

/*
 * Synthetic frame source for bring-up and ReShade validation.
 *
 * Prism's phase-one milestone is "render something through D3D11 under Proton
 * and confirm ReShade hooks it", which should not require a working portal, a
 * running game, or even a Wayland session. This produces frames that go through
 * exactly the same mailbox, upload and present path as real capture, so a
 * monochrome or inverted test pattern proves the whole chain.
 *
 * It is a diagnostic aid, not a feature: it only runs when Prism.exe is started
 * with --test-pattern.
 */
class TestPatternSource
{
public:
    ~TestPatternSource();

    void Start(FrameMailbox& mailbox, uint32_t width, uint32_t height, unsigned fps);
    void Stop();
    bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void                Run();

    FrameMailbox*     m_mailbox = nullptr;
    HANDLE            m_thread  = nullptr;
    HANDLE            m_stopEvent = nullptr;
    uint32_t          m_width   = 1280;
    uint32_t          m_height  = 720;
    unsigned          m_fps     = 60;
    std::atomic<bool> m_running {false};
};
