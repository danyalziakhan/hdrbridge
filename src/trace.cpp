// Diagnostic tracing for finding out how a game decides whether HDR is
// available. Logs the registry reads and device enumeration that the game's
// own code makes, ignoring those from the driver, DXGI and other add-ons, and
// every NVIDIA driver setting read through NVAPI. Enabled with
// TraceGameCalls=1 under [HDRBRIDGE].

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <devpropdef.h>

#include <reshade.hpp>
#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern std::atomic<bool> g_reshade_alive;

static void tlog(const char *fmt, ...)
{
    if (!g_reshade_alive)
        return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    reshade::log::message(reshade::log::level::info, buf);
}

static thread_local int t_depth = 0;

struct reentry_guard
{
    bool outer;
    reentry_guard() : outer(t_depth++ == 0) {}
    ~reentry_guard() { --t_depth; }
};

// True when the game's executable is on the stack a few frames up. Walking
// the stack rather than reading one return address catches calls that pass
// through a system forwarder such as advapi32 on their way to kernelbase.
static bool from_game()
{
    static const HMODULE game = GetModuleHandleW(nullptr);
    void *frames[6];
    const USHORT n = RtlCaptureStackBackTrace(2, 6, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(frames[i]), &m) && m == game)
            return true;
    }
    return false;
}

// Each distinct message is logged once, so a value read every frame does not
// flood the log.
static bool first_time(const std::string &key)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    return seen.size() < 400 && seen.insert(key).second;
}

static std::string narrow(const wchar_t *w)
{
    if (w == nullptr)
        return "(null)";
    std::string s;
    for (; *w; ++w)
        s += (*w < 128) ? static_cast<char>(*w) : '?';
    return s;
}

static std::mutex g_key_mutex;
static std::unordered_map<HKEY, std::string> g_key_paths;

static std::string key_path(HKEY key)
{
    if (key == HKEY_LOCAL_MACHINE) return "HKLM";
    if (key == HKEY_CURRENT_USER) return "HKCU";
    if (key == HKEY_CLASSES_ROOT) return "HKCR";
    if (key == HKEY_USERS) return "HKU";
    if (key == HKEY_CURRENT_CONFIG) return "HKCC";
    std::lock_guard<std::mutex> lock(g_key_mutex);
    auto it = g_key_paths.find(key);
    return it != g_key_paths.end() ? it->second : "<key>";
}

static void remember_key(HKEY key, const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_key_mutex);
    g_key_paths[key] = path;
}

using PFN_RegOpenKeyExA = LSTATUS(WINAPI *)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using PFN_RegOpenKeyExW = LSTATUS(WINAPI *)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using PFN_RegQueryValueExA = LSTATUS(WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using PFN_RegQueryValueExW = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using PFN_RegGetValueA = LSTATUS(WINAPI *)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using PFN_RegGetValueW = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

static PFN_RegOpenKeyExA real_RegOpenKeyExA;
static PFN_RegOpenKeyExW real_RegOpenKeyExW;
static PFN_RegQueryValueExA real_RegQueryValueExA;
static PFN_RegQueryValueExW real_RegQueryValueExW;
static PFN_RegGetValueA real_RegGetValueA;
static PFN_RegGetValueW real_RegGetValueW;

static LSTATUS WINAPI hook_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    reentry_guard g;
    const LSTATUS r = real_RegOpenKeyExA(root, sub, opt, sam, out);
    if (g.outer && r == ERROR_SUCCESS && out != nullptr)
    {
        const std::string path = key_path(root) + "\\" + (sub ? sub : "");
        remember_key(*out, path);
        if (from_game() && first_time("open " + path))
            tlog("[trace] game RegOpenKeyExA %s", path.c_str());
    }
    return r;
}

static LSTATUS WINAPI hook_RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    reentry_guard g;
    const LSTATUS r = real_RegOpenKeyExW(root, sub, opt, sam, out);
    if (g.outer && r == ERROR_SUCCESS && out != nullptr)
    {
        const std::string path = key_path(root) + "\\" + narrow(sub);
        remember_key(*out, path);
        if (from_game() && first_time("open " + path))
            tlog("[trace] game RegOpenKeyExW %s", path.c_str());
    }
    return r;
}

static LSTATUS WINAPI hook_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    reentry_guard g;
    const LSTATUS r = real_RegQueryValueExA(key, name, res, type, data, cb);
    if (g.outer && from_game())
    {
        const std::string msg = key_path(key) + " : " + (name ? name : "(default)");
        if (first_time("query " + msg))
            tlog("[trace] game RegQueryValueExA %s -> %ld", msg.c_str(), r);
    }
    return r;
}

