/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Renderer.h"

#include "PrismCapture.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace
{
/* Fullscreen triangle. No vertex buffer, no input layout: the vertex shader
 * derives its own positions from SV_VertexID, which is one fewer object for
 * ReShade to reason about and one fewer state change per frame. */
const char kPresentShader[] = R"HLSL(
cbuffer PrismConstants : register(b0)
{
    float4 uvScaleOffset; // xy = scale, zw = offset
};

Texture2D    SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    VSOutput output;
    float2 uv       = float2((id << 1) & 2, id & 2);
    output.uv       = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv * uvScaleOffset.xy + uvScaleOffset.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    // No gamma, no tone mapping, no sharpening: the captured pixels reach the
    // back buffer exactly as PipeWire delivered them.
    return float4(SourceTexture.Sample(SourceSampler, uv).rgb, 1.0);
}
)HLSL";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

/* d3dcompiler_47 is resolved at run time rather than linked. ReShade needs it
 * too, so it is almost always present, but Prism should degrade instead of
 * refusing to start when it is not. */
D3DCompileFn LoadD3DCompile()
{
    static D3DCompileFn compile = [] () -> D3DCompileFn {
        static const wchar_t* candidates[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"};
        for(const wchar_t* name : candidates)
        {
            HMODULE module = LoadLibraryW(name);
            if(!module)
                continue;
            auto fn = PrismResolveProc<D3DCompileFn>(module, "D3DCompile");
            if(fn)
            {
                PrismLog("renderer: using %ls for shader compilation", name);
                return fn;
            }
            FreeLibrary(module);
        }
        return nullptr;
    }();
    return compile;
}

bool CompileStage(D3DCompileFn compile, const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob,
                  std::wstring& outError)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT    hr = compile(kPresentShader, sizeof(kPresentShader) - 1, "PrismPresent.hlsl", nullptr, nullptr,
                                  entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, outBlob.Put(), errors.Put());
    if(FAILED(hr))
    {
        const char* text = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error";
        PrismLog("renderer: %s (%s) failed to compile: %s", entryPoint, target, text);
        outError = L"Shader compilation failed; falling back to a 1:1 copy blit.";
        return false;
    }
    return true;
}
} // namespace

Renderer::~Renderer()
{
    Shutdown();
}

/* One place that turns a DXGI_ADAPTER_DESC1 into Prism's own record, so the
 * menu, the diagnostics panel and the active-device report never disagree. */
static AdapterInfo DescribeAdapter(const DXGI_ADAPTER_DESC1& desc)
{
    AdapterInfo info;
    info.description           = desc.Description;
    info.vendorId              = desc.VendorId;
    info.deviceId              = desc.DeviceId;
    info.dedicatedVideoMemory  = desc.DedicatedVideoMemory;
    info.dedicatedSystemMemory = desc.DedicatedSystemMemory;
    info.sharedSystemMemory    = desc.SharedSystemMemory;
    info.luid                  = desc.AdapterLuid;
    info.isSoftware            = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    return info;
}

std::vector<AdapterInfo> Renderer::EnumerateAdapters()
{
    std::vector<AdapterInfo> adapters;
    ComPtr<IDXGIFactory1>    factory;
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.PutVoid())))
        return adapters;

    ComPtr<IDXGIAdapter1> adapter;
    for(UINT index = 0; factory->EnumAdapters1(index, adapter.Put()) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 desc {};
        if(FAILED(adapter->GetDesc1(&desc)))
            continue;

        adapters.push_back(DescribeAdapter(desc));
    }
    return adapters;
}

bool Renderer::Initialize(HWND window, int adapterIndex)
{
    m_window = window;
    if(!CreateDeviceAndSwapChain(adapterIndex))
        return false;
    if(!CreateBackBufferViews())
        return false;
    CreatePresentPipeline(); /* soft failure: the copy path still works */
    return true;
}

