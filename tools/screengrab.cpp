// screengrab: save what the display is actually being sent, on a hotkey.
//
// Runs in the background. Press F8 and it grabs the composed desktop through
// DXGI desktop duplication, which is the frame DWM hands to the display after
// every effect, add-on and color conversion, and writes it as a BMP to a
// screens folder beside the executable, or to the folder given on the command
// line. Ctrl+Alt+F8 quits. With --once it grabs a single frame and exits.
//
// Build: cl /nologo /EHsc /O2 /std:c++17 screengrab.cpp d3d11.lib dxgi.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static bool grab(const std::filesystem::path &dir)
{
    IDXGIFactory1 *factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND)
            break;
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (output == nullptr)
        return false;

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    IDXGIOutput1 *out1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&out1));
    IDXGIOutputDuplication *dup = nullptr;
    bool ok = false;
    if (SUCCEEDED(out1->DuplicateOutput(dev, &dup)))
    {
        // The first frame after duplication starts can be stale; take a later one.
        for (int attempt = 0; attempt < 20 && !ok; ++attempt)
        {
            DXGI_OUTDUPL_FRAME_INFO fi;
            IDXGIResource *res = nullptr;
            if (FAILED(dup->AcquireNextFrame(500, &fi, &res)))
                continue;
            if (attempt < 2 || fi.LastPresentTime.QuadPart == 0)
            {
                res->Release();
                dup->ReleaseFrame();
                continue;
            }
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
            if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM && SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
            {
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[64];
                swprintf_s(name, L"screen_%04u%02u%02u_%02u%02u%02u_%03u.bmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
                const std::filesystem::path path = dir / name;

                const DWORD row = td.Width * 3, pad = (4 - row % 4) % 4, img = (row + pad) * td.Height;
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + img;
                ih.biSize = sizeof(ih);
                ih.biWidth = static_cast<LONG>(td.Width);
                ih.biHeight = static_cast<LONG>(td.Height);
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                FILE *f = _wfopen(path.c_str(), L"wb");
                if (f)
                {
                    std::fwrite(&fh, sizeof(fh), 1, f);
                    std::fwrite(&ih, sizeof(ih), 1, f);
                    std::vector<uint8_t> line(row + pad, 0);
                    for (UINT y = td.Height; y-- > 0;)
                    {
                        const uint8_t *src = static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                        for (UINT x = 0; x < td.Width; ++x)
                        {
                            line[x * 3 + 0] = src[x * 4 + 0];
                            line[x * 3 + 1] = src[x * 4 + 1];
                            line[x * 3 + 2] = src[x * 4 + 2];
                        }
                        std::fwrite(line.data(), 1, line.size(), f);
                    }
                    std::fclose(f);
                    std::printf("saved %ls\n", path.c_str());
                    ok = true;
                }
                ctx->Unmap(staging, 0);
            }
            else if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                std::printf("unexpected duplication format %u\n", static_cast<unsigned>(td.Format));
                attempt = 20;
            }
            staging->Release();
            tex->Release();
            res->Release();
            dup->ReleaseFrame();
        }
        dup->Release();
    }
    else
    {
        std::printf("desktop duplication is not available right now\n");
    }
    out1->Release();
    output->Release();
    ctx->Release();
    dev->Release();
    adapter->Release();
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    SetProcessDPIAware();
    bool once = false;
    std::filesystem::path dir = std::filesystem::path(argv[0]).parent_path() / L"screens";
    for (int i = 1; i < argc; ++i)
    {
        if (std::wstring(argv[i]) == L"--once")
            once = true;
        else
            dir = argv[i];
    }
    std::filesystem::create_directories(dir);
    if (once)
        return grab(dir) ? 0 : 1;

    if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F8) ||
        !RegisterHotKey(nullptr, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8))
    {
        std::printf("could not register F8, is another instance running?\n");
        return 1;
    }
    std::printf("screengrab: F8 saves the screen to %ls, Ctrl+Alt+F8 quits\n", dir.c_str());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message != WM_HOTKEY)
            continue;
        if (msg.wParam == 2)
            break;
        if (!grab(dir))
        {
            MessageBeep(MB_ICONHAND);
            std::printf("grab failed\n");
        }
        else
        {
            MessageBeep(MB_OK);
        }
    }
    return 0;
}