static LSTATUS WINAPI hook_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    reentry_guard g;
    const LSTATUS r = real_RegQueryValueExW(key, name, res, type, data, cb);
    if (g.outer && from_game())
    {
        const std::string msg = key_path(key) + " : " + (name ? narrow(name) : "(default)");
        if (first_time("query " + msg))
            tlog("[trace] game RegQueryValueExW %s -> %ld", msg.c_str(), r);
    }
    return r;
}

static LSTATUS WINAPI hook_RegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb)
{
    reentry_guard g;
    const LSTATUS r = real_RegGetValueA(key, sub, name, flags, type, data, cb);
    if (g.outer && from_game())
    {
        const std::string msg = key_path(key) + "\\" + (sub ? sub : "") + " : " + (name ? name : "(default)");
        if (first_time("get " + msg))
            tlog("[trace] game RegGetValueA %s -> %ld", msg.c_str(), r);
    }
    return r;
}

static LSTATUS WINAPI hook_RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb)
{
    reentry_guard g;
    const LSTATUS r = real_RegGetValueW(key, sub, name, flags, type, data, cb);
    if (g.outer && from_game())
    {
        const std::string msg = key_path(key) + "\\" + narrow(sub) + " : " + (name ? narrow(name) : "(default)");
        if (first_time("get " + msg))
            tlog("[trace] game RegGetValueW %s -> %ld", msg.c_str(), r);
    }
    return r;
}

static std::string guid_string(const GUID *g)
{
    if (g == nullptr)
        return "(null)";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return buf;
}

using PFN_SetupDiGetClassDevsW = HDEVINFO(WINAPI *)(const GUID *, PCWSTR, HWND, DWORD);
using PFN_SetupDiGetDeviceRegistryPropertyW = BOOL(WINAPI *)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
using PFN_SetupDiGetDevicePropertyW = BOOL(WINAPI *)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, DWORD, PDWORD, DWORD);
using PFN_SetupDiGetDeviceInterfaceDetailW = BOOL(WINAPI *)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);

static PFN_SetupDiGetClassDevsW real_SetupDiGetClassDevsW;
static PFN_SetupDiGetDeviceRegistryPropertyW real_SetupDiGetDeviceRegistryPropertyW;
static PFN_SetupDiGetDevicePropertyW real_SetupDiGetDevicePropertyW;
static PFN_SetupDiGetDeviceInterfaceDetailW real_SetupDiGetDeviceInterfaceDetailW;

static HDEVINFO WINAPI hook_SetupDiGetClassDevsW(const GUID *guid, PCWSTR enumerator, HWND hwnd, DWORD flags)
{
    reentry_guard g;
    HDEVINFO r = real_SetupDiGetClassDevsW(guid, enumerator, hwnd, flags);
    if (g.outer && from_game())
    {
        const std::string msg = guid_string(guid) + " enumerator " + narrow(enumerator);
        if (first_time("classdevs " + msg))
            tlog("[trace] game SetupDiGetClassDevsW %s flags 0x%lX", msg.c_str(), flags);
    }
    return r;
}

static BOOL WINAPI hook_SetupDiGetDeviceRegistryPropertyW(HDEVINFO set, PSP_DEVINFO_DATA dev, DWORD prop, PDWORD type, PBYTE buf, DWORD size, PDWORD req)
{
    reentry_guard g;
    const BOOL r = real_SetupDiGetDeviceRegistryPropertyW(set, dev, prop, type, buf, size, req);
    if (g.outer && from_game() && first_time("regprop " + std::to_string(prop)))
        tlog("[trace] game SetupDiGetDeviceRegistryPropertyW property %lu -> %d", prop, r);
    return r;
}

static BOOL WINAPI hook_SetupDiGetDevicePropertyW(HDEVINFO set, PSP_DEVINFO_DATA dev, const DEVPROPKEY *key, DEVPROPTYPE *type, PBYTE buf, DWORD size, PDWORD req, DWORD flags)
{
    reentry_guard g;
    const BOOL r = real_SetupDiGetDevicePropertyW(set, dev, key, type, buf, size, req, flags);
    if (g.outer && from_game() && key != nullptr)
    {
        const std::string msg = guid_string(&key->fmtid) + " pid " + std::to_string(key->pid);
        if (first_time("devprop " + msg))
            tlog("[trace] game SetupDiGetDevicePropertyW %s -> %d", msg.c_str(), r);
    }
    return r;
}