bool Renderer::CreateDeviceAndSwapChain(int adapterIndex)
{
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), m_factory.PutVoid())))
    {
        m_lastError = L"CreateDXGIFactory1 failed. Under Proton this usually means DXVK is not loaded.";
        return false;
    }

    /* Adapter 0 is not automatically the right GPU: on a laptop it is often the
     * integrated part, and on a desktop it may be whichever card the compositor
     * is not using. Honour the explicit choice when there is one, otherwise ask
     * DXGI for the high-performance adapter and only then fall back. */
    ComPtr<IDXGIAdapter1> adapter;
    m_adapterIndex = -1;

    if(adapterIndex >= 0)
    {
        if(SUCCEEDED(m_factory->EnumAdapters1(static_cast<UINT>(adapterIndex), adapter.Put())))
            m_adapterIndex = adapterIndex;
        else
            PrismLog("renderer: adapter %d not present, falling back to the default", adapterIndex);
    }

    if(!adapter)
    {
        ComPtr<IDXGIFactory6> factory6;
        if(SUCCEEDED(m_factory->QueryInterface(__uuidof(IDXGIFactory6), factory6.PutVoid())))
        {
            if(SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                              __uuidof(IDXGIAdapter1), adapter.PutVoid())))
            {
                /* Map the chosen adapter back to an enumeration index so the UI
                 * can show which one is active. */
                DXGI_ADAPTER_DESC1 chosen {};
                adapter->GetDesc1(&chosen);
                ComPtr<IDXGIAdapter1> probe;
                for(UINT i = 0; m_factory->EnumAdapters1(i, probe.Put()) != DXGI_ERROR_NOT_FOUND; ++i)
                {
                    DXGI_ADAPTER_DESC1 desc {};
                    probe->GetDesc1(&desc);
                    if(desc.AdapterLuid.LowPart == chosen.AdapterLuid.LowPart &&
                       desc.AdapterLuid.HighPart == chosen.AdapterLuid.HighPart)
                    {
                        m_adapterIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }

    if(!adapter && SUCCEEDED(m_factory->EnumAdapters1(0, adapter.Put())))
        m_adapterIndex = 0;

    DXGI_ADAPTER_DESC1 desc {};
    if(adapter && SUCCEEDED(adapter->GetDesc1(&desc)))
    {
        m_activeAdapter      = DescribeAdapter(desc);
        m_adapterDescription = m_activeAdapter.description;
    }

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, m_device.Put(),
                                   &obtained, m_context.Put());
    if(FAILED(hr))
    {
        PrismLog("renderer: D3D11CreateDevice failed (0x%08lx), retrying without BGRA flag", hr);
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, m_device.Put(), &obtained,
                               m_context.Put());
    }
    if(FAILED(hr))
    {
        m_lastError = L"D3D11CreateDevice failed. Check that DXVK (or wined3d) is working in this prefix.";
        return false;
    }

    /* Tearing lets an uncapped present skip the compositor's queue, which is
     * where most of the residual latency lives. */
    ComPtr<IDXGIFactory5> factory5;
    if(SUCCEEDED(m_factory->QueryInterface(__uuidof(IDXGIFactory5), factory5.PutVoid())))
    {
        BOOL allow = FALSE;
        if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            m_allowTearing = allow != FALSE;
    }

    RECT client {};
    GetClientRect(m_window, &client);
    const UINT width  = std::max<UINT>(1, static_cast<UINT>(client.right - client.left));
    const UINT height = std::max<UINT>(1, static_cast<UINT>(client.bottom - client.top));

    DXGI_SWAP_CHAIN_DESC1 swapDesc {};
    swapDesc.Width            = width;
    swapDesc.Height           = height;
    swapDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM; /* matches BGRx exactly */
    swapDesc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount      = 2;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Scaling          = DXGI_SCALING_NONE;
    swapDesc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags            = m_allowTearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;

    hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &swapDesc, nullptr, nullptr,
                                           m_swapChain.Put());
    if(FAILED(hr))
    {
        PrismLog("renderer: flip-model swap chain rejected (0x%08lx), using the bitblt model", hr);
        m_allowTearing      = false;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapDesc.BufferCount = 1;
        swapDesc.Scaling    = DXGI_SCALING_STRETCH;
        swapDesc.Flags      = 0;
        hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &swapDesc, nullptr, nullptr,
                                               m_swapChain.Put());
    }
    else
    {
        m_flipModel = true;
    }

    if(FAILED(hr))
    {
        m_lastError = L"CreateSwapChainForHwnd failed.";
        return false;
    }

    /* Prism handles its own borderless fullscreen; DXGI's Alt+Enter would fight
     * with it and with ReShade's overlay. */
    m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);

    m_backBufferWidth  = width;
    m_backBufferHeight = height;

    PrismLog("renderer: D3D11 device on '%ls' [%ls %04x:%04x, %llu MB VRAM] "
             "(feature level 0x%x, flip=%d, tearing=%d)",
             m_adapterDescription.c_str(), m_activeAdapter.VendorName(), m_activeAdapter.vendorId,
             m_activeAdapter.deviceId,
             (unsigned long long)(m_activeAdapter.dedicatedVideoMemory / (1024ull * 1024ull)), obtained,
             m_flipModel ? 1 : 0, m_allowTearing ? 1 : 0);
    return true;
}

