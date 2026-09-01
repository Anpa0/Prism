/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestPattern.h"

#include "PrismCapture.h"

TestPatternSource::~TestPatternSource()
{
    Stop();
}

void TestPatternSource::Start(FrameMailbox& mailbox, uint32_t width, uint32_t height, unsigned fps)
{
    if(m_running.load(std::memory_order_relaxed))
        return;

    m_mailbox   = &mailbox;
    m_width     = width;
    m_height    = height;
    m_fps       = fps ? fps : 60;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = CreateThread(nullptr, 0, &TestPatternSource::ThreadProc, this, 0, nullptr);
    if(!m_thread)
        m_running.store(false, std::memory_order_relaxed);
}

void TestPatternSource::Stop()
{
    if(!m_running.load(std::memory_order_relaxed))
        return;

    m_running.store(false, std::memory_order_relaxed);
    if(m_stopEvent)
        SetEvent(m_stopEvent);
    if(m_thread)
    {
        WaitForSingleObject(m_thread, 2000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if(m_stopEvent)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

DWORD WINAPI TestPatternSource::ThreadProc(LPVOID param)
{
    static_cast<TestPatternSource*>(param)->Run();
    return 0;
}

void TestPatternSource::Run()
{
    /* BGRA byte order, matching what the bridge delivers, so the renderer sees
     * no difference between this and a real PipeWire frame. */
    std::vector<uint8_t> pixels(static_cast<size_t>(m_width) * m_height * 4u);
    const DWORD          intervalMs = static_cast<DWORD>(1000u / m_fps);
    uint64_t             sequence   = 0;

    while(m_running.load(std::memory_order_relaxed))
    {
        const uint32_t phase = static_cast<uint32_t>(sequence * 2u);

        for(uint32_t y = 0; y < m_height; ++y)
        {
            uint8_t* row = pixels.data() + static_cast<size_t>(y) * m_width * 4u;
            for(uint32_t x = 0; x < m_width; ++x)
            {
                /* Colour bars over a horizontal gradient, plus a scrolling band.
                 * Saturated primaries make a monochrome or inversion shader
                 * obvious at a glance; the gradient makes banding obvious. */
                const uint32_t bar = (x * 8u) / m_width;
                uint8_t        r   = (bar & 1u) ? 255 : 0;
                uint8_t        g   = (bar & 2u) ? 255 : 0;
                uint8_t        b   = (bar & 4u) ? 255 : 0;

                const uint8_t gradient = static_cast<uint8_t>((x * 255u) / (m_width ? m_width : 1u));
                if(y > m_height / 2u)
                {
                    r = g = b = gradient;
                }
                if(((y + phase) % m_height) < 8u)
                {
                    r = g = b = 255;
                }

                row[x * 4u + 0] = b;
                row[x * 4u + 1] = g;
                row[x * 4u + 2] = r;
                row[x * 4u + 3] = 255;
            }
        }

        m_mailbox->Publish(pixels.data(), m_width, m_height, m_width * 4u, PRISM_FORMAT_BGRX, ++sequence, 0, 0,
                           pixels.size());

        if(WaitForSingleObject(m_stopEvent, intervalMs ? intervalMs : 1) == WAIT_OBJECT_0)
            break;
    }
}
