// HDR Bridge: a ReShade add-on that lets a game switch on its HDR output on a
// plain SDR monitor, tone maps that HDR frame to SDR for ReShade's effects
// (tonemap.cpp), and has Windows show the result. It also saves the game's
// back buffer at full precision on a hotkey.
//
// Load it from DllMain so the hooks are in place before the game asks what
// the display supports. In ReShade.ini:
//
//   [ADDON]
//   LoadFromDllMain=hdrbridge.addon64
//
// Settings live in the same file under [HDRBRIDGE]; see README.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>

// Dear ImGui for the settings panel. ReShade hands the functions over at
// registration, so only the header is needed, at the version ReShade uses.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "tonemap.h"
#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

// trace.cpp
void trace_install();
void *trace_wrap_nvapi(unsigned id, void *fn);

extern "C" __declspec(dllexport) const char *NAME = "HDR Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Lets a game run its HDR output on an SDR monitor, and saves HDR frames on a hotkey.";

// Settings

static bool  g_spoof_dxgi   = true;
static bool  g_spoof_nvapi  = true;
static bool  g_hide_nvapi_from_game = false;
static bool  g_trace = false;
static std::atomic<bool> g_borderless{true};
static float g_max_nits     = 4000.0f;
static float g_min_nits     = 0.005f;
static float g_max_fall     = 4000.0f;
// Scroll Lock rather than a function key: F9 is quick load in a lot of games.
static std::atomic<int> g_capture_key{VK_SCROLL};

static std::filesystem::path g_output_dir;

// False while the ReShade that registered this add-on has been unloaded and
// no new one has taken it back; every call into ReShade waits for it.
std::atomic<bool> g_reshade_alive{true};

static void logf(reshade::log::level level, const char *fmt, ...)
{
    if (!g_reshade_alive)
        return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    reshade::log::message(level, buf);
}
#define LOG(...)  logf(reshade::log::level::info, __VA_ARGS__)
#define WARN(...) logf(reshade::log::level::warning, __VA_ARGS__)

static void read_settings()
{
    reshade::get_config_value(nullptr, "HDRBRIDGE", "SpoofDXGI", g_spoof_dxgi);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "SpoofNVAPI", g_spoof_nvapi);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "HideNVAPIFromGame", g_hide_nvapi_from_game);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "TraceGameCalls", g_trace);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "MaxLuminance", g_max_nits);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "MinLuminance", g_min_nits);
    reshade::get_config_value(nullptr, "HDRBRIDGE", "MaxFrameAverageLuminance", g_max_fall);
    int capture_key = g_capture_key;
    reshade::get_config_value(nullptr, "HDRBRIDGE", "CaptureKey", capture_key);
    g_capture_key = capture_key;
    bool borderless = g_borderless;
    reshade::get_config_value(nullptr, "HDRBRIDGE", "BorderlessFullscreen", borderless);
    g_borderless = borderless;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_output_dir = std::filesystem::path(exe).parent_path() / L"HDRBridge Captures";

    char dir[MAX_PATH] = {};
    size_t size = sizeof(dir);
    if (reshade::get_config_value(nullptr, "HDRBRIDGE", "OutputPath", dir, &size) && dir[0] != '\0')
        g_output_dir = std::filesystem::u8path(dir);

    LOG("settings: SpoofDXGI=%d SpoofNVAPI=%d HideNVAPIFromGame=%d TraceGameCalls=%d BorderlessFullscreen=%d MaxLuminance=%.1f MinLuminance=%.4f MaxFrameAverageLuminance=%.1f CaptureKey=0x%X OutputPath=%s",
        g_spoof_dxgi, g_spoof_nvapi, g_hide_nvapi_from_game, g_trace, g_borderless.load(), g_max_nits, g_min_nits, g_max_fall, g_capture_key.load(), g_output_dir.u8string().c_str());
}

// BT.2020 primaries and D65. Reporting the widest gamut means the game has no
// reason to compress color toward the monitor's real primaries before output.
static const float PRIM_R[2] = { 0.708f, 0.292f };
static const float PRIM_G[2] = { 0.170f, 0.797f };
static const float PRIM_B[2] = { 0.131f, 0.046f };
static const float WHITE[2]  = { 0.3127f, 0.3290f };

// DXGI: IDXGIOutput6::GetDesc1 is where a game reads the display's color
// space and luminance range. Hooked on the function, not a vtable, so every
// output object in the process goes through it.

// Which module a return address belongs to, so the game's own queries can be
// told apart from those DXGI, the driver and other add-ons make in the same
// process.
static HMODULE module_from_address(void *address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(address), &module);
    return module;
}

static std::wstring caller_name(void *address)
{
    wchar_t name[MAX_PATH] = L"?";
    if (HMODULE module = module_from_address(address))
        GetModuleFileNameW(module, name, MAX_PATH);
    return std::filesystem::path(name).filename().wstring();
}

using PFN_GetDesc1 = HRESULT(STDMETHODCALLTYPE *)(IDXGIOutput6 *, DXGI_OUTPUT_DESC1 *);
static PFN_GetDesc1 real_GetDesc1 = nullptr;
static std::atomic<int> g_getdesc1_calls{0};

static HRESULT STDMETHODCALLTYPE hook_GetDesc1(IDXGIOutput6 *self, DXGI_OUTPUT_DESC1 *desc)
{
    void *const ret = _ReturnAddress();
    const HRESULT hr = real_GetDesc1(self, desc);
    const int n = ++g_getdesc1_calls;

    if (SUCCEEDED(hr) && desc != nullptr)
    {
        if (n <= 12)
            LOG("IDXGIOutput6::GetDesc1 call %d on %ls from %ls: real ColorSpace=%d BitsPerColor=%u MaxLuminance=%.1f%s",
                n, desc->DeviceName, caller_name(ret).c_str(), static_cast<int>(desc->ColorSpace), desc->BitsPerColor, desc->MaxLuminance,
                g_spoof_dxgi ? ", reporting HDR10" : "");

        if (g_spoof_dxgi)
        {
            desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            desc->BitsPerColor = 10;
            desc->RedPrimary[0]   = PRIM_R[0]; desc->RedPrimary[1]   = PRIM_R[1];
            desc->GreenPrimary[0] = PRIM_G[0]; desc->GreenPrimary[1] = PRIM_G[1];
            desc->BluePrimary[0]  = PRIM_B[0]; desc->BluePrimary[1]  = PRIM_B[1];
            desc->WhitePoint[0]   = WHITE[0];  desc->WhitePoint[1]   = WHITE[1];
            desc->MinLuminance = g_min_nits;
            desc->MaxLuminance = g_max_nits;
            desc->MaxFullFrameLuminance = g_max_fall;
        }
    }
    return hr;
}

