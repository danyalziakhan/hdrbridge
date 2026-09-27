// dwmprobe: measure what DWM sends to the display for a float scRGB swap
// chain. Shows a row of patches with known linear values through an
// R16G16B16A16_FLOAT flip model swap chain in scRGB, then reads the composed
// desktop back through DXGI desktop duplication, which is exactly what DWM
// hands to the display, and prints the 8-bit code each patch arrived as.
//
// Build: cl /nologo /EHsc /O2 /std:c++17 dwmprobe.cpp d3d11.lib dxgi.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

static const int PATCHES = 24;
static const int PW = 40, PH = 120;

static uint16_t to_half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFF;
    if (exp <= 0)
        return static_cast<uint16_t>(sign);
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (exp << 10) | ((man + 0x1000) >> 13));
}

static float patch_value(int i)
{
    // Denser near black, where sRGB and a pure 2.2 power differ most.
    const float t = static_cast<float>(i) / (PATCHES - 1);
    return t * t;
}

int main()
{
    // Window rectangles in physical pixels, to match the duplicated desktop.
    SetProcessDPIAware();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dwmprobe";
    RegisterClassW(&wc);
    const int W = PATCHES * PW, H = PH;
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_POPUP, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"dwmprobe", L"dwmprobe", WS_POPUP | WS_VISIBLE,
        100, 100, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory2 *factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));

    // Render on the adapter that owns the display, so duplication works too.
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND)
            break;
        adapter->Release();
        adapter = nullptr;
    }
    if (output == nullptr)
    {
        std::printf("no output found\n");
        return 1;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    std::printf("adapter with the display: %ls\n", ad.Description);

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc1)))
    {
        std::printf("swap chain creation failed\n");
        return 1;
    }
    IDXGISwapChain3 *sc = nullptr;
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc));
    const HRESULT hr_cs = sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    std::printf("SetColorSpace1(scRGB) = 0x%08lX\n", static_cast<unsigned long>(hr_cs));

    std::vector<uint16_t> pixels(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const uint16_t v = to_half(patch_value(x / PW));
            uint16_t *p = &pixels[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = to_half(1.0f);
        }

    IDXGIOutput1 *out1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&out1));
    IDXGIOutputDuplication *dup = nullptr;
    const HRESULT hr_dup = out1->DuplicateOutput(dev, &dup);
    if (FAILED(hr_dup))
    {
        std::printf("DuplicateOutput failed 0x%08lX\n", static_cast<unsigned long>(hr_dup));
        return 1;
    }
    DXGI_OUTDUPL_DESC dd;
    dup->GetDesc(&dd);
    std::printf("duplication format %u\n", static_cast<unsigned>(dd.ModeDesc.Format));

    // Present for a while so DWM has composed the window, then grab frames.
    for (int i = 0; i < 30; ++i)
    {
        ID3D11Texture2D *bb = nullptr;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb));
        ctx->UpdateSubresource(bb, 0, nullptr, pixels.data(), W * 8, 0);
        bb->Release();
        sc->Present(1, 0);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    bool done = false;
    for (int attempt = 0; attempt < 60 && !done; ++attempt)
    {
        ID3D11Texture2D *bb = nullptr;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb));
        ctx->UpdateSubresource(bb, 0, nullptr, pixels.data(), W * 8, 0);
        bb->Release();
        sc->Present(1, 0);

        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = nullptr;
        if (FAILED(dup->AcquireNextFrame(200, &fi, &res)))
            continue;
        ID3D11Texture2D *tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ID3D11Texture2D *staging = nullptr;
        dev->CreateTexture2D(&td, nullptr, &staging);
        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (attempt >= 10 && SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            std::printf("format %u, patch: linear in -> code out (sRGB piecewise, pure 2.2)\n", static_cast<unsigned>(td.Format));
            for (int i = 0; i < PATCHES; ++i)
            {
                const int x = wr.left + i * PW + PW / 2, y = wr.top + PH / 2;
                const float v = patch_value(i);
                float code;
                if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    const uint16_t *p = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch) + x * 4;
                    uint32_t h = p[1];
                    const uint32_t e = (h >> 10) & 0x1F, mm = h & 0x3FF;
                    const float lin = e == 0 ? std::ldexp(static_cast<float>(mm), -24) : std::ldexp(static_cast<float>(mm | 0x400), static_cast<int>(e) - 25);
                    code = lin; // linear readback, printed as is
                }
                else
                {
                    const uint8_t *p = static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch + x * 4;
                    code = p[1];
                }
                const float srgb = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
                std::printf("  %2d  %.5f -> %8.3f   (%6.1f, %6.1f)\n", i, v, code, srgb * 255, std::pow(v, 1 / 2.2f) * 255);
            }
            ctx->Unmap(staging, 0);
            done = true;
        }
        staging->Release();
        tex->Release();
        res->Release();
        dup->ReleaseFrame();
    }
    if (!done)
        std::printf("no frame captured\n");
    DestroyWindow(hwnd);
    return 0;
}
