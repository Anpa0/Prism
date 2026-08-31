/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"

/* One captured frame, already detached from PipeWire's memory. */
struct CapturedFrame
{
    std::vector<uint8_t> pixels;
    uint32_t             width    = 0;
    uint32_t             height   = 0;
    uint32_t             pitch    = 0;
    uint32_t             format   = 0;
    uint64_t             sequence = 0;
    uint64_t             ptsNs        = 0; /* PipeWire stamp, 0 when unknown */
    uint64_t             bridgeRecvNs = 0; /* bridge clock when it arrived   */
    uint64_t             receivedUs   = 0; /* Prism clock at copy time       */
};

/*
 * Latest-frame mailbox.
 *
 * Prism is a viewer, not a recorder: when the renderer is busy the right answer
 * is always "give me the newest frame and forget the rest". Three buffers
 * circulate between the capture thread and the render thread, so neither ever
 * waits on the other and no queue can grow:
 *
 *   producer buffer  - written by the capture thread, no lock held
 *   ready buffer     - the newest complete frame, swapped under a tiny lock
 *   consumer buffer  - owned by the render thread while it uploads
 *
 * Publishing over an unread ready buffer is a stale drop and is counted.
 */
class FrameMailbox
{
public:
    FrameMailbox();
    ~FrameMailbox();

    /* Capture thread. Copies `pixels` into the producer buffer and publishes. */
    void Publish(const void* pixels, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format,
                 uint64_t sequence, uint64_t ptsNs, uint64_t bridgeRecvNs);

    /* Render thread. Returns the newest unseen frame, or nullptr if there is
     * nothing new. The pointer stays valid until the next Acquire(). */
    const CapturedFrame* Acquire();

    /* Signalled on every Publish() so the render thread can sleep instead of
     * spinning. Auto-reset. */
    HANDLE FrameEvent() const { return m_event; }

    uint64_t StaleDrops() const { return m_staleDrops.load(std::memory_order_relaxed); }
    uint64_t Published() const { return m_published.load(std::memory_order_relaxed); }
    void     ResetCounters();

private:
    std::mutex                     m_mutex;
    std::unique_ptr<CapturedFrame> m_producer;
    std::unique_ptr<CapturedFrame> m_ready;
    std::unique_ptr<CapturedFrame> m_consumer;
    bool                           m_hasNew = false;

    HANDLE                m_event = nullptr;
    std::atomic<uint64_t> m_staleDrops {0};
    std::atomic<uint64_t> m_published {0};
};
