/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* Minimal COM smart pointer. MinGW has no wrl/client.h and no C++/WinRT, and
 * Prism needs about six lines of what they offer. */
template<typename T> class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr& other) : m_ptr(other.m_ptr)
    {
        if(m_ptr)
            m_ptr->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other)
    {
        if(this != &other)
        {
            if(other.m_ptr)
                other.m_ptr->AddRef();
            Reset();
            m_ptr = other.m_ptr;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if(this != &other)
        {
            Reset();
            m_ptr       = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void Reset()
    {
        if(m_ptr)
        {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

    T*  Get() const { return m_ptr; }
    T** Put()
    {
        Reset();
        return &m_ptr;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }

    T*       operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T* m_ptr = nullptr;
};

/* GetProcAddress returns FARPROC, and casting that straight to a typed pointer
 * trips -Wcast-function-type on GCC. The round trip through void* is the
 * portable way to say "yes, I mean it". */
template<typename T> T PrismResolveProc(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

/* Monotonic microseconds from QueryPerformanceCounter. */
inline uint64_t PrismNowUs()
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart * 1000000ull / static_cast<uint64_t>(frequency.QuadPart));
}

/* Directory holding Prism.exe, with a trailing backslash. ReShade, Prism.ini
 * and PrismCapture.dll all live here. */
const std::wstring& PrismModuleDirectory();

void PrismLog(const char* format, ...);