static BOOL WINAPI hook_SetupDiGetDeviceInterfaceDetailW(HDEVINFO set, PSP_DEVICE_INTERFACE_DATA iface, PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD size, PDWORD req, PSP_DEVINFO_DATA dev)
{
    reentry_guard g;
    const BOOL r = real_SetupDiGetDeviceInterfaceDetailW(set, iface, detail, size, req, dev);
    if (g.outer && r && detail != nullptr && from_game())
    {
        const std::string path = narrow(detail->DevicePath);
        if (first_time("iface " + path))
            tlog("[trace] game SetupDiGetDeviceInterfaceDetailW %s", path.c_str());
    }
    return r;
}

// NVAPI calls, wrapped when nvapi_QueryInterface hands them out. hdrbridge.cpp
// keeps the display ID lookup for itself and logs it there, so in practice only
// the driver settings reads come through this path.

using PFN_GetDisplayIdByDisplayName = int(__cdecl *)(const char *, uint32_t *);
using PFN_DRS_GetSetting = int(__cdecl *)(void *, void *, uint32_t, void *);
static PFN_GetDisplayIdByDisplayName real_GetDisplayIdByDisplayName;
static PFN_DRS_GetSetting real_DRS_GetSetting;

static int __cdecl hook_GetDisplayIdByDisplayName(const char *name, uint32_t *id)
{
    const int r = real_GetDisplayIdByDisplayName(name, id);
    tlog("[trace] NvAPI_DISP_GetDisplayIdByDisplayName(%s) -> status %d, id 0x%X", name ? name : "(null)", r, (r == 0 && id) ? *id : 0);
    return r;
}

static int __cdecl hook_DRS_GetSetting(void *session, void *profile, uint32_t setting, void *out)
{
    const int r = real_DRS_GetSetting(session, profile, setting, out);
    if (first_time("drs " + std::to_string(setting)))
        tlog("[trace] NvAPI_DRS_GetSetting(setting 0x%08X) -> status %d", setting, r);
    return r;
}

void *trace_wrap_nvapi(unsigned id, void *fn)
{
    if (fn == nullptr)
        return fn;
    if (id == 0xAE457190)
    {
        real_GetDisplayIdByDisplayName = reinterpret_cast<PFN_GetDisplayIdByDisplayName>(fn);
        return reinterpret_cast<void *>(&hook_GetDisplayIdByDisplayName);
    }
    if (id == 0x73BF8338)
    {
        real_DRS_GetSetting = reinterpret_cast<PFN_DRS_GetSetting>(fn);
        return reinterpret_cast<void *>(&hook_DRS_GetSetting);
    }
    return fn;
}

template <typename T>
static void hook_export(HMODULE module, const char *name, T hook, T *real)
{
    void *target = module != nullptr ? reinterpret_cast<void *>(GetProcAddress(module, name)) : nullptr;
    if (target == nullptr ||
        MH_CreateHook(target, reinterpret_cast<void *>(hook), reinterpret_cast<void **>(real)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
        tlog("[trace] could not hook %s", name);
}

void trace_install()
{
    HMODULE kb = LoadLibraryW(L"kernelbase.dll");
    hook_export(kb, "RegOpenKeyExA", &hook_RegOpenKeyExA, &real_RegOpenKeyExA);
    hook_export(kb, "RegOpenKeyExW", &hook_RegOpenKeyExW, &real_RegOpenKeyExW);
    hook_export(kb, "RegQueryValueExA", &hook_RegQueryValueExA, &real_RegQueryValueExA);
    hook_export(kb, "RegQueryValueExW", &hook_RegQueryValueExW, &real_RegQueryValueExW);
    hook_export(kb, "RegGetValueA", &hook_RegGetValueA, &real_RegGetValueA);
    hook_export(kb, "RegGetValueW", &hook_RegGetValueW, &real_RegGetValueW);

    HMODULE setupapi = LoadLibraryW(L"setupapi.dll");
    hook_export(setupapi, "SetupDiGetClassDevsW", &hook_SetupDiGetClassDevsW, &real_SetupDiGetClassDevsW);
    hook_export(setupapi, "SetupDiGetDeviceRegistryPropertyW", &hook_SetupDiGetDeviceRegistryPropertyW, &real_SetupDiGetDeviceRegistryPropertyW);
    hook_export(setupapi, "SetupDiGetDevicePropertyW", &hook_SetupDiGetDevicePropertyW, &real_SetupDiGetDevicePropertyW);
    hook_export(setupapi, "SetupDiGetDeviceInterfaceDetailW", &hook_SetupDiGetDeviceInterfaceDetailW, &real_SetupDiGetDeviceInterfaceDetailW);
    tlog("[trace] registry and SetupAPI tracing installed");
}