static void install_dxgi_output_hook()
{
    // The system DXGI by full path, so a ReShade dxgi.dll in the game folder is
    // not the module the output object comes from. Take it from memory when it
    // is already there: a proxy such as OptiScaler hooks LoadLibrary, hands
    // back itself for this path, and runs its overlay setup on that call. From
    // this thread that happens before the game has created anything, and in
    // one game it left OptiScaler's menu unable to open.
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring path = std::wstring(sys) + L"\\dxgi.dll";
    HMODULE dxgi = GetModuleHandleW(path.c_str());
    if (dxgi == nullptr)
        dxgi = LoadLibraryW(path.c_str());
    {
        wchar_t got[MAX_PATH] = {};
        GetModuleFileNameW(dxgi, got, MAX_PATH);
        LOG("reading the DXGI output from %ls", got);
    }
    if (dxgi == nullptr)
    {
        WARN("could not load %ls", path.c_str());
        return;
    }

    using PFN_CreateFactory1 = HRESULT(WINAPI *)(REFIID, void **);
    auto create = reinterpret_cast<PFN_CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1 *factory = nullptr;
    if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
    {
        WARN("CreateDXGIFactory1 failed");
        return;
    }

    IDXGIOutput6 *output6 = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT a = 0; output6 == nullptr && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        IDXGIOutput *output = nullptr;
        for (UINT o = 0; output6 == nullptr && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void **>(&output6));
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    if (output6 == nullptr)
    {
        WARN("no IDXGIOutput6 found, DXGI HDR reporting not installed");
        return;
    }

    // IUnknown 3, IDXGIObject 4, IDXGIOutput 12, IDXGIOutput1 4, IDXGIOutput2..5
    // one each, so GetDesc1 is slot 27.
    void **vtbl = *reinterpret_cast<void ***>(output6);
    void *target = vtbl[27];
    output6->Release();

    if (MH_CreateHook(target, reinterpret_cast<void *>(&hook_GetDesc1), reinterpret_cast<void **>(&real_GetDesc1)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        WARN("failed to hook IDXGIOutput6::GetDesc1");
        return;
    }
    LOG("hooked IDXGIOutput6::GetDesc1");
}

// Windows display configuration: DisplayConfigGetDeviceInfo is where a game
// asks whether HDR is switched on for the display, as opposed to what the
// output can describe. On Windows 11 an SDR display with Auto Color
// Management answers "advanced color enabled" here but also sets "wide
// color enforced", which means HDR is off, so a careful game still says
// unavailable. Both the original query and its newer INFO_2 form are answered
// as an HDR display in HDR mode.

static const UINT32 DC_GET_ADVANCED_COLOR_INFO   = 9;
static const UINT32 DC_GET_ADVANCED_COLOR_INFO_2 = 15;

struct DcHeader { UINT32 type; UINT32 size; LUID adapterId; UINT32 id; };
struct DcAdvancedColorInfo { DcHeader header; UINT32 value; UINT32 colorEncoding; UINT32 bitsPerColorChannel; };
struct DcAdvancedColorInfo2 { DcHeader header; UINT32 value; UINT32 colorEncoding; UINT32 bitsPerColorChannel; UINT32 activeColorMode; };

using PFN_DisplayConfigGetDeviceInfo = LONG(WINAPI *)(DcHeader *);
static PFN_DisplayConfigGetDeviceInfo real_DisplayConfigGetDeviceInfo = nullptr;

static LONG WINAPI hook_DisplayConfigGetDeviceInfo(DcHeader *packet)
{
    void *const ret = _ReturnAddress();
    const LONG result = real_DisplayConfigGetDeviceInfo(packet);
    if (packet == nullptr || !g_spoof_dxgi)
        return result;

    static std::atomic<int> logged_v1{0}, logged_v2{0}, logged_other{0};

    if (packet->type != DC_GET_ADVANCED_COLOR_INFO && packet->type != DC_GET_ADVANCED_COLOR_INFO_2 && logged_other++ < 12)
        LOG("DisplayConfigGetDeviceInfo(type %u) from %ls: result %ld", packet->type, caller_name(ret).c_str(), result);

    if (packet->type == DC_GET_ADVANCED_COLOR_INFO && packet->size >= sizeof(DcAdvancedColorInfo))
    {
        auto info = reinterpret_cast<DcAdvancedColorInfo *>(packet);
        if (logged_v1++ < 8)
            LOG("DisplayConfigGetDeviceInfo(ADVANCED_COLOR_INFO) from %ls: real result %ld value 0x%X bpc %u, reporting HDR on",
                caller_name(ret).c_str(), result, result == ERROR_SUCCESS ? info->value : 0, result == ERROR_SUCCESS ? info->bitsPerColorChannel : 0);
        // Bits: 0 supported, 1 enabled, 2 wide color enforced, 3 force disabled.
        info->value = (info->value & ~0xFu) | 0x3u;
        info->colorEncoding = 0; // RGB
        info->bitsPerColorChannel = 10;
        return ERROR_SUCCESS;
    }
    if (packet->type == DC_GET_ADVANCED_COLOR_INFO_2 && packet->size >= sizeof(DcAdvancedColorInfo2))
    {
        auto info = reinterpret_cast<DcAdvancedColorInfo2 *>(packet);
        if (logged_v2++ < 5)
            LOG("DisplayConfigGetDeviceInfo(ADVANCED_COLOR_INFO_2) from %ls: real result %ld value 0x%X mode %u, reporting HDR on",
                caller_name(ret).c_str(), result, result == ERROR_SUCCESS ? info->value : 0, result == ERROR_SUCCESS ? info->activeColorMode : 0);
        // Bits: 0 advanced color supported, 1 active, 3 limited by policy,
        // 4 HDR supported, 5 HDR user enabled, 6 wide color supported,
        // 7 wide color user enabled. Mode 2 is HDR.
        info->value = (info->value & ~0xFFu) | 0x73u;
        info->colorEncoding = 0;
        info->bitsPerColorChannel = 10;
        info->activeColorMode = 2;
        return ERROR_SUCCESS;
    }
    return result;
}

static void install_display_config_hook()
{
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    void *target = user32 != nullptr ? reinterpret_cast<void *>(GetProcAddress(user32, "DisplayConfigGetDeviceInfo")) : nullptr;
    if (target == nullptr ||
        MH_CreateHook(target, reinterpret_cast<void *>(&hook_DisplayConfigGetDeviceInfo), reinterpret_cast<void **>(&real_DisplayConfigGetDeviceInfo)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        WARN("failed to hook DisplayConfigGetDeviceInfo");
        return;
    }
    LOG("hooked DisplayConfigGetDeviceInfo");
}

// DXGI swap chain: CheckColorSpaceSupport and SetColorSpace1. Hooked on the
// real swap chain beneath ReShade's proxy, so ReShade still sees the color
// space the game asked for and records it.

using PFN_CheckColorSpaceSupport = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE, UINT *);
using PFN_SetColorSpace1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);
static PFN_CheckColorSpaceSupport real_CheckColorSpaceSupport = nullptr;
static PFN_SetColorSpace1 real_SetColorSpace1 = nullptr;
static std::atomic<int> g_game_color_space{-1};
static std::atomic<bool> g_scrgb_declared{false}; // see declare_scrgb
static std::atomic<int> g_hdr10_label{-1};          // see follow_tone_mapping

