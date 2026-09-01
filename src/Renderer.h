/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include "FrameMailbox.h"
#include "Settings.h"

#include <d3d11.h>
#include <dxgi1_6.h>

struct AdapterInfo
{
    std::wstring description;
    unsigned     vendorId        = 0;
    unsigned     deviceId        = 0;
    uint64_t     dedicatedVideoMemory  = 0;
    uint64_t     dedicatedSystemMemory = 0;
    uint64_t     sharedSystemMemory    = 0;
    LUID         luid {};
    bool         isSoftware = false;

    /* PCI vendor IDs, so the UI can label an adapter without string matching. */
    static constexpr unsigned kVendorAmd    = 0x1002;
    static constexpr unsigned kVendorNvidia = 0x10DE;
    static constexpr unsigned kVendorIntel  = 0x8086;

    const wchar_t* VendorName() const
    {
        switch(vendorId)
        {
        case kVendorAmd: return L"AMD";
        case kVendorNvidia: return L"NVIDIA";
        case kVendorIntel: return L"Intel";
        default: return L"Unknown";
        }
    }
};

struct RenderTimings
{
    double uploadMs  = 0.0;
    double drawMs    = 0.0;
    double presentMs = 0.0;
};

/* How the captured bytes reach the back buffer. Prism prefers a texture format
 * that matches the source exactly, so the usual answer is "no conversion". */
enum class PixelPath
{
    DirectBgra, /* PipeWire BGRx/BGRA -> DXGI_FORMAT_B8G8R8A8_UNORM */
    DirectRgba, /* PipeWire RGBx/RGBA -> DXGI_FORMAT_R8G8B8A8_UNORM */
    CpuSwizzle, /* only in the no-shader fallback, where the blit cannot swap */
};

/*
 * Direct3D 11 presentation host.
 *
 * The pipeline is deliberately three stages long - upload, one fullscreen
 * triangle, Present - because everything ReShade does happens inside that
 * Present call. Adding stages here would only put Prism's own processing
 * between the capture and the effect chain, which is the opposite of the point.
 */
class Renderer
{
public:
    ~Renderer();

    static std::vector<AdapterInfo> EnumerateAdapters();

    bool Initialize(HWND window, int adapterIndex);
    void Shutdown();
    bool IsReady() const { return m_device && m_swapChain; }

    /* Rebuilds the device on a different adapter, keeping the same window. */
    bool Recreate(int adapterIndex);

    void OnResize(UINT width, UINT height);

    bool UploadFrame(const CapturedFrame& frame);

    PixelPath           CurrentPixelPath() const { return m_pixelPath; }
    const AdapterInfo&  ActiveAdapter() const { return m_activeAdapter; }
    bool Present(DisplayMode mode, bool vsync);

    const std::wstring& AdapterDescription() const { return m_adapterDescription; }
    int                 AdapterIndex() const { return m_adapterIndex; }
    const std::wstring& LastError() const { return m_lastError; }
    bool                UsesShaderPath() const { return m_pixelShader.Get() != nullptr; }
    const RenderTimings& Timings() const { return m_timings; }
    UINT SourceWidth() const { return m_sourceWidth; }
    UINT SourceHeight() const { return m_sourceHeight; }

private:
    bool CreateDeviceAndSwapChain(int adapterIndex);
    bool CreateBackBufferViews();
    bool CreatePresentPipeline();
    bool EnsureSourceTexture(UINT width, UINT height, DXGI_FORMAT format);
    void ComputeMapping(DisplayMode mode, float outScaleOffset[4], bool& pointSampling) const;
    void ReleaseBackBuffer();

    HWND m_window = nullptr;
    int  m_adapterIndex = -1;
    std::wstring m_adapterDescription;
    std::wstring m_lastError;

    ComPtr<IDXGIFactory2>       m_factory;
    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1>     m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBufferView;
    ComPtr<ID3D11Texture2D>        m_backBuffer;

    ComPtr<ID3D11Texture2D>          m_sourceTexture;
    ComPtr<ID3D11ShaderResourceView> m_sourceView;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader>  m_pixelShader;
    ComPtr<ID3D11Buffer>       m_constantBuffer;
    ComPtr<ID3D11SamplerState> m_pointSampler;
    ComPtr<ID3D11SamplerState> m_linearSampler;
    ComPtr<ID3D11BlendState>   m_blendState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;

    UINT m_backBufferWidth  = 0;
    UINT m_backBufferHeight = 0;
    UINT        m_sourceWidth  = 0;
    UINT        m_sourceHeight = 0;
    DXGI_FORMAT m_sourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    PixelPath   m_pixelPath    = PixelPath::DirectBgra;
    AdapterInfo m_activeAdapter;
    bool m_allowTearing     = false;
    bool m_flipModel        = false;

    RenderTimings m_timings;
};
