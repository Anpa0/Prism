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
    unsigned     vendorId = 0;
    unsigned     deviceId = 0;
    LUID         luid {};
    bool         isSoftware = false;
};

struct RenderTimings
{
    double uploadMs  = 0.0;
    double drawMs    = 0.0;
    double presentMs = 0.0;
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
    bool EnsureSourceTexture(UINT width, UINT height);
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
    UINT m_sourceWidth      = 0;
    UINT m_sourceHeight     = 0;
    bool m_allowTearing     = false;
    bool m_flipModel        = false;

    RenderTimings m_timings;
};
