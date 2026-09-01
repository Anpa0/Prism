/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FrameMailbox.h"

#include <memory>

FrameMailbox::FrameMailbox() :
    m_producer(std::make_unique<CapturedFrame>()), m_ready(std::make_unique<CapturedFrame>()),
    m_consumer(std::make_unique<CapturedFrame>())
{
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FrameMailbox::~FrameMailbox()
{
    if(m_event)
        CloseHandle(m_event);
}

void FrameMailbox::Publish(const void* pixels, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format,
                           uint64_t sequence, uint64_t ptsNs, uint64_t bridgeRecvNs)
{
    if(pixels == nullptr || width == 0 || height == 0 || pitch < width * 4u)
        return;

    CapturedFrame& target = *m_producer;

    /* Rows are packed to width*4 on the way in. PipeWire pitches are often
     * padded, and carrying the padding through would force the renderer to
     * deal with it on every upload for no benefit. */
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    const size_t needed   = rowBytes * height;
    if(target.pixels.size() != needed)
        target.pixels.resize(needed);

    const uint8_t* source      = static_cast<const uint8_t*>(pixels);
    uint8_t*       destination = target.pixels.data();
    if(pitch == rowBytes)
    {
        memcpy(destination, source, needed);
    }
    else
    {
        for(uint32_t row = 0; row < height; ++row)
        {
            memcpy(destination, source, rowBytes);
            source += pitch;
            destination += rowBytes;
        }
    }

    target.width      = width;
    target.height     = height;
    target.pitch      = static_cast<uint32_t>(rowBytes);
    target.format     = format;
    target.sequence   = sequence;
    target.ptsNs      = ptsNs;
    target.bridgeRecvNs = bridgeRecvNs;
    target.receivedUs = PrismNowUs();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_hasNew)
            m_staleDrops.fetch_add(1, std::memory_order_relaxed);
        m_producer.swap(m_ready);
        m_hasNew = true;
    }

    m_published.fetch_add(1, std::memory_order_relaxed);
    if(m_event)
        SetEvent(m_event);
}

const CapturedFrame* FrameMailbox::Acquire()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_hasNew)
        return nullptr;
    m_consumer.swap(m_ready);
    m_hasNew = false;
    return m_consumer.get();
}

void FrameMailbox::ResetCounters()
{
    m_staleDrops.store(0, std::memory_order_relaxed);
    m_published.store(0, std::memory_order_relaxed);
}