static HRESULT STDMETHODCALLTYPE hook_CheckColorSpaceSupport(IDXGISwapChain3 *self, DXGI_COLOR_SPACE_TYPE cs, UINT *support)
{
    HRESULT hr = real_CheckColorSpaceSupport(self, cs, support);
    const UINT real_support = (SUCCEEDED(hr) && support != nullptr) ? *support : 0;

    if (g_spoof_dxgi && support != nullptr &&
        (cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))
    {
        *support = real_support | DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
        hr = S_OK;
    }
    // Some games ask every frame, so each color space is logged when its
    // answer changes.
    static std::atomic<long long> logged[32];
    const unsigned returned = support != nullptr ? *support : 0;
    const long long answer = (static_cast<long long>(real_support) << 32 | returned) + 1;
    const int i = static_cast<int>(cs);
    if (i < 0 || i >= 32 || logged[i].exchange(answer) != answer)
        LOG("IDXGISwapChain3::CheckColorSpaceSupport(%d): real support 0x%X, returning 0x%X", i, real_support, returned);
    return hr;
}

// Set while this add-on labels a swap chain itself, so its own calls are not
// recorded as the game's.
static thread_local bool t_own_call = false;

// An HDR10 game asks for PQ. While the tone mapping runs, the frame is
// ordinary sRGB by the time it is shown, and a PQ label would make DWM convert
// it a second time. So the game is told PQ was accepted, ReShade's own record
// stays PQ so the tone mapping decodes the frame on that basis, and the swap
// chain is really labeled sRGB. With the tone mapping off, or not set up yet,
// the swap chain keeps PQ and DWM converts the HDR10 frame itself.
static DXGI_COLOR_SPACE_TYPE real_label(DXGI_COLOR_SPACE_TYPE cs)
{
    return (g_spoof_dxgi && cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && tonemap_outputs_srgb())
        ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 : cs;
}

static HRESULT STDMETHODCALLTYPE hook_SetColorSpace1(IDXGISwapChain3 *self, DXGI_COLOR_SPACE_TYPE cs)
{
    if (t_own_call)
        return real_SetColorSpace1(self, cs);

    g_game_color_space = static_cast<int>(cs);
    const DXGI_COLOR_SPACE_TYPE real_cs = real_label(cs);
    HRESULT hr = real_SetColorSpace1(self, real_cs);
    g_hdr10_label = static_cast<int>(real_cs);
    // Logged when the color space, the label or the outcome changes, not on
    // every call from a game that sets it each frame.
    static std::atomic<long long> logged{-1};
    const long long call = static_cast<long long>(cs) | static_cast<long long>(real_cs) << 16 | static_cast<long long>(FAILED(hr)) << 32;
    if (logged.exchange(call) != call)
        LOG("IDXGISwapChain3::SetColorSpace1(%d)%s: real result 0x%08X", static_cast<int>(cs),
            real_cs != cs ? ", labeled sRGB for Windows" : "", static_cast<unsigned>(hr));
    if (FAILED(hr) && g_spoof_dxgi)
        hr = S_OK;
    return hr;
}

// Borderless fullscreen. Some games that switch HDR on through NVAPI only
// offer it in exclusive fullscreen. Alt+Tab ends exclusive mode: DXGI
// minimizes the window, the game falls back to windowed on its return, and
// there it switches HDR off and marks it unavailable. So a
// request for exclusive fullscreen is answered with a borderless window
// covering the monitor instead, and the game is told it got what it asked
// for. There is then nothing for Alt+Tab to take away.

using PFN_SetFullscreenState = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL, IDXGIOutput *);
using PFN_GetFullscreenState = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
using PFN_ResizeTarget = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, const DXGI_MODE_DESC *);
static PFN_SetFullscreenState real_SetFullscreenState = nullptr;
static PFN_GetFullscreenState real_GetFullscreenState = nullptr;
static PFN_ResizeTarget real_ResizeTarget = nullptr;

struct emulated_fullscreen
{
    IDXGIOutput *output = nullptr;
    HWND hwnd = nullptr;
    LONG_PTR style = 0, ex_style = 0;
    RECT rect = {};
};
static std::mutex g_fs_mutex;
static std::unordered_map<IDXGISwapChain *, emulated_fullscreen> g_fs;