bool Renderer::CreateBackBufferViews()
{
    if(FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), m_backBuffer.PutVoid())))
    {
        m_lastError = L"Could not obtain the swap chain back buffer.";
        return false;
    }
    if(FAILED(m_device->CreateRenderTargetView(m_backBuffer.Get(), nullptr, m_backBufferView.Put())))
    {
        m_lastError = L"CreateRenderTargetView failed.";
        return false;
    }
    return true;
}

void Renderer::ReleaseBackBuffer()
{
    if(m_context)
    {
        ID3D11RenderTargetView* nullView = nullptr;
        m_context->OMSetRenderTargets(1, &nullView, nullptr);
    }
    m_backBufferView.Reset();
    m_backBuffer.Reset();
}

bool Renderer::CreatePresentPipeline()
{
    D3DCompileFn compile = LoadD3DCompile();
    if(!compile)
    {
        m_lastError = L"d3dcompiler_47.dll is missing, so Prism is using an unscaled copy blit. "
                      L"Install it in the prefix (winetricks d3dcompiler_47) for scaling modes.";
        PrismLog("renderer: no D3DCompile available, falling back to the copy path");
        return false;
    }

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    if(!CompileStage(compile, "VSMain", "vs_4_0", vsBlob, m_lastError))
        return false;
    if(!CompileStage(compile, "PSMain", "ps_4_0", psBlob, m_lastError))
        return false;

    if(FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                           m_vertexShader.Put())) ||
       FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                          m_pixelShader.Put())))
    {
        m_lastError = L"Shader object creation failed.";
        m_vertexShader.Reset();
        m_pixelShader.Reset();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc {};
    cbDesc.ByteWidth      = 16;
    cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if(FAILED(m_device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.Put())))
    {
        m_lastError = L"Constant buffer creation failed.";
        m_pixelShader.Reset();
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc {};
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    m_device->CreateSamplerState(&samplerDesc, m_pointSampler.Put());
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    m_device->CreateSamplerState(&samplerDesc, m_linearSampler.Put());

    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&blendDesc, m_blendState.Put());

    D3D11_RASTERIZER_DESC rasterDesc {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rasterDesc, m_rasterizerState.Put());

    m_lastError.clear();
    return true;
}

void Renderer::OnResize(UINT width, UINT height)
{
    if(!m_swapChain || width == 0 || height == 0)
        return;
    if(width == m_backBufferWidth && height == m_backBufferHeight)
        return;

    ReleaseBackBuffer();

    const UINT flags = m_allowTearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
    const HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
    if(FAILED(hr))
    {
        PrismLog("renderer: ResizeBuffers failed (0x%08lx)", hr);
        return;
    }

    m_backBufferWidth  = width;
    m_backBufferHeight = height;
    CreateBackBufferViews();
}