// The output the window sits on. GetContainingOutput is not enough when the
// monitor hangs off another adapter than the one rendering, so every output
// of every adapter is checked against the window's monitor.
static IDXGIOutput *output_for_window(IDXGISwapChain *sc, HWND hwnd)
{
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(sc->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return nullptr;
    IDXGIOutput *found = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT a = 0; found == nullptr && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        IDXGIOutput *output = nullptr;
        for (UINT o = 0; found == nullptr && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                found = output;
            else
                output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

static HRESULT STDMETHODCALLTYPE hook_SetFullscreenState(IDXGISwapChain *self, BOOL fullscreen, IDXGIOutput *target)
{
    // The setting can change in the overlay at any time. A window already
    // running borderless is still handled here until the game leaves it.
    bool emulating;
    {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        emulating = g_fs.count(self) != 0;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if ((!g_borderless && !emulating) || FAILED(self->GetDesc(&desc)) || desc.OutputWindow == nullptr)
        return real_SetFullscreenState(self, fullscreen, target);
    const HWND hwnd = desc.OutputWindow;

    // Window calls send messages to the window's thread, whose handler may
    // well ask for the fullscreen state, so none of them happen under the lock.
    if (fullscreen)
    {
        IDXGIOutput *output = target;
        if (output != nullptr)
            output->AddRef();
        else
            output = output_for_window(self, hwnd);

        bool first = false;
        emulated_fullscreen state;
        {
            std::lock_guard<std::mutex> lock(g_fs_mutex);
            auto it = g_fs.find(self);
            first = it == g_fs.end();
            if (first)
            {
                state.hwnd = hwnd;
                state.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                state.ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                GetWindowRect(hwnd, &state.rect);
                it = g_fs.emplace(self, state).first;
            }
            else if (it->second.output != nullptr)
                it->second.output->Release();
            it->second.output = output;
            state = it->second;
        }

        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (state.style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, state.ex_style & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        if (first)
            LOG("exclusive fullscreen requested, running borderless at %ldx%ld instead",
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        return S_OK;
    }

    emulated_fullscreen state;
    {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        auto it = g_fs.find(self);
        if (it != g_fs.end())
        {
            // A game that leaves fullscreen while in the background is reacting
            // to losing focus, which borderless does not need. Staying put keeps
            // the game in the only mode where it offers HDR.
            if (GetForegroundWindow() != hwnd)
                return S_OK;
            state = it->second;
            g_fs.erase(it);
        }
    }
    if (state.hwnd != nullptr)
    {
        if (state.output != nullptr)
            state.output->Release();
        SetWindowLongPtrW(hwnd, GWL_STYLE, state.style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, state.ex_style);
        SetWindowPos(hwnd, nullptr, state.rect.left, state.rect.top, state.rect.right - state.rect.left, state.rect.bottom - state.rect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
        LOG("left borderless fullscreen");
    }

    // Exclusive mode entered before the hooks were in place is left for real.
    BOOL real_fullscreen = FALSE;
    if (SUCCEEDED(real_GetFullscreenState(self, &real_fullscreen, nullptr)) && real_fullscreen)
        return real_SetFullscreenState(self, FALSE, nullptr);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hook_GetFullscreenState(IDXGISwapChain *self, BOOL *fullscreen, IDXGIOutput **target)
{
    {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        auto it = g_fs.find(self);
        if (it != g_fs.end())
        {
            if (fullscreen != nullptr)
                *fullscreen = TRUE;
            if (target != nullptr)
            {
                *target = it->second.output;
                if (*target != nullptr)
                    (*target)->AddRef();
            }
            return S_OK;
        }
    }
    return real_GetFullscreenState(self, fullscreen, target);
}

// A mode change would resize the window away from the monitor it covers.
static HRESULT STDMETHODCALLTYPE hook_ResizeTarget(IDXGISwapChain *self, const DXGI_MODE_DESC *mode)
{
    {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        if (g_fs.count(self) != 0)
            return S_OK;
    }
    return real_ResizeTarget(self, mode);
}

static void install_fullscreen_hooks(IDXGISwapChain *native)
{
    // IUnknown 3, IDXGIObject 4, IDXGIDeviceSubObject 1, then IDXGISwapChain:
    // Present 8, GetBuffer 9, SetFullscreenState 10, GetFullscreenState 11,
    // GetDesc 12, ResizeBuffers 13, ResizeTarget 14.
    void **vtbl = *reinterpret_cast<void ***>(native);
    if (MH_CreateHook(vtbl[10], reinterpret_cast<void *>(&hook_SetFullscreenState), reinterpret_cast<void **>(&real_SetFullscreenState)) == MH_OK &&
        MH_CreateHook(vtbl[11], reinterpret_cast<void *>(&hook_GetFullscreenState), reinterpret_cast<void **>(&real_GetFullscreenState)) == MH_OK &&
        MH_CreateHook(vtbl[14], reinterpret_cast<void *>(&hook_ResizeTarget), reinterpret_cast<void **>(&real_ResizeTarget)) == MH_OK &&
        MH_EnableHook(vtbl[10]) == MH_OK && MH_EnableHook(vtbl[11]) == MH_OK && MH_EnableHook(vtbl[14]) == MH_OK)
        LOG("hooked the swap chain fullscreen methods for borderless fullscreen");
    else
        WARN("failed to hook the swap chain fullscreen methods");
}

static void on_destroy_swapchain(swapchain *sc, bool resize)
{
    if (resize)
        return;
    auto native = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
    std::lock_guard<std::mutex> lock(g_fs_mutex);
    auto it = g_fs.find(native);
    if (it == g_fs.end())
        return;
    if (it->second.output != nullptr)
        it->second.output->Release();
    g_fs.erase(it);
}

static void on_init_swapchain(swapchain *sc, bool resize)
{
    auto native = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
    const device_api api = sc->get_device()->get_api();
    if (api != device_api::d3d10 && api != device_api::d3d11 && api != device_api::d3d12)
        return;

    const resource_desc desc = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
    LOG("swap chain %s: %ux%u format %u, color space %u",
        resize ? "resized" : "created", desc.texture.width, desc.texture.height,
        static_cast<unsigned>(desc.texture.format), static_cast<unsigned>(sc->get_color_space()));

    g_scrgb_declared = false;
    g_hdr10_label = -1;

    if (native != nullptr && real_SetFullscreenState == nullptr)
        install_fullscreen_hooks(native);

    if (real_SetColorSpace1 != nullptr || native == nullptr)
        return;

    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc3))))
        return;

    // IDXGISwapChain 18 slots, IDXGISwapChain1 11, IDXGISwapChain2 7, then
    // IDXGISwapChain3: GetCurrentBackBufferIndex 36, CheckColorSpaceSupport 37,
    // SetColorSpace1 38.
    void **vtbl = *reinterpret_cast<void ***>(sc3);
    void *check = vtbl[37];
    void *set = vtbl[38];
    sc3->Release();

    if (MH_CreateHook(check, reinterpret_cast<void *>(&hook_CheckColorSpaceSupport), reinterpret_cast<void **>(&real_CheckColorSpaceSupport)) == MH_OK &&
        MH_CreateHook(set, reinterpret_cast<void *>(&hook_SetColorSpace1), reinterpret_cast<void **>(&real_SetColorSpace1)) == MH_OK &&
        MH_EnableHook(check) == MH_OK && MH_EnableHook(set) == MH_OK)
        LOG("hooked IDXGISwapChain3::CheckColorSpaceSupport and SetColorSpace1");
    else
        WARN("failed to hook the swap chain color space methods");
}

// NVAPI: games from 2017 and 2018 often ask the NVIDIA driver directly rather
// than DXGI. nvapi_QueryInterface is the only export and hands back a function
// pointer per interface ID, so hooking it is enough. Each ID is logged once
// per caller, which shows what a game actually uses.

using NvU32 = uint32_t;
using NvU16 = uint16_t;
using NvAPI_Status = int;
static const NvAPI_Status NVAPI_OK = 0;

static const unsigned ID_GetHdrCapabilities = 0x84F2A8DF;
static const unsigned ID_HdrColorControl    = 0x351DA224;
static const unsigned ID_GetDisplayIdByDisplayName = 0xAE457190;

// The part every version of NV_HDR_CAPABILITIES shares.
struct NvHdrCapsHead
{
    NvU32 version;
    NvU32 flags;             // bit 0 ST2084 EOTF, bit 1 traditional HDR gamma, bit 2 EDR, bit 3 expand defaults, bit 4 traditional SDR gamma
    NvU32 static_metadata_descriptor_id;
    NvU16 prim[8];           // x0 y0 x1 y1 x2 y2 white x y, units of 1/50000
    NvU16 max_luminance;     // 1 nit
    NvU16 min_luminance;     // 0.0001 nit
    NvU16 max_frame_average; // 1 nit
};

// Likewise for NV_HDR_COLOR_DATA.
struct NvHdrColorHead
{
    NvU32 version;
    NvU32 cmd;               // 0 get, 1 set
    NvU32 hdrMode;           // 0 off, 2 UHDA (FP16 scRGB source), 5 HDR10 passthrough
    NvU32 static_metadata_descriptor_id;
    NvU16 mastering[12];
};

using PFN_nvapi_QueryInterface = void *(__cdecl *)(unsigned);
using PFN_GetHdrCapabilities = NvAPI_Status(__cdecl *)(NvU32, NvHdrCapsHead *);
using PFN_HdrColorControl = NvAPI_Status(__cdecl *)(NvU32, NvHdrColorHead *);

static PFN_nvapi_QueryInterface real_nvapi_QueryInterface = nullptr;
static PFN_GetHdrCapabilities real_GetHdrCapabilities = nullptr;
static PFN_HdrColorControl real_HdrColorControl = nullptr;
static std::atomic<int> g_nv_hdr_mode{0};
static NvHdrColorHead g_nv_last_set = {};
static std::mutex g_nv_mutex;

static NvU16 to_nv_prim(float v) { return static_cast<NvU16>(std::lround(v * 50000.0f)); }
static NvU16 clamp_u16(float v) { return static_cast<NvU16>(std::min(65535.0f, std::max(0.0f, std::round(v)))); }

static NvAPI_Status __cdecl hook_GetHdrCapabilities(NvU32 displayId, NvHdrCapsHead *caps)
{
    const NvAPI_Status status = real_GetHdrCapabilities(displayId, caps);
    if (caps == nullptr)
        return status;

    const NvU32 size = caps->version & 0xFFFF;
    static std::atomic<int> calls{0};
    const int n = ++calls;
    if (n <= 5)
        LOG("NvAPI_Disp_GetHdrCapabilities call %d (display 0x%X, struct v%u, %u bytes): real status %d, real flags 0x%X",
            n, displayId, caps->version >> 16, size, status, status == NVAPI_OK ? caps->flags : 0);

    if (!g_spoof_nvapi || size < sizeof(NvHdrCapsHead))
        return status;

    if (status != NVAPI_OK)
        std::memset(reinterpret_cast<char *>(caps) + sizeof(NvU32), 0, size - sizeof(NvU32));

    caps->flags = (1u << 0) | (1u << 4);
    caps->static_metadata_descriptor_id = 0;
    const float prim[8] = { PRIM_R[0], PRIM_R[1], PRIM_G[0], PRIM_G[1], PRIM_B[0], PRIM_B[1], WHITE[0], WHITE[1] };
    for (int i = 0; i < 8; ++i)
        caps->prim[i] = to_nv_prim(prim[i]);
    caps->max_luminance = clamp_u16(g_max_nits);
    caps->min_luminance = clamp_u16(g_min_nits * 10000.0f);
    caps->max_frame_average = clamp_u16(g_max_fall);
    return NVAPI_OK;
}

static NvAPI_Status __cdecl hook_HdrColorControl(NvU32 displayId, NvHdrColorHead *data)
{
    if (!g_spoof_nvapi || data == nullptr)
        return real_HdrColorControl(displayId, data);

    const NvU32 size = data->version & 0xFFFF;
    if (data->cmd == 1)
    {
        // The driver rejects HDR on a display that cannot take it, and the
        // game then turns its HDR option off. Record the request and report
        // it back on GET; declare_scrgb does the part the driver would have.
        static std::atomic<long long> logged_set{-1};
        if (logged_set.exchange(data->hdrMode) != static_cast<long long>(data->hdrMode))
            LOG("NvAPI_Disp_HdrColorControl SET mode %u (struct v%u), accepted without forwarding", data->hdrMode, data->version >> 16);
        std::lock_guard<std::mutex> lock(g_nv_mutex);
        std::memcpy(&g_nv_last_set, data, sizeof(NvHdrColorHead));
        g_nv_hdr_mode = static_cast<int>(data->hdrMode);
        return NVAPI_OK;
    }

    NvAPI_Status status = real_HdrColorControl(displayId, data);
    {
        std::lock_guard<std::mutex> lock(g_nv_mutex);
        data->hdrMode = static_cast<NvU32>(g_nv_hdr_mode.load());
        if (g_nv_hdr_mode != 0 && size >= sizeof(NvHdrColorHead))
            std::memcpy(data->mastering, g_nv_last_set.mastering, sizeof(data->mastering));
    }
    static std::atomic<long long> logged_get{-1};
    const long long reply = static_cast<long long>(data->hdrMode) | static_cast<long long>(static_cast<unsigned>(status)) << 32;
    if (logged_get.exchange(reply) != reply)
        LOG("NvAPI_Disp_HdrColorControl GET: real status %d, reporting mode %u", status, data->hdrMode);
    return NVAPI_OK;
}

// On a machine whose monitor hangs off another adapter, the integrated GPU
// for instance, NVIDIA's driver does not own the display and this returns
// NVAPI_NVIDIA_DEVICE_NOT_FOUND. A game that gates HDR on NVAPI stops there
// without ever asking about HDR, so hand back a made-up display ID; the two
// HDR calls that follow are answered above and never need the real one.
static const NvU32 FAKE_DISPLAY_ID = 0x80061082;
using PFN_GetDisplayIdByDisplayName = NvAPI_Status(__cdecl *)(const char *, NvU32 *);
static PFN_GetDisplayIdByDisplayName real_GetDisplayIdByDisplayName = nullptr;

static NvAPI_Status __cdecl hook_GetDisplayIdByDisplayName(const char *name, NvU32 *id)
{
    const NvAPI_Status status = real_GetDisplayIdByDisplayName(name, id);
    static std::atomic<int> logged{0};
    if (status == NVAPI_OK || id == nullptr)
    {
        if (logged++ < 3)
            LOG("NvAPI_DISP_GetDisplayIdByDisplayName(%s): display id 0x%X", name ? name : "(null)", id ? *id : 0);
        return status;
    }
    if (logged++ < 3)
        LOG("NvAPI_DISP_GetDisplayIdByDisplayName(%s): real status %d, the display is not on an NVIDIA GPU, reporting id 0x%X",
            name ? name : "(null)", status, FAKE_DISPLAY_ID);
    *id = FAKE_DISPLAY_ID;
    return NVAPI_OK;
}

static void *__cdecl hook_nvapi_QueryInterface(unsigned id)
{
    void *const ret = _ReturnAddress();
    const HMODULE caller = module_from_address(ret);
    const bool from_game = caller != nullptr && caller == GetModuleHandleW(nullptr);

    static std::mutex seen_mutex;
    static std::unordered_set<unsigned long long> seen;
    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        if (seen.insert((static_cast<unsigned long long>(from_game) << 32) | id).second)
            LOG("nvapi_QueryInterface(0x%08X)%s from %ls%s", id,
                id == ID_GetHdrCapabilities ? " NvAPI_Disp_GetHdrCapabilities" :
                id == ID_HdrColorControl ? " NvAPI_Disp_HdrColorControl" : "",
                caller_name(ret).c_str(),
                from_game && g_hide_nvapi_from_game ? ", hidden" : "");
    }

    // Without NVAPI the game takes the path it uses on AMD and Intel, which
    // decides HDR from DXGI and the Windows display configuration, both of
    // which are answered above.
    if (from_game && g_hide_nvapi_from_game)
        return nullptr;

    void *fn = real_nvapi_QueryInterface(id);
    if (fn == nullptr)
        return fn;
    if (g_trace && id != ID_GetDisplayIdByDisplayName)
        fn = trace_wrap_nvapi(id, fn);
    if (id == ID_GetHdrCapabilities)
    {
        real_GetHdrCapabilities = reinterpret_cast<PFN_GetHdrCapabilities>(fn);
        return reinterpret_cast<void *>(&hook_GetHdrCapabilities);
    }
    if (id == ID_HdrColorControl)
    {
        real_HdrColorControl = reinterpret_cast<PFN_HdrColorControl>(fn);
        return reinterpret_cast<void *>(&hook_HdrColorControl);
    }
    if (id == ID_GetDisplayIdByDisplayName)
    {
        real_GetDisplayIdByDisplayName = reinterpret_cast<PFN_GetDisplayIdByDisplayName>(fn);
        return reinterpret_cast<void *>(&hook_GetDisplayIdByDisplayName);
    }
    return fn;
}

static void install_nvapi_hook()
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    {
        LOG("nvapi64.dll not present, NVAPI reporting not installed");
        return;
    }
    void *target = reinterpret_cast<void *>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (target == nullptr ||
        MH_CreateHook(target, reinterpret_cast<void *>(&hook_nvapi_QueryInterface), reinterpret_cast<void **>(&real_nvapi_QueryInterface)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        WARN("failed to hook nvapi_QueryInterface");
        return;
    }
    LOG("hooked nvapi_QueryInterface");
}

// Capture. Runs in the present event, which ReShade fires before it renders
// any effects, so the file holds the game's own output and nothing else.

static float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    float v;
    if (exp == 0)
        v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

static float pq_to_nits(float e)
{
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

struct capture_job
{
    std::vector<float> rgb;  // top to bottom, 3 floats per pixel
    uint32_t width = 0, height = 0;
    std::string format_name, color_space_name, encoding_name;
    int game_color_space = -1, nv_hdr_mode = 0;
    std::filesystem::path path;
};

static void write_job(capture_job job)
{
    std::error_code ec;
    std::filesystem::create_directories(job.path.parent_path(), ec);

    // PFM stores rows bottom to top.
    FILE *f = _wfopen(job.path.c_str(), L"wb");
    if (f == nullptr)
    {
        WARN("could not open %s for writing", job.path.u8string().c_str());
        return;
    }
    std::fprintf(f, "PF\n%u %u\n-1.0\n", job.width, job.height);
    for (uint32_t y = job.height; y-- > 0;)
        std::fwrite(job.rgb.data() + static_cast<size_t>(y) * job.width * 3, sizeof(float), static_cast<size_t>(job.width) * 3, f);
    std::fclose(f);

    std::filesystem::path meta = job.path;
    meta.replace_extension(L".json");
    if (FILE *m = _wfopen(meta.c_str(), L"w"))
    {
        std::fprintf(m,
            "{\n  \"width\": %u,\n  \"height\": %u,\n  \"format\": \"%s\",\n  \"color_space\": \"%s\",\n"
            "  \"game_dxgi_color_space\": %d,\n  \"nvapi_hdr_mode\": %d,\n  \"encoding\": \"%s\",\n"
            "  \"reported_max_luminance\": %.1f,\n  \"reported_min_luminance\": %.4f,\n  \"reported_max_frame_average\": %.1f\n}\n",
            job.width, job.height, job.format_name.c_str(), job.color_space_name.c_str(),
            job.game_color_space, job.nv_hdr_mode, job.encoding_name.c_str(),
            g_max_nits, g_min_nits, g_max_fall);
        std::fclose(m);
    }
    LOG("saved %s (%ux%u, %s, %s)", job.path.u8string().c_str(), job.width, job.height, job.format_name.c_str(), job.encoding_name.c_str());
}

static const char *color_space_name(color_space cs)
{
    switch (cs)
    {
    case color_space::srgb: return "srgb";
    case color_space::scrgb: return "scrgb";
    case color_space::hdr10_pq: return "hdr10_pq";
    case color_space::hdr10_hlg: return "hdr10_hlg";
    default: return "unknown";
    }
}

static void capture(command_queue *queue, swapchain *sc)
{
    device *const dev = sc->get_device();
    const resource back_buffer = sc->get_current_back_buffer();
    const resource_desc desc = dev->get_resource_desc(back_buffer);
    const format fmt = format_to_default_typed(desc.texture.format, 0);
    const color_space cs = sc->get_color_space();
    const uint32_t w = desc.texture.width, h = desc.texture.height;

    resource staging = {};
    if (!dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::readback, resource_usage::copy_dest),
            nullptr, resource_usage::copy_dest, &staging))
    {
        WARN("could not create a readback texture for format %u", static_cast<unsigned>(fmt));
        return;
    }

    command_list *const cmd = queue->get_immediate_command_list();
    cmd->barrier(back_buffer, resource_usage::present, resource_usage::copy_source);
    cmd->copy_texture_region(back_buffer, 0, nullptr, staging, 0, nullptr);
    cmd->barrier(back_buffer, resource_usage::copy_source, resource_usage::present);
    queue->flush_immediate_command_list();
    queue->wait_idle();

    subresource_data mapped = {};
    if (!dev->map_texture_region(staging, 0, nullptr, map_access::read_only, &mapped))
    {
        WARN("could not map the readback texture");
        dev->destroy_resource(staging);
        return;
    }

    capture_job job;
    job.width = w;
    job.height = h;
    job.rgb.resize(static_cast<size_t>(w) * h * 3);
    job.color_space_name = color_space_name(cs);
    job.game_color_space = g_game_color_space;
    job.nv_hdr_mode = g_nv_hdr_mode;

    const bool pq = cs == color_space::hdr10_pq || g_game_color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || g_nv_hdr_mode == 5;

    bool ok = true;
    for (uint32_t y = 0; y < h && ok; ++y)
    {
        const uint8_t *row = static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(y) * mapped.row_pitch;
        float *out = job.rgb.data() + static_cast<size_t>(y) * w * 3;
        for (uint32_t x = 0; x < w; ++x, out += 3)
        {
            switch (fmt)
            {
            case format::r16g16b16a16_float:
            {
                // scRGB: linear BT.709, 1.0 is 80 nits. Negative values are
                // colors outside BT.709 and are kept.
                const uint16_t *p = reinterpret_cast<const uint16_t *>(row) + x * 4;
                out[0] = half_to_float(p[0]) * 80.0f;
                out[1] = half_to_float(p[1]) * 80.0f;
                out[2] = half_to_float(p[2]) * 80.0f;
                break;
            }
            case format::r10g10b10a2_unorm:
            {
                const uint32_t v = reinterpret_cast<const uint32_t *>(row)[x];
                const float r = (v & 0x3FF) / 1023.0f, g = ((v >> 10) & 0x3FF) / 1023.0f, b = ((v >> 20) & 0x3FF) / 1023.0f;
                if (pq)
                {
                    // HDR10: PQ in BT.2020, converted to linear BT.709 nits.
                    const float R = pq_to_nits(r), G = pq_to_nits(g), B = pq_to_nits(b);
                    out[0] =  1.6604910f * R - 0.5876411f * G - 0.0728499f * B;
                    out[1] = -0.1245505f * R + 1.1328999f * G - 0.0083494f * B;
                    out[2] = -0.0181508f * R - 0.1005789f * G + 1.1187297f * B;
                }
                else
                {
                    out[0] = r; out[1] = g; out[2] = b;
                }
                break;
            }
            case format::r8g8b8a8_unorm:
            case format::r8g8b8a8_unorm_srgb:
            case format::r8g8b8x8_unorm:
            {
                const uint8_t *p = row + x * 4;
                out[0] = p[0] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[2] / 255.0f;
                break;
            }
            case format::b8g8r8a8_unorm:
            case format::b8g8r8a8_unorm_srgb:
            case format::b8g8r8x8_unorm:
            {
                const uint8_t *p = row + x * 4;
                out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f;
                break;
            }
            default:
                ok = false;
                break;
            }
            if (!ok)
                break;
        }
    }
    dev->unmap_texture_region(staging, 0);
    dev->destroy_resource(staging);

    if (!ok)
    {
        WARN("back buffer format %u is not handled", static_cast<unsigned>(fmt));
        return;
    }

    switch (fmt)
    {
    case format::r16g16b16a16_float: job.format_name = "r16g16b16a16_float"; break;
    case format::r10g10b10a2_unorm:  job.format_name = "r10g10b10a2_unorm"; break;
    default:                         job.format_name = "8 bit"; break;
    }
    const bool linear = fmt == format::r16g16b16a16_float || (fmt == format::r10g10b10a2_unorm && pq);
    job.encoding_name = linear ? "linear_nits_bt709" : "srgb_encoded";

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t name[160];
    swprintf_s(name, L"%ls_%04u%02u%02u_%02u%02u%02u_%03u_%ls.pfm",
        std::filesystem::path(exe).stem().c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        linear ? L"hdr" : L"sdr");
    job.path = g_output_dir / name;

    // The file is 27 MB at 1920x1200; write it off the render thread.
    std::thread(write_job, std::move(job)).detach();
}

// A game that turns HDR on through NVAPI never calls SetColorSpace1, because
// in UHDA mode the driver treats the float swap chain as scRGB. With the SET
// request swallowed nothing does, so DWM falls back to the DXGI default, G22,
// and shows linear values as if gamma encoded. Measured in one game, the screen
// matched the raw buffer within a level at every percentile, two stops dark in
// the midtones. g_scrgb_declared is cleared on every swap chain init, since a
// resize or recreation resets the color space.
static std::atomic<bool> g_ever_declared_scrgb{false};

static void declare_scrgb(swapchain *sc)
{
    if (g_scrgb_declared)
        return;
    const format fmt = format_to_default_typed(sc->get_device()->get_resource_desc(sc->get_current_back_buffer()).texture.format, 0);
    const bool fp16 = fmt == format::r16g16b16a16_float;

    // Once scRGB has been declared, an 8-bit swap chain the game goes back to
    // keeps it, and DWM shows its sRGB values as linear, washed out.
    DXGI_COLOR_SPACE_TYPE want;
    if (fp16 && g_nv_hdr_mode == 2)
        want = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    else if (!fp16 && g_ever_declared_scrgb)
        want = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    else
        return;

    IDXGISwapChain3 *sc3 = nullptr;
    auto native = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
    if (native == nullptr || FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc3))))
        return;
    t_own_call = true;
    const HRESULT hr = sc3->SetColorSpace1(want);
    t_own_call = false;
    sc3->Release();
    g_scrgb_declared = true;
    if (want == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
        g_ever_declared_scrgb = true;
    LOG("declared the swap chain as %s, result 0x%08X", want == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? "scRGB for the NVAPI HDR mode" : "sRGB again for SDR", static_cast<unsigned>(hr));
}

// The tone mapping can be switched in the overlay, and it only starts once
// ReShade has a runtime, both well after the game labeled its swap chain. So
// an HDR10 swap chain's real label is checked every frame and moved to match.
static void follow_tone_mapping(swapchain *sc)
{
    if (sc->get_color_space() != color_space::hdr10_pq)
        return;
    const DXGI_COLOR_SPACE_TYPE want = real_label(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (g_hdr10_label == static_cast<int>(want))
        return;

    IDXGISwapChain3 *sc3 = nullptr;
    auto native = reinterpret_cast<IDXGISwapChain *>(sc->get_native());
    if (native == nullptr || FAILED(native->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc3))))
        return;
    t_own_call = true;
    const HRESULT hr = sc3->SetColorSpace1(want);
    t_own_call = false;
    sc3->Release();
    g_hdr10_label = static_cast<int>(want);
    const bool srgb = want == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    LOG("HDR10 swap chain labeled %s for Windows, since the tone mapping is %s, result 0x%08X",
        srgb ? "sRGB" : "PQ", srgb ? "on" : "off", static_cast<unsigned>(hr));
}

// Settings panel, under this add-on in ReShade's Add-ons tab. Changes are
// written back to ReShade.ini at once.

static bool g_listening = false;
static int g_listen_frame = 0;               // ImGui frame listening was last drawn on
static std::atomic<bool> g_capture_pending{false};
static bool g_skip_capture = false;          // the press that set the key is not a capture