bool Renderer::EnsureSourceTexture(UINT width, UINT height, DXGI_FORMAT format)
{
    if(m_sourceTexture && width == m_sourceWidth && height == m_sourceHeight && format == m_sourceFormat)
        return true;

    m_sourceView.Reset();
    m_sourceTexture.Reset();

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_DYNAMIC;
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    if(FAILED(m_device->CreateTexture2D(&desc, nullptr, m_sourceTexture.Put())))
    {
        m_lastError = L"Could not create the capture texture.";
        return false;
    }
    if(FAILED(m_device->CreateShaderResourceView(m_sourceTexture.Get(), nullptr, m_sourceView.Put())))
    {
        m_lastError = L"Could not create the capture shader resource view.";
        m_sourceTexture.Reset();
        return false;
    }

    m_sourceWidth  = width;
    m_sourceHeight = height;
    m_sourceFormat = format;
    PrismLog("renderer: capture texture is now %ux%u %s", width, height,
             format == DXGI_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8" : "B8G8R8A8");
    return true;
}

bool Renderer::UploadFrame(const CapturedFrame& frame)
{
    if(!m_device || frame.width == 0 || frame.height == 0 || frame.pixels.empty())
        return false;

    const uint64_t started = PrismNowUs();

    /* Rather than converting RGBx to BGRx, Prism gives Direct3D a texture whose
     * channel order already matches the source. The sampler then returns the
     * same RGB either way, so neither the CPU nor the GPU touches a pixel.
     *
     * The one exception is the shader-free fallback: CopySubresourceRegion
     * cannot cross format families, so an RGBx source is swapped on the CPU by
     * the mailbox and arrives here as BGRA. */
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_pixelPath        = PixelPath::DirectBgra;
    if(frame.format == PRISM_FORMAT_RGBX || frame.format == PRISM_FORMAT_RGBA)
    {
        if(m_pixelShader)
        {
            format      = DXGI_FORMAT_R8G8B8A8_UNORM;
            m_pixelPath = PixelPath::DirectRgba;
        }
        else
        {
            m_pixelPath = PixelPath::CpuSwizzle;
        }
    }

    if(!EnsureSourceTexture(frame.width, frame.height, format))
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if(FAILED(m_context->Map(m_sourceTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    if(mapped.RowPitch == rowBytes)
    {
        memcpy(mapped.pData, frame.pixels.data(), rowBytes * frame.height);
    }
    else
    {
        const uint8_t* source      = frame.pixels.data();
        uint8_t*       destination = static_cast<uint8_t*>(mapped.pData);
        for(uint32_t row = 0; row < frame.height; ++row)
        {
            memcpy(destination, source, rowBytes);
            source += rowBytes;
            destination += mapped.RowPitch;
        }
    }
    m_context->Unmap(m_sourceTexture.Get(), 0);

    m_timings.uploadMs = static_cast<double>(PrismNowUs() - started) / 1000.0;
    return true;
}

void Renderer::ComputeMapping(DisplayMode mode, float outScaleOffset[4], bool& pointSampling) const
{
    const float backWidth   = static_cast<float>(std::max<UINT>(1, m_backBufferWidth));
    const float backHeight  = static_cast<float>(std::max<UINT>(1, m_backBufferHeight));
    const float sourceW     = static_cast<float>(std::max<UINT>(1, m_sourceWidth));
    const float sourceH     = static_cast<float>(std::max<UINT>(1, m_sourceHeight));

    float destWidth  = backWidth;
    float destHeight = backHeight;
    pointSampling    = false;

    switch(mode)
    {
    case DisplayMode::OriginalSize:
        destWidth     = sourceW;
        destHeight    = sourceH;
        pointSampling = true;
        break;
    case DisplayMode::Stretch:
        destWidth  = backWidth;
        destHeight = backHeight;
        break;
    case DisplayMode::IntegerScale:
    {
        const float fit   = std::min(backWidth / sourceW, backHeight / sourceH);
        float       scale = std::floor(fit);
        if(scale < 1.0f)
            scale = 1.0f; /* source larger than the window: show it 1:1 cropped */
        destWidth     = sourceW * scale;
        destHeight    = sourceH * scale;
        pointSampling = true;
        break;
    }
    case DisplayMode::Fit:
    default:
    {
        const float scale = std::min(backWidth / sourceW, backHeight / sourceH);
        destWidth         = sourceW * scale;
        destHeight        = sourceH * scale;
        /* An exact 1:1 fit deserves point sampling too - no filter, no blur. */
        pointSampling = (std::abs(scale - 1.0f) < 0.0001f);
        break;
    }
    }

    const float destX = (backWidth - destWidth) * 0.5f;
    const float destY = (backHeight - destHeight) * 0.5f;

    /* Map back-buffer UV to source UV: uv_src = uv_dst * scale + offset. */
    outScaleOffset[0] = backWidth / destWidth;
    outScaleOffset[1] = backHeight / destHeight;
    outScaleOffset[2] = -destX / destWidth;
    outScaleOffset[3] = -destY / destHeight;
}

bool Renderer::Present(DisplayMode mode, bool vsync)
{
    if(!m_swapChain || !m_backBufferView)
        return false;

    const uint64_t drawStarted = PrismNowUs();

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_context->ClearRenderTargetView(m_backBufferView.Get(), black);

    if(m_sourceTexture)
    {
        if(m_pixelShader)
        {
            float scaleOffset[4];
            bool  pointSampling = false;
            ComputeMapping(mode, scaleOffset, pointSampling);

            D3D11_MAPPED_SUBRESOURCE mapped {};
            if(SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, scaleOffset, sizeof(scaleOffset));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            D3D11_VIEWPORT viewport {};
            viewport.Width    = static_cast<float>(m_backBufferWidth);
            viewport.Height   = static_cast<float>(m_backBufferHeight);
            viewport.MaxDepth = 1.0f;

            ID3D11RenderTargetView* rtv         = m_backBufferView.Get();
            ID3D11ShaderResourceView* srv       = m_sourceView.Get();
            ID3D11SamplerState*     sampler     = pointSampling ? m_pointSampler.Get() : m_linearSampler.Get();
            ID3D11Buffer*           constants   = m_constantBuffer.Get();

            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            m_context->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffffu);
            m_context->RSSetState(m_rasterizerState.Get());
            m_context->RSSetViewports(1, &viewport);
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &srv);
            m_context->PSSetSamplers(0, 1, &sampler);
            m_context->PSSetConstantBuffers(0, 1, &constants);
            m_context->Draw(3, 0);
        }
        else
        {
            /* No shader compiler in the prefix. A straight subresource copy of
             * the overlapping region still gives a correct, unfiltered 1:1
             * image, which is enough for ReShade validation. */
            const UINT copyWidth  = std::min(m_sourceWidth, m_backBufferWidth);
            const UINT copyHeight = std::min(m_sourceHeight, m_backBufferHeight);
            if(copyWidth > 0 && copyHeight > 0 && m_backBuffer)
            {
                D3D11_BOX box {};
                box.right  = copyWidth;
                box.bottom = copyHeight;
                box.back   = 1;
                const UINT destX = (m_backBufferWidth - copyWidth) / 2;
                const UINT destY = (m_backBufferHeight - copyHeight) / 2;
                m_context->CopySubresourceRegion(m_backBuffer.Get(), 0, destX, destY, 0, m_sourceTexture.Get(), 0,
                                                 &box);
            }
        }
    }

    m_timings.drawMs = static_cast<double>(PrismNowUs() - drawStarted) / 1000.0;

    const uint64_t presentStarted = PrismNowUs();
    const UINT     syncInterval   = vsync ? 1u : 0u;
    const UINT     presentFlags   = (!vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;

    /* Everything ReShade does happens inside this call. */
    const HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    m_timings.presentMs = static_cast<double>(PrismNowUs() - presentStarted) / 1000.0;

    if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        PrismLog("renderer: device lost (0x%08lx)", hr);
        return false;
    }
    return true;
}

bool Renderer::Recreate(int adapterIndex)
{
    HWND window = m_window;
    Shutdown();
    m_window = window;
    return Initialize(window, adapterIndex);
}

void Renderer::Shutdown()
{
    if(m_context)
        m_context->ClearState();

    m_sourceView.Reset();
    m_sourceTexture.Reset();
    m_rasterizerState.Reset();
    m_blendState.Reset();
    m_linearSampler.Reset();
    m_pointSampler.Reset();
    m_constantBuffer.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    ReleaseBackBuffer();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_factory.Reset();

    m_sourceWidth      = 0;
    m_sourceHeight     = 0;
    m_backBufferWidth  = 0;
    m_backBufferHeight = 0;
    m_flipModel        = false;
    m_allowTearing     = false;
}