// ReShade's own hotkeys, which cannot double as the capture key. Returns the
// name of the one using this key, or null.
static const char *is_reshade_hotkey(int vk)
{
    for (const char *name : {"KeyOverlay", "KeyEffects", "KeyScreenshot", "KeyReload", "KeyNextPreset", "KeyPreviousPreset"})
    {
        // Stored as "keycode,ctrl,shift,alt"; the key code comes first.
        char text[64] = {};
        size_t size = sizeof(text);
        if (reshade::get_config_value(nullptr, "INPUT", name, text, &size) && atoi(text) == vk)
            return name;
    }
    return nullptr;
}

static std::string key_name(int vk)
{
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    // Keys without a plain scan code, which GetKeyNameText cannot name.
    switch (vk)
    {
    case VK_PAUSE:    return "Pause";
    case VK_CANCEL:   return "Break";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_SCROLL:   return "Scroll Lock";
    case VK_LWIN:     return "Left Windows";
    case VK_RWIN:     return "Right Windows";
    case VK_APPS:     return "Menu";
    }
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    switch (vk)
    {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        scan |= 0x100; // extended key, or GetKeyNameText names the numpad twin
        break;
    }
    char name[64] = {};
    if (scan != 0 && GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0)
        return name;
    char code[32];
    std::snprintf(code, sizeof(code), "key 0x%02X", vk);
    return code;
}

static void draw_settings(effect_runtime *runtime)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Capture key");
    ImGui::SameLine();
    const std::string label = (g_listening ? std::string("Press a key, Esc to cancel") : key_name(g_capture_key)) + "###capture_key";
    const int frame = ImGui::GetFrameCount();
    // Closed or collapsed mid-listen: drop it rather than wait unseen.
    if (g_listening && frame != g_listen_frame + 1)
        g_listening = false;
    bool started = false;
    if (ImGui::Button(label.c_str(), ImVec2(220.0f, 0.0f)) && !g_listening)
        g_listening = started = true;
    g_listen_frame = frame;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Saves the game's frame before any effect runs, as a float image.");

    // Asked of ReShade rather than of Windows: while the overlay is open ReShade
    // blocks input from the game by hooking GetAsyncKeyState, and every key
    // then reads as up. Only a key pressed this frame counts, and not on the
    // frame listening starts, so the Space or Enter that pressed the button is
    // not taken for the answer. Mouse buttons sit below 0x08 and are skipped.
    if (g_listening && !started && runtime != nullptr)
    {
        for (int vk = 0x08; vk < 256; ++vk)
        {
            // Generic modifier codes duplicate their left and right forms.
            if (!runtime->is_key_pressed(vk) || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
                continue;
            if (vk != VK_ESCAPE && is_reshade_hotkey(vk) != nullptr)
                continue;
            if (vk != VK_ESCAPE)
            {
                g_capture_key = vk;
                g_skip_capture = true;
                reshade::set_config_value(nullptr, "HDRBRIDGE", "CaptureKey", vk);
                LOG("capture key set to %s (0x%02X)", key_name(vk).c_str(), vk);
            }
            g_listening = false;
            break;
        }
    }

    bool borderless = g_borderless;
    if (ImGui::Checkbox("Borderless fullscreen", &borderless))
    {
        g_borderless = borderless;
        reshade::set_config_value(nullptr, "HDRBRIDGE", "BorderlessFullscreen", borderless);
        LOG("BorderlessFullscreen set to %d", borderless);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the game in a borderless window when it asks for exclusive\n"
                          "fullscreen, so Alt+Tab cannot drop it out of HDR. Takes effect\n"
                          "the next time the game enters fullscreen.");

    tonemap_draw_settings();
}

static void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
    declare_scrgb(sc);
    follow_tone_mapping(sc);

    // Seen at the end of the previous frame, taken here before any effect runs.
    if (g_capture_pending.exchange(false))
        capture(queue, sc);
}

// The capture key is read through ReShade, the same input the binding uses, so
// it works with the overlay open and only while the game has focus.
static void on_reshade_present(effect_runtime *runtime)
{
    if (g_listening || !runtime->is_key_pressed(g_capture_key))
        return;
    if (g_skip_capture)
        g_skip_capture = false;
    else
        g_capture_pending = true;
}

static DWORD WINAPI init_thread(LPVOID)
{
    read_settings();
    // Only a rebinding in the panel is checked against ReShade's keys, so a key
    // from ReShade.ini, or the default, is checked here.
    if (const char *clash = is_reshade_hotkey(g_capture_key))
        WARN("the capture key, %s (0x%02X), is also ReShade's %s; one key press will do both",
            key_name(g_capture_key).c_str(), g_capture_key.load(), clash);
    if (g_trace)
        trace_install();
    if (g_spoof_dxgi)
    {
        install_dxgi_output_hook();
        install_display_config_hook();
    }
    if (g_spoof_nvapi)
        install_nvapi_hook();
    return 0;
}

static HMODULE g_module = nullptr;

static bool register_with_reshade()
{
    if (!reshade::register_addon(g_module))
        return false;
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    tonemap_register();
    reshade::register_overlay(nullptr, draw_settings);
    return true;
}

// Some mod setups, OptiScaler among them, load ReShade, unload it and load it
// again. This module stays loaded throughout, so the second ReShade's
// LoadLibrary hands back the existing module without running DllMain, and the
// add-on is never registered with the ReShade that actually runs: its hooks
// work, its events and settings never arrive. So the loader is watched. When
// ReShade goes, nothing calls into it; when a module exporting the add-on API
// arrives, a thread registers again once its DllMain is done.
struct ldr_unicode_string { USHORT length, maximum_length; PWSTR buffer; };
struct ldr_dll_notification_data { ULONG flags; const ldr_unicode_string *full_name, *base_name; PVOID base; ULONG size; };
using PFN_LdrDllNotification = VOID(CALLBACK *)(ULONG, const ldr_dll_notification_data *, PVOID);
using PFN_LdrRegisterDllNotification = LONG(NTAPI *)(ULONG, PFN_LdrDllNotification, PVOID, PVOID *);
static const ULONG LDR_DLL_LOADED = 1, LDR_DLL_UNLOADED = 2;
static std::atomic<HMODULE> g_reshade_module{nullptr};

static DWORD WINAPI reregister_thread(LPVOID)
{
    // Thread start waits on the loader lock, so ReShade's DllMain has finished.
    // ReShade's header caches the module handle it found first, so this only
    // works when the reload lands at the same address. It does: an image's
    // randomized base is fixed for the boot session, and the old one is free.
    if (g_reshade_module != reshade::internal::get_reshade_module_handle())
        return 0;
    g_reshade_alive = true;
    if (register_with_reshade())
        LOG("ReShade was unloaded and loaded again; registered with the new instance");
    else
        g_reshade_alive = false;
    return 0;
}

static VOID CALLBACK on_dll_notification(ULONG reason, const ldr_dll_notification_data *data, PVOID)
{
    const HMODULE module = static_cast<HMODULE>(data->base);
    if (reason == LDR_DLL_UNLOADED && module == g_reshade_module)
    {
        g_reshade_alive = false;
        tonemap_forget();
    }
    else if (reason == LDR_DLL_LOADED && !g_reshade_alive &&
             GetProcAddress(module, "ReShadeRegisterAddon") != nullptr)
    {
        g_reshade_module = module;
        if (HANDLE t = CreateThread(nullptr, 0, reregister_thread, nullptr, 0, nullptr))
            CloseHandle(t);
    }
}

static void watch_reshade_reloads()
{
    g_reshade_module = reshade::internal::get_reshade_module_handle();
    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    static PVOID cookie = nullptr;
    if (reg != nullptr)
        reg(0, on_dll_notification, nullptr, &cookie);
}

bridge_info hdrbridge_info()
{
    bridge_info i;
    i.game_color_space = g_game_color_space;
    i.hdr10_label = g_hdr10_label;
    i.nvapi_hdr_mode = g_nv_hdr_mode;
    i.max_nits = g_max_nits;
    i.min_nits = g_min_nits;
    i.max_frame_average = g_max_fall;
    return i;
}

// False when ReShade declined the add-on at load, and then nothing is set up.
static bool g_active = false;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        if (!register_with_reshade())
        {
            // A ReShade that declined it, which is how an add-on switched off
            // in the Add-ons tab arrives: stay loaded and do nothing, since
            // failing DllMain makes ReShade log a load error. With no ReShade
            // at all, fail, so a ReShade loaded later can run this
            // DllMain again.
            if (GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeLogMessage") == nullptr)
                return FALSE;
            reshade::log::message(reshade::log::level::info,
                "HDR Bridge is disabled in ReShade's Add-ons tab, or this ReShade cannot load it; it does nothing this session.");
            break;
        }
        g_active = true;
        tonemap_init(module);
        MH_Initialize();
        watch_reshade_reloads();
        // Loading DXGI and NVAPI is not allowed under the loader lock, so the
        // hooks go in from a thread that starts as soon as DllMain returns,
        // well before a game gets as far as asking about the display.
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr))
            CloseHandle(t);
        break;
    case DLL_PROCESS_DETACH:
        if (!g_active)
            break;
        if (g_reshade_alive)
            reshade::unregister_addon(module);
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
