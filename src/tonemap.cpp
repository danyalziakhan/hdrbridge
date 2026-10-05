// Runs shaders/tonemap.hlsl around ReShade's effects: the "before" passes turn
// the game's HDR frame into SDR for the effects, and the "after" pass hands
// their result back to the swap chain. Where each stage runs, and why it is
// not simply ReShade's begin and finish effects events, is at g_converted.
//
// tonemap.manifest says what to create and run, in the same format fxshot
// reads, so a frame measured offline is the frame the game shows.

#include "tonemap.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <psapi.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace reshade::api;

extern std::atomic<bool> g_reshade_alive;

namespace
{
// Everything here logs on a change of state, never per frame, so a single
// session in game shows what happened and when without filling the log.
void logf(reshade::log::level level, const char *fmt, ...)
{
    if (!g_reshade_alive)
        return;
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    reshade::log::message(level, buf);
}
#define LOG(...)  logf(reshade::log::level::info, "tone mapping: " __VA_ARGS__)
#define WARN(...) logf(reshade::log::level::warning, "tone mapping: " __VA_ARGS__)

//----------------------------------------------------------------------------
// The manifest

struct tex_def
{
    std::string name;
    bool back_buffer = false;
    std::string width, height, levels;  // numbers, or W, H and METER
    format fmt = format::unknown;
    bool blue_noise = false;
    bool hud = false;     // HUD Mask's view of the game's HUD, bound per frame
    std::string mips_if;  // mips generated only while this setting is on
};

struct uni_def
{
    std::string type, name, source;
    // Doubles, so the frame counter stays exact however long the game runs.
    double value = 0.0, fallback = 0.0;
    int offset = 0;
    // How the overlay shows it
    std::string widget, label, category, tip, unit;
    float lo = 0.0f, hi = 1.0f, step = 0.01f;
    std::vector<std::string> items;
    std::string group;       // folded under this name inside its section
    bool advanced = false;   // drawn under Advanced in its section
    bool no_preset = false;  // a game setting a preset leaves alone
};

struct pass_def
{
    std::string name, ps, target;
    bool has_target2 = false;
    std::string target2;  // written as SV_Target1; empty, like target, for the back buffer
    bool after = false;
    std::string run_if;  // pass run only while this setting is on
    bool run_if_off = false;  // or, written !Setting, only while it is off
    pipeline pipe = {0};
};

struct samstate_def { bool point = false, wrap = false; };

// A named look for the settings in one category. The first preset is the
// defaults; the others list only what they change.
struct preset_def
{
    std::string name, tip;
    std::vector<std::pair<std::string, double>> values;
};

std::vector<tex_def> g_texdefs;
std::vector<samstate_def> g_samstates;
std::vector<uni_def> g_unis;
std::vector<pass_def> g_passes;
std::vector<preset_def> g_presets;
std::string g_preset_category;  // the settings a preset covers
int g_preset = 0;               // the preset those settings started from
std::string g_hlsl;
std::vector<uint8_t> g_blue_noise;

HMODULE g_module = nullptr;
std::wstring g_cfg_path;
bool g_enabled = true;

format format_from(const std::string &s)
{
    if (s == "R16G16B16A16_FLOAT") return format::r16g16b16a16_float;
    if (s == "R32G32B32A32_FLOAT") return format::r32g32b32a32_float;
    if (s == "R8G8B8A8_UNORM") return format::r8g8b8a8_unorm;
    return format::unknown;
}

std::string load_resource(const wchar_t *name)
{
    HRSRC r = FindResourceW(g_module, name, MAKEINTRESOURCEW(10));
    if (r == nullptr)
        return {};
    HGLOBAL h = LoadResource(g_module, r);
    const char *p = static_cast<const char *>(LockResource(h));
    return std::string(p, p + SizeofResource(g_module, r));
}

uni_def *find_uni(const std::string &name)
{
    for (uni_def &u : g_unis)
        if (u.name == name)
            return &u;
    return nullptr;
}

int tex_index(const char *name)
{
    for (size_t i = 0; i < g_texdefs.size(); ++i)
        if (g_texdefs[i].name == name)
            return int(i);
    return -1;
}

std::string unescape(std::string s)
{
    for (size_t i = 0; (i = s.find("\\n", i)) != std::string::npos;)
        s.replace(i, 2, "\n");
    return s;
}

void parse_manifest(const std::string &text)
{
    std::istringstream in(text);
    std::string line;
    bool after = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        const std::string rest = line.size() > tag.size() + 1 ? line.substr(tag.size() + 1) : std::string();
        if (tag == "TEX")
        {
            tex_def t;
            std::string kind, fmt, source;
            ls >> t.name >> kind >> t.width >> t.height >> fmt >> t.levels >> source;
            t.back_buffer = kind == "BACKBUFFER";
            t.hud = kind == "HUD";
            t.fmt = format_from(fmt);
            t.blue_noise = source != "-";
            g_texdefs.push_back(t);
        }
        // SAM lines, which texture uses which state, are not read: all three
        // states are bound to every pass and the shader picks its register.
        else if (tag == "SAMSTATE")
        {
            int idx;
            std::string mn, mg, mp, au, av;
            ls >> idx >> mn >> mg >> mp >> au >> av;
            if (int(g_samstates.size()) <= idx)
                g_samstates.resize(idx + 1);
            g_samstates[idx] = {mn == "POINT", au == "WRAP"};
        }
        else if (tag == "UNI")
        {
            uni_def u;
            std::string fallback;
            ls >> u.type >> u.name >> u.source >> fallback >> u.offset;
            if (u.source == "-")
                u.source.clear();
            u.fallback = fallback == "-" ? 0.0 : atof(fallback.c_str());
            u.value = u.fallback;
            g_unis.push_back(u);
        }
        else if (tag == "STAGE")
        {
            after = rest == "after";
        }
        else if (tag == "PASS")
        {
            pass_def p;
            std::string vs;
            ls >> p.name >> vs >> p.ps >> p.target;
            if (p.target == "-")
                p.target.clear();
            if (ls >> p.target2)
            {
                p.has_target2 = true;
                if (p.target2 == "-")
                    p.target2.clear();
            }
            p.after = after;
            g_passes.push_back(p);
        }
        else if (tag == "UI")
        {
            std::string name;
            ls >> name;
            if (uni_def *u = find_uni(name))
                ls >> u->widget >> u->lo >> u->hi >> u->step;
        }
        else if (tag == "SKIPIF" || tag == "MIPSIF")
        {
            std::string name, setting;
            ls >> name >> setting;
            if (tag == "SKIPIF")
            {
                const bool off = !setting.empty() && setting[0] == '!';
                for (pass_def &p : g_passes)
                    if (p.name == name)
                    {
                        p.run_if = off ? setting.substr(1) : setting;
                        p.run_if_off = off;
                    }
            }
            else
            {
                for (tex_def &t : g_texdefs)
                    if (t.name == name)
                        t.mips_if = setting;
            }
        }
        else if (tag == "ADVANCED" || tag == "NOPRESET")
        {
            if (uni_def *u = find_uni(rest))
                (tag == "ADVANCED" ? u->advanced : u->no_preset) = true;
        }
        else if (tag == "GROUP")
        {
            const size_t sp = rest.find(' ');
            if (sp != std::string::npos)
                if (uni_def *u = find_uni(rest.substr(0, sp)))
                    u->group = rest.substr(sp + 1);
        }
        else if (tag == "UNIT")
        {
            const size_t sp = rest.find(' ');
            if (sp != std::string::npos)
                if (uni_def *u = find_uni(rest.substr(0, sp)))
                    u->unit = rest.substr(sp + 1);
        }
        else if (tag == "PRESETCATEGORY")
        {
            g_preset_category = rest;
        }
        else if (tag == "PRESET")
        {
            preset_def p;
            ls >> p.name;
            for (std::string kv; ls >> kv;)
            {
                const size_t eq = kv.find('=');
                if (eq != std::string::npos && find_uni(kv.substr(0, eq)) != nullptr)
                    p.values.emplace_back(kv.substr(0, eq), atof(kv.c_str() + eq + 1));
                else
                    WARN("preset %s names %s, which is not a setting", p.name.c_str(), kv.c_str());
            }
            g_presets.push_back(p);
        }
        else if (tag == "PRESETTIP")
        {
            const size_t sp = rest.find(' ');
            for (preset_def &p : g_presets)
                if (sp != std::string::npos && p.name == rest.substr(0, sp))
                    p.tip = unescape(rest.substr(sp + 1));
        }
        else if (tag == "LABEL" || tag == "CATEGORY" || tag == "TIP" || tag == "ITEMS")
        {
            const size_t sp = rest.find(' ');
            uni_def *u = find_uni(rest.substr(0, sp));
            const std::string value = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            if (u == nullptr)
                continue;
            if (tag == "LABEL") u->label = value;
            else if (tag == "CATEGORY") u->category = value;
            else if (tag == "TIP") u->tip = unescape(value);
            else
            {
                std::istringstream items(value);
                for (std::string item; std::getline(items, item, '|');)
                    u->items.push_back(item);
            }
        }
    }
}

//----------------------------------------------------------------------------
// Presets

// A slider hands back a float, so compare at that precision.
bool same(double a, double b)
{
    return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b));
}

bool in_preset(const uni_def &u)
{
    return !g_presets.empty() && u.category == g_preset_category && !u.no_preset;
}

double preset_value(int p, const uni_def &u)
{
    for (const auto &kv : g_presets[p].values)
        if (kv.first == u.name)
            return kv.second;
    return u.fallback;
}

// Where a setting goes back to when reset: the chosen preset's value for the
// settings presets cover, the default for the rest.
double base_value(const uni_def &u)
{
    return in_preset(u) ? preset_value(g_preset, u) : u.fallback;
}

void apply_preset(int p)
{
    std::string moved;
    for (const uni_def &u : g_unis)
        if (in_preset(u) && !same(u.value, preset_value(p, u)))
        {
            char buf[96];
            snprintf(buf, sizeof(buf), " %s %g->%g", u.name.c_str(), u.value, preset_value(p, u));
            moved += buf;
        }
    LOG("preset %s%s", g_presets[p].name.c_str(), moved.empty() ? ", nothing to change" : (":" + moved).c_str());
    g_preset = p;
    for (uni_def &u : g_unis)
        if (in_preset(u))
            u.value = preset_value(p, u);
}

bool preset_edited()
{
    for (const uni_def &u : g_unis)
        if (in_preset(u) && !same(u.value, preset_value(g_preset, u)))
            return true;
    return false;
}

//----------------------------------------------------------------------------
// hdrbridge.cfg: Name=value per setting, # for comments. Preset=Name picks a
// preset by name, so a game that uses one follows it when it is retuned. The
// other lines are what was changed from that preset, or from the default for
// settings no preset covers.

std::vector<std::string> read_cfg()
{
    std::vector<std::string> lines;
    if (FILE *f = _wfopen(g_cfg_path.c_str(), L"r"))
    {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f))
        {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    return lines;
}

std::string value_text(const uni_def &u)
{
    char buf[32];
    if (u.type == "float")
        snprintf(buf, sizeof(buf), "%g", float(u.value));
    else
        snprintf(buf, sizeof(buf), "%d", int(u.value));
    return buf;
}

void load_cfg()
{
    const std::vector<std::string> lines = read_cfg();
    char path[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, g_cfg_path.c_str(), -1, path, sizeof(path), nullptr, nullptr);
    if (lines.empty())
        LOG("no settings file at %s, using the defaults", path);
    else
        LOG("reading %s, %d lines", path, int(lines.size()));

    std::string applied;
    for (const std::string &l : lines)
        if (l.rfind("Preset=", 0) == 0)
        {
            bool found = false;
            for (int i = 0; i < int(g_presets.size()); i++)
                if (g_presets[i].name == l.substr(7))
                {
                    apply_preset(i);
                    found = true;
                }
            if (!found)
                WARN("no preset is called \"%s\", starting from %s", l.c_str() + 7,
                     g_presets.empty() ? "the defaults" : g_presets[0].name.c_str());
        }

    for (const std::string &l : lines)
    {
        const size_t eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos)
            continue;
        std::string key = l.substr(0, eq);
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        if (key == "Enabled")
            g_enabled = atoi(l.c_str() + eq + 1) != 0;
        else if (key == "Preset")
            continue;
        else if (uni_def *u = find_uni(key))
        {
            // Held to what the panel can set, so a hand edited file cannot ask
            // for something the slider would never reach.
            const char *text = l.c_str() + eq + 1;
            char *end = nullptr;
            const double asked = std::strtod(text, &end);
            while (end != nullptr && (*end == ' ' || *end == '\t'))
                ++end;
            if (end == text || end == nullptr || *end != '\0' || !std::isfinite(asked))
            {
                WARN("%s=%s is not a number, keeping %g", key.c_str(), l.c_str() + eq + 1, u->value);
                continue;
            }
            double v = asked;
            if (!u->items.empty())
                v = std::clamp(v, 0.0, double(u->items.size() - 1));
            else if (!u->widget.empty())
                v = std::clamp(v, double(u->lo), double(u->hi));
            if (v != asked)
                WARN("%s=%g is outside %g to %g, using %g", key.c_str(), asked,
                     u->items.empty() ? double(u->lo) : 0.0,
                     u->items.empty() ? double(u->hi) : double(u->items.size() - 1), v);
            u->value = v;
            char buf[96];
            snprintf(buf, sizeof(buf), " %s=%g", key.c_str(), v);
            applied += buf;
        }
        else
            WARN("%s in the settings file is not a setting, ignored", key.c_str());
    }
    LOG("%s, preset %s%s%s%s", g_enabled ? "on" : "off (Enabled=0)",
        g_presets.empty() ? "none" : g_presets[g_preset].name.c_str(), preset_edited() ? " with edits (Custom)" : "",
        applied.empty() ? "" : ", from the file:", applied.c_str());
}

// Rewrites the settings and keeps the comments, which is where a preset author
// names the game and the preset the file belongs to.
void save_cfg()
{
    std::vector<std::string> out;
    for (const std::string &l : read_cfg())
        if (!l.empty() && l[0] == '#')
            out.push_back(l);
    if (out.empty())
        out.push_back("# HDR Bridge tone mapping for this game. Settings not listed keep their preset or default value.");
    out.push_back(std::string("Enabled=") + (g_enabled ? "1" : "0"));
    if (g_preset > 0)
        out.push_back("Preset=" + g_presets[g_preset].name);
    for (const uni_def &u : g_unis)
        if (u.source.empty() && !same(u.value, base_value(u)))
            out.push_back(u.name + "=" + value_text(u));
    if (FILE *f = _wfopen(g_cfg_path.c_str(), L"w"))
    {
        std::string written;
        for (const std::string &l : out)
        {
            fprintf(f, "%s\n", l.c_str());
            if (l[0] != '#')
                written += " " + l;
        }
        fclose(f);
        LOG("saved hdrbridge.cfg:%s", written.c_str());
    }
    else
        WARN("could not write the settings file, error %lu", GetLastError());
}

//----------------------------------------------------------------------------
// GPU side, rebuilt when the back buffer's size, format or color space changes

struct texture
{
    resource res = {0};
    resource_view srv = {0}, rtv = {0};
    uint32_t width = 0, height = 0, levels = 1;
    format fmt = format::unknown;
};

// The panel's readouts: timestamps around both stages, and small copies of
// the frame statistics, a probed pixel and a reduced frame for the histogram.
// Each frame uses one of a few slots and reads back the slot it is about to
// reuse, several frames old, so the CPU never waits on the GPU. All optional:
// without them the panel shows less and nothing else changes.
constexpr uint32_t kSlots = 4;

// Timestamp layout within one slot: the tone map stage's start and the end
// of its copy, the output stage's start and the end of its copy, and one mark
// after each pass, in manifest order.
uint32_t before_pass_count();
uint32_t ts_per_slot();
uint32_t ts_after_start();
uint32_t ts_of_pass(size_t pass);

struct monitor
{
    query_heap queries = {0};
    uint64_t frequency = 0;
    resource stats[kSlots] = {};
    resource probe_hdr[kSlots] = {}, probe_sdr[kSlots] = {};
    resource hist = {0};
    uint32_t hist_w = 0, hist_h = 0, hist_mip = 0;
    uint64_t hist_frame = 0;
    bool hist_pending = false;
    bool timed_before[kSlots] = {}, timed_after[kSlots] = {}, copied[kSlots] = {}, probed[kSlots] = {};
};

struct gpu_state
{
    uint32_t width = 0, height = 0;
    format back_format = format::unknown;
    int color_space = 0;
    std::vector<texture> textures;  // manifest order; the back buffer copy is a texture too
    texture dummy;                  // stands in for a pass's own target among its inputs
    std::map<uint64_t, resource_view> back_rtvs;  // for running without ReShade's pass
    // Shader views of the back buffers themselves. A null view means the swap
    // chain does not allow one, and the frame is copied instead.
    std::map<uint64_t, resource_view> back_srvs;
    sampler samplers[3] = {};
    pipeline_layout layout = {0};
    monitor mon;
    bool ready = false;
    bool failed = false;
};

gpu_state g_gpu;
uint64_t g_frame_count = 0;
std::chrono::high_resolution_clock::time_point g_last_frame;

uint32_t meter_mips(uint32_t w, uint32_t h)
{
    const uint32_t big = std::max(w, h);
    return big >= 4096 ? 13 : big >= 2048 ? 12 : big >= 1024 ? 11 : big >= 512 ? 10 : 9;
}

uint32_t resolve(const std::string &v, uint32_t w, uint32_t h)
{
    if (v == "W") return w;
    if (v == "H") return h;
    if (v == "METER") return meter_mips(w, h);
    return uint32_t(atoi(v.c_str()));
}

void log_error(const char *msg)
{
    reshade::log::message(reshade::log::level::error, msg);
}

bool compile(const std::string &entry, const char *target, const std::vector<std::string> &defs, std::vector<uint8_t> &out)
{
    static HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto fn = compiler != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (fn == nullptr)
        return false;
    std::vector<D3D_SHADER_MACRO> macros;
    for (size_t i = 0; i + 1 < defs.size(); i += 2)
        macros.push_back({defs[i].c_str(), defs[i + 1].c_str()});
    macros.push_back({nullptr, nullptr});
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = fn(g_hlsl.data(), g_hlsl.size(), "tonemap.hlsl", macros.data(), nullptr, entry.c_str(), target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors != nullptr)
    {
        if (FAILED(hr))
            log_error(static_cast<const char *>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || code == nullptr)
        return false;
    const auto *b = static_cast<const uint8_t *>(code->GetBufferPointer());
    out.assign(b, b + code->GetBufferSize());
    code->Release();
    return true;
}

void publish_output();

void release_gpu(device *dev)
{
    if (g_gpu.ready)
        LOG("released the %ux%u resources", g_gpu.width, g_gpu.height);
    for (texture &t : g_gpu.textures)
    {
        if (t.srv.handle) dev->destroy_resource_view(t.srv);
        if (t.rtv.handle) dev->destroy_resource_view(t.rtv);
        if (t.res.handle) dev->destroy_resource(t.res);
    }
    if (g_gpu.dummy.srv.handle) dev->destroy_resource_view(g_gpu.dummy.srv);
    for (const auto &[res, rtv] : g_gpu.back_rtvs)
        if (rtv.handle) dev->destroy_resource_view(rtv);
    for (const auto &[res, srv] : g_gpu.back_srvs)
        if (srv.handle) dev->destroy_resource_view(srv);
    if (g_gpu.dummy.res.handle) dev->destroy_resource(g_gpu.dummy.res);
    for (sampler s : g_gpu.samplers)
        if (s.handle) dev->destroy_sampler(s);
    for (pass_def &p : g_passes)
    {
        if (p.pipe.handle) dev->destroy_pipeline(p.pipe);
        p.pipe = {0};
    }
    if (g_gpu.layout.handle) dev->destroy_pipeline_layout(g_gpu.layout);
    monitor &m = g_gpu.mon;
    if (m.queries.handle) dev->destroy_query_heap(m.queries);
    for (uint32_t s = 0; s < kSlots; ++s)
        for (resource r : {m.stats[s], m.probe_hdr[s], m.probe_sdr[s]})
            if (r.handle) dev->destroy_resource(r);
    if (m.hist.handle) dev->destroy_resource(m.hist);
    g_gpu = gpu_state();
    publish_output();
}

bool make_texture(device *dev, texture &t, uint32_t w, uint32_t h, uint32_t levels, format fmt, bool target,
                  const subresource_data *initial = nullptr)
{
    t.width = w;
    t.height = h;
    t.levels = levels;
    t.fmt = fmt;
    resource_usage usage = resource_usage::shader_resource;
    if (target)
        usage |= resource_usage::render_target;
    const resource_flags flags = levels > 1 ? resource_flags::generate_mipmaps : resource_flags::none;
    const resource_desc desc(w, h, 1, uint16_t(levels), fmt, 1, memory_heap::default_, usage, flags);
    if (!dev->create_resource(desc, initial, resource_usage::shader_resource, &t.res))
        return false;
    if (!dev->create_resource_view(t.res, resource_usage::shader_resource, resource_view_desc(fmt, 0, levels, 0, 1), &t.srv))
        return false;
    if (target && !dev->create_resource_view(t.res, resource_usage::render_target, resource_view_desc(fmt, 0, 1, 0, 1), &t.rtv))
        return false;
    return true;
}

bool build_gpu(device *dev, resource back_buffer, int color_space)
{
    const resource_desc bb = dev->get_resource_desc(back_buffer);
    const format back_typed = format_to_default_typed(bb.texture.format, 0);
    gpu_state &g = g_gpu;
    g.width = bb.texture.width;
    g.height = bb.texture.height;
    g.back_format = bb.texture.format;
    g.color_space = color_space;

    char w[16], h[16], rw[32], rh[32], cs[8];
    snprintf(w, sizeof(w), "%u", g.width);
    snprintf(h, sizeof(h), "%u", g.height);
    snprintf(rw, sizeof(rw), "(1.0/%u)", g.width);
    snprintf(rh, sizeof(rh), "(1.0/%u)", g.height);
    snprintf(cs, sizeof(cs), "%d", color_space);
    const std::vector<std::string> defs = {"BUFFER_WIDTH", w, "BUFFER_HEIGHT", h, "BUFFER_RCP_WIDTH", rw,
                                           "BUFFER_RCP_HEIGHT", rh, "BUFFER_COLOR_SPACE", cs};

    // Textures. The back buffer is read through a copy, since a swap chain
    // buffer is not always created with shader access.
    for (const tex_def &d : g_texdefs)
    {
        texture t;
        bool ok;
        if (d.back_buffer)
        {
            t.width = g.width;
            t.height = g.height;
            t.fmt = back_typed;
            const resource_desc desc(g.width, g.height, 1, 1, format_to_typeless(bb.texture.format), 1, memory_heap::default_,
                                     resource_usage::copy_dest | resource_usage::shader_resource);
            ok = dev->create_resource(desc, nullptr, resource_usage::shader_resource, &t.res) &&
                 dev->create_resource_view(t.res, resource_usage::shader_resource, resource_view_desc(back_typed), &t.srv);
        }
        else if (d.hud)
        {
            // A blank stand-in, alpha 0, for frames without a HUD texture.
            std::vector<uint8_t> blank(4, 0);
            subresource_data data = {blank.data(), 4, 0};
            ok = make_texture(dev, t, 1, 1, 1, format::r8g8b8a8_unorm, false, &data);
        }
        else if (d.blue_noise)
        {
            subresource_data data = {g_blue_noise.data(), 512 * 4, 0};
            ok = g_blue_noise.size() == 512 * 256 * 4 && make_texture(dev, t, 512, 256, 1, d.fmt, false, &data);
        }
        else
        {
            ok = make_texture(dev, t, resolve(d.width, g.width, g.height), resolve(d.height, g.width, g.height),
                              resolve(d.levels, g.width, g.height), d.fmt, true);
        }
        g.textures.push_back(t);
        if (!ok)
            return false;
    }
    std::vector<uint8_t> zero(4, 0);
    subresource_data zd = {zero.data(), 4, 0};
    if (!make_texture(dev, g.dummy, 1, 1, 1, format::r8g8b8a8_unorm, false, &zd))
        return false;

    for (size_t i = 0; i < 3 && i < g_samstates.size(); ++i)
    {
        sampler_desc sd;
        sd.filter = g_samstates[i].point ? filter_mode::min_mag_mip_point : filter_mode::min_mag_mip_linear;
        sd.address_u = sd.address_v = sd.address_w = g_samstates[i].wrap ? texture_address_mode::wrap : texture_address_mode::clamp;
        if (!dev->create_sampler(sd, &g.samplers[i]))
            return false;
    }

    pipeline_layout_param params[3];
    params[0] = descriptor_range{0, 0, 0, uint32_t(g.textures.size()), shader_stage::pixel, 1, descriptor_type::shader_resource_view};
    params[1] = descriptor_range{0, 0, 0, 3, shader_stage::pixel, 1, descriptor_type::sampler};
    params[2] = constant_range{0, 0, 0, uint32_t(g_unis.size()), shader_stage::pixel};
    if (!dev->create_pipeline_layout(3, params, &g.layout))
        return false;

    std::vector<uint8_t> vs;
    if (!compile("PostProcessVS", "vs_5_0", defs, vs))
        return false;
    for (pass_def &p : g_passes)
    {
        std::vector<uint8_t> ps;
        if (!compile(p.ps, "ps_5_0", defs, ps))
            return false;
        format rt_formats[2] = {back_typed, back_typed};
        for (size_t i = 0; i < g_texdefs.size(); ++i)
        {
            if (g_texdefs[i].name == p.target)
                rt_formats[0] = g.textures[i].fmt;
            if (p.has_target2 && g_texdefs[i].name == p.target2)
                rt_formats[1] = g.textures[i].fmt;
        }
        shader_desc vs_desc = {vs.data(), vs.size()};
        shader_desc ps_desc = {ps.data(), ps.size()};
        primitive_topology topology = primitive_topology::triangle_list;
        rasterizer_desc rasterizer;
        rasterizer.cull_mode = cull_mode::none;
        depth_stencil_desc depth;
        depth.depth_enable = false;
        pipeline_subobject subobjects[] = {
            {pipeline_subobject_type::vertex_shader, 1, &vs_desc},
            {pipeline_subobject_type::pixel_shader, 1, &ps_desc},
            {pipeline_subobject_type::render_target_formats, p.has_target2 ? 2u : 1u, rt_formats},
            {pipeline_subobject_type::primitive_topology, 1, &topology},
            {pipeline_subobject_type::rasterizer_state, 1, &rasterizer},
            {pipeline_subobject_type::depth_stencil_state, 1, &depth},
        };
        if (!dev->create_pipeline(g.layout, uint32_t(std::size(subobjects)), subobjects, &p.pipe))
            return false;
    }

    monitor &m = g.mon;
    dev->create_query_heap(query_type::timestamp, kSlots * ts_per_slot(), &m.queries);
    auto readback = [dev](uint32_t w, uint32_t h, format fmt, resource &out) {
        dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::readback, resource_usage::copy_dest),
                             nullptr, resource_usage::copy_dest, &out);
    };
    const int stats_i = tex_index("TexStats"), hdr_i = tex_index("TexHdr"), proxy_i = tex_index("TexProxy"), meter_i = tex_index("TexMeter");
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        if (stats_i >= 0) readback(1, 1, g.textures[stats_i].fmt, m.stats[s]);
        if (hdr_i >= 0) readback(1, 1, g.textures[hdr_i].fmt, m.probe_hdr[s]);
        if (proxy_i >= 0) readback(1, 1, g.textures[proxy_i].fmt, m.probe_sdr[s]);
    }
    // A quarter of the frame each way, block averaged by the meter's mips: plenty
    // for a histogram, and a fraction of a full frame to copy.
    if (meter_i >= 0 && g.textures[meter_i].levels > 2)
    {
        m.hist_mip = 2;
        m.hist_w = std::max(1u, g.textures[meter_i].width >> 2);
        m.hist_h = std::max(1u, g.textures[meter_i].height >> 2);
        readback(m.hist_w, m.hist_h, g.textures[meter_i].fmt, m.hist);
    }
    g.ready = true;
    return true;
}

// The swap chain's color space, read at present, which is where ReShade gets
// its BUFFER_COLOR_SPACE from. The enum's values are that macro's: 2 is scRGB,
// 3 is HDR10. Anything else is stored as 0 and the frame is left alone.
int g_color_space = 0;
effect_runtime *g_runtime = nullptr;
// Every live runtime, and g_runtime's swap chain once one of its presents has
// been recognized by its back buffers.
std::vector<effect_runtime *> g_runtimes;
swapchain *g_runtime_sc = nullptr;
bool g_logged_other_sc = false;
uint64_t g_present_count = 0;  // presents of g_runtime's swap chain, tone mapped or not

// The conversion to SDR does not wait for ReShade's effect pass. ReShade skips
// that pass while effects are loading, toggled off or absent, and the raw HDR
// frame would reach the screen. So the "before" stage runs at present, ahead
// of everything ReShade does, and the "after" stage runs wherever the frame
// ends up: after the effects when they ran, otherwise at the end of present.
bool g_converted = false;
bool g_output_done = false;

// Whether the frame leaving the swap chain is sRGB, read from the game's own
// thread when it labels its swap chain. Published wherever one of its inputs
// changes rather than read from g_gpu, which the render thread rebuilds.
std::atomic<bool> g_outputs_srgb{false};

void publish_output()
{
    g_outputs_srgb = g_enabled && g_gpu.ready;
}

// Where the last frame's output stage ran, and why a frame was left alone, so
// each change is logged once.
enum class route { none, at_present, after_effects, end_of_present };
route g_last_route = route::none;
std::string g_last_skip = "-";
int g_last_color_space = -1;
int g_last_effects = -1;

void note_route(route r)
{
    if (r == g_last_route)
        return;
    g_last_route = r;
    LOG("%s", r == route::at_present ? "output at present, effects are switched off"
            : r == route::after_effects ? "output after the effects"
            : "output at the end of present, ReShade skipped its effect pass (loading, or no effects)");
}

void note_skip(const char *why)
{
    if (g_last_skip == why)
        return;
    g_last_skip = why;
    if (*why)
    {
        LOG("not tone mapping: %s", why);
        g_last_route = route::none;
    }
}

void run_at_present(command_list *cmd, resource back_buffer, bool before, bool after);

// The game's HUD texture, from the HUD Mask add-on when it is loaded. Its
// export is found by name in whichever module carries it, so neither add-on
// has to link against the other.
using hud_frame_fn = int (*)(void *dev, uint64_t *srv);
hud_frame_fn g_hud_fn = nullptr;
uint64_t g_hud_next_lookup = 0;
int g_hud_lookups = 0;
enum class hud_mask { missing, too_old, found };
hud_mask g_hud_mask = hud_mask::missing;
// -1 when HUD Mask was not asked this frame, else its answer: 0 no HUD, 1 a
// texture in g_hud_srv, 2 drawn onto the back buffer. The view is HUD Mask's
// and only lives for this frame, so it is never kept past it.
int g_hud_kind = -1;
int g_hud_logged = 0;  // bit per kind already logged
resource_view g_hud_srv = {0};

// HUD Mask before 0.3 has no hudmask_frame_texture, but like every add-on it
// exports NAME, a pointer to its name. The pointer is only followed into the
// module's own image, since another module's NAME could be anything.
bool is_old_hud_mask(HMODULE module)
{
    const auto name = reinterpret_cast<const char *const *>(GetProcAddress(module, "NAME"));
    MODULEINFO info = {};
    if (name == nullptr || !K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
        return false;
    const char *const base = static_cast<const char *>(info.lpBaseOfDll);
    const auto inside = [&](const void *p, size_t n) {
        const char *c = static_cast<const char *>(p);
        return c >= base && c + n <= base + info.SizeOfImage;
    };
    static const char wanted[] = "HUD Mask";
    return inside(name, sizeof(*name)) && inside(*name, sizeof(wanted)) && std::memcmp(*name, wanted, sizeof(wanted)) == 0;
}

bool is_hdr(color_space cs)
{
    return cs == color_space::scrgb || cs == color_space::hdr10_pq;
}

// The runtime whose back buffers this swap chain presents, or null.
effect_runtime *runtime_of(swapchain *sc)
{
    const uint64_t current = sc->get_current_back_buffer().handle;
    for (effect_runtime *r : g_runtimes)
        for (uint32_t i = 0; i < r->get_back_buffer_count(); ++i)
            if (r->get_back_buffer(i).handle == current)
                return r;
    return nullptr;
}

// A game can present from a second swap chain in turn with its own, for a
// splash screen or a video. The GPU resources fit one back buffer, so tone
// mapping both would rebuild them on every present; only g_runtime's swap
// chain is followed. Another runtime's swap chain takes over only when it is
// in HDR and g_runtime's is not, or has not presented yet, so a runtime made
// for a splash cannot leave the game's own HDR frame unconverted. A swap chain
// no runtime claims is followed until g_runtime's own has been recognized,
// which keeps the old behavior should back buffers ever fail to match.
bool follow(swapchain *sc)
{
    if (g_runtime == nullptr || sc == g_runtime_sc)
        return true;
    effect_runtime *const owner = runtime_of(sc);
    if (owner == g_runtime)
    {
        g_runtime_sc = sc;
        return true;
    }
    if (owner == nullptr && g_runtime_sc == nullptr)
        return true;
    if (owner == nullptr || !is_hdr(sc->get_color_space()) ||
        (g_runtime_sc != nullptr && is_hdr(g_runtime_sc->get_color_space())))
    {
        if (!g_logged_other_sc)
        {
            g_logged_other_sc = true;
            LOG("a second swap chain is presenting too, and is left alone");
        }
        return false;
    }
    LOG("following another swap chain, which is in HDR while the one followed so far is not");
    release_gpu(g_runtime->get_device());
    g_runtime = owner;
    g_runtime_sc = sc;
    g_logged_other_sc = false;
    return true;
}

void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
    if (!follow(sc))
        return;
    ++g_present_count;
    // Not asked yet this frame; query_hud fills it in if the tone mapping runs.
    g_hud_kind = -1;
    g_hud_srv = {0};
    const int cs = static_cast<int>(sc->get_color_space());
    g_color_space = (cs == static_cast<int>(color_space::scrgb) || cs == static_cast<int>(color_space::hdr10_pq)) ? cs : 0;
    if (cs != g_last_color_space)
    {
        g_last_color_space = cs;
        LOG("swap chain color space %d (%s)", cs, cs == 2 ? "scRGB" : cs == 3 ? "HDR10" : cs == 1 ? "sRGB, SDR" : "other");
    }
    g_converted = false;
    g_output_done = false;
    if (g_unis.empty()) { note_skip("the embedded manifest did not load"); return; }
    if (!g_enabled) { note_skip("switched off in the HDR Bridge tab or by Enabled=0"); return; }
    if (g_color_space == 0) { note_skip("the game is not outputting HDR"); return; }
    if (g_runtime == nullptr) { note_skip("ReShade has no effect runtime yet"); return; }
    note_skip("");

    // With effects toggled off nothing runs in between, so finish now, before
    // ReShade draws its overlay on top.
    const bool effects_off = !g_runtime->get_effects_state();
    if (int(effects_off) != g_last_effects)
    {
        g_last_effects = int(effects_off);
        LOG("ReShade effects %s", effects_off ? "switched off" : "switched on");
    }
    run_at_present(queue->get_immediate_command_list(), sc->get_current_back_buffer(), true, effects_off);
    if (effects_off && g_output_done)
        note_route(route::at_present);
}

int color_space_of(effect_runtime *)
{
    return g_color_space;
}

// Ready for this frame's back buffer, rebuilding when its size, format or
// color space changed. A build that failed is not retried until one of them
// changes again, so the error is logged once rather than every frame.
bool prepare(effect_runtime *runtime, resource back_buffer)
{
    const int cs = color_space_of(runtime);
    if (cs == 0)
        return false;
    device *const dev = runtime->get_device();
    const resource_desc bb = dev->get_resource_desc(back_buffer);
    if (bb.texture.samples > 1)
        return false;
    gpu_state &g = g_gpu;
    if (g.ready && g.width == bb.texture.width && g.height == bb.texture.height && g.back_format == bb.texture.format &&
        g.color_space == cs)
        return true;
    if (g.failed && g.width == bb.texture.width && g.height == bb.texture.height && g.color_space == cs)
        return false;
    release_gpu(dev);
    if (!build_gpu(dev, back_buffer, cs))
    {
        release_gpu(dev);
        g_gpu.failed = true;
        g_gpu.width = bb.texture.width;
        g_gpu.height = bb.texture.height;
        g_gpu.color_space = cs;
        log_error("HDR Bridge could not set up tone mapping");
        return false;
    }
    if (g_gpu.mon.queries.handle)
        g_gpu.mon.frequency = runtime->get_command_queue()->get_timestamp_frequency();
    LOG("set up for %ux%u, back buffer format %u, %s, %d passes", bb.texture.width, bb.texture.height,
        static_cast<unsigned>(bb.texture.format), cs == 2 ? "scRGB" : "HDR10", int(g_passes.size()));
    publish_output();
    return true;
}

void copy_back_buffer(command_list *cmd, resource back_buffer)
{
    size_t slot = 0;
    for (size_t i = 0; i < g_texdefs.size(); ++i)
        if (g_texdefs[i].back_buffer)
            slot = i;
    const texture &copy = g_gpu.textures[slot];
    const resource res[2] = {back_buffer, copy.res};
    const resource_usage before[2] = {resource_usage::render_target, resource_usage::shader_resource};
    const resource_usage during[2] = {resource_usage::copy_source, resource_usage::copy_dest};
    cmd->barrier(2, res, before, during);
    cmd->copy_resource(back_buffer, copy.res);
    cmd->barrier(2, res, during, before);
}

// While the tone map stage reads the back buffer in place: the buffer, its
// view, and whether it is still in the shader resource state.
resource g_direct_back = {0};
resource_view g_direct_back_srv = {0};
bool g_back_readable = false;

// The back buffer can be read in place when the swap chain allows shader
// access, which saves the copy. The output stage still copies, since it
// writes the buffer it reads.
resource_view direct_back_view(device *dev, resource back_buffer)
{
    const auto it = g_gpu.back_srvs.find(back_buffer.handle);
    if (it != g_gpu.back_srvs.end())
        return it->second;
    resource_view srv = {0};
    const resource_desc bb = dev->get_resource_desc(back_buffer);
    if (static_cast<uint32_t>(bb.usage & resource_usage::shader_resource) != 0 &&
        !dev->create_resource_view(back_buffer, resource_usage::shader_resource,
                                   resource_view_desc(format_to_default_typed(bb.texture.format, 0)), &srv))
        srv = {0};
    if (g_gpu.back_srvs.empty())
        LOG("%s", srv.handle != 0 ? "reading the back buffer in place, no copy needed"
                                  : "copying the back buffer, since the swap chain does not allow reading it in place");
    g_gpu.back_srvs[back_buffer.handle] = srv;
    return srv;
}

void mark_time(command_list *cmd, uint32_t index);

bool setting_on(const std::string &name)
{
    const uni_def *u = find_uni(name);
    return u == nullptr || u->value != 0.0;
}

hud_frame_fn find_hud_mask()
{
    // Looked for at most every 120 presents while missing, since enumerating
    // modules is not free and HUD Mask may load after this add-on. It loads
    // with ReShade, so ten tries are plenty.
    if (g_hud_fn != nullptr || g_hud_lookups >= 10 || g_present_count < g_hud_next_lookup)
        return g_hud_fn;
    g_hud_next_lookup = g_present_count + 120;
    ++g_hud_lookups;
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return nullptr;
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), DWORD(std::size(modules)));
    bool too_old = false;
    for (DWORD i = 0; i < count && g_hud_fn == nullptr; ++i)
    {
        g_hud_fn = reinterpret_cast<hud_frame_fn>(GetProcAddress(modules[i], "hudmask_frame_texture"));
        too_old = too_old || (g_hud_fn == nullptr && is_old_hud_mask(modules[i]));
    }
    if (g_hud_fn != nullptr)
    {
        g_hud_mask = hud_mask::found;
        LOG("found HUD Mask");
    }
    else if (too_old && g_hud_mask != hud_mask::too_old)
    {
        g_hud_mask = hud_mask::too_old;
        WARN("HUD Mask is loaded but older than 0.3, which Protect HUD needs");
    }
    return g_hud_fn;
}

// Asks HUD Mask for this frame's HUD once, before the tone map stage. The two
// flags it sets also hold for the output stage of the same frame.
void query_hud(device *dev)
{
    g_hud_kind = -1;
    g_hud_srv = {0};
    if (const hud_frame_fn fn = find_hud_mask())
    {
        uint64_t srv = 0;
        const int kind = fn(dev, &srv);
        // A texture answer without a view is no use, so it counts as no HUD.
        g_hud_kind = kind == 1 && srv != 0 ? 1 : kind == 2 ? 2 : 0;
        if (g_hud_kind == 1)
            g_hud_srv = {srv};
    }
    // Each answer is logged the first time only: some games skip the HUD on
    // alternate frames, and logging every change would flood the log.
    if (g_hud_kind > 0 && (g_hud_logged & (1 << g_hud_kind)) == 0)
    {
        g_hud_logged |= 1 << g_hud_kind;
        LOG("HUD Mask: %s", g_hud_kind == 1 ? "using the game's HUD texture"
                                            : "the HUD is drawn onto the back buffer, which Protect HUD cannot use");
    }
    const bool available = g_hud_kind == 1;
    const uni_def *protect = find_uni("ProtectHud");
    if (uni_def *u = find_uni("HudAvailable"))
        u->value = available ? 1.0 : 0.0;
    if (uni_def *u = find_uni("HudActive"))
        u->value = available && protect != nullptr && protect->value != 0.0 ? 1.0 : 0.0;
}

void run_stage(command_list *cmd, bool after, resource_view back_rtv)
{
    gpu_state &g = g_gpu;

    std::vector<uint32_t> constants(g_unis.size());
    for (const uni_def &u : g_unis)
    {
        if (u.offset < 0 || u.offset >= int(constants.size()))
            continue;
        if (u.type == "float")
        {
            const float f = float(u.value);
            memcpy(&constants[u.offset], &f, 4);
        }
        else
        {
            constants[u.offset] = uint32_t(int64_t(u.value));
        }
    }

    for (size_t pi = 0; pi < g_passes.size(); ++pi)
    {
        const pass_def &p = g_passes[pi];
        if (p.after != after)
            continue;
        if (!p.run_if.empty() && setting_on(p.run_if) == p.run_if_off)
        {
            mark_time(cmd, ts_of_pass(pi));
            continue;
        }
        auto tex_of = [](const std::string &name) {
            int found = -1;
            for (size_t i = 0; i < g_texdefs.size(); ++i)
                if (g_texdefs[i].name == name)
                    found = int(i);
            return found;
        };
        const int targets[2] = {tex_of(p.target), p.has_target2 ? tex_of(p.target2) : -1};
        const uint32_t target_count = p.has_target2 ? 2 : 1;

        resource_view rtvs[2] = {back_rtv, back_rtv};
        uint32_t w = g.width, h = g.height;
        for (uint32_t k = 0; k < target_count; ++k)
        {
            if (targets[k] < 0)
                continue;
            const texture &t = g.textures[targets[k]];
            rtvs[k] = t.rtv;
            if (k == 0)
            {
                w = t.width;
                h = t.height;
            }
            cmd->barrier(t.res, resource_usage::shader_resource, resource_usage::render_target);
        }

        // A pass that draws to the back buffer cannot also read it in place.
        const bool writes_back = targets[0] < 0 || (p.has_target2 && targets[1] < 0);
        if (writes_back && g_back_readable)
        {
            cmd->barrier(g_direct_back, resource_usage::shader_resource, resource_usage::render_target);
            g_back_readable = false;
        }

        std::vector<resource_view> srvs;
        for (size_t i = 0; i < g.textures.size(); ++i)
        {
            resource_view v = g.textures[i].srv;
            if (!after && g_direct_back_srv.handle != 0 && g_texdefs[i].back_buffer)
                v = writes_back ? g.dummy.srv : g_direct_back_srv;
            if (g_texdefs[i].hud && g_hud_srv.handle != 0)
                v = g_hud_srv;
            srvs.push_back(int(i) == targets[0] || int(i) == targets[1] ? g.dummy.srv : v);
        }
        std::vector<sampler> samplers;
        for (int i = 0; i < 3; ++i)
            samplers.push_back(g.samplers[i]);

        cmd->bind_render_targets_and_depth_stencil(target_count, rtvs);
        const viewport vp = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
        cmd->bind_viewports(0, 1, &vp);
        const rect scissor = {0, 0, int32_t(w), int32_t(h)};
        cmd->bind_scissor_rects(0, 1, &scissor);
        cmd->bind_pipeline(pipeline_stage::all_graphics, p.pipe);
        cmd->push_descriptors(shader_stage::pixel, g.layout, 0,
                              descriptor_table_update{{}, 0, 0, uint32_t(srvs.size()), descriptor_type::shader_resource_view, srvs.data()});
        cmd->push_descriptors(shader_stage::pixel, g.layout, 1,
                              descriptor_table_update{{}, 0, 0, 3, descriptor_type::sampler, samplers.data()});
        cmd->push_constants(shader_stage::pixel, g.layout, 2, 0, uint32_t(constants.size()), constants.data());
        cmd->draw(3, 1, 0, 0);

        for (uint32_t k = 0; k < target_count; ++k)
        {
            if (targets[k] < 0)
                continue;
            const texture &t = g.textures[targets[k]];
            cmd->barrier(t.res, resource_usage::render_target, resource_usage::shader_resource);
            if (t.levels > 1 && (g_texdefs[targets[k]].mips_if.empty() || setting_on(g_texdefs[targets[k]].mips_if)))
                cmd->generate_mipmaps(t.srv);
        }
        mark_time(cmd, ts_of_pass(pi));
    }
}

// What the panel shows, filled from readbacks a few frames old.
struct readouts
{
    bool stats = false;
    float peak_nits = 0.0f, avg_nits = 0.0f, bright_nits = 0.0f, exposure_stops = 0.0f;
    bool gpu = false;
    float gpu_before_ms = 0.0f, gpu_after_ms = 0.0f;
    float copy_in_ms = 0.0f, copy_out_ms = 0.0f;
    std::vector<float> pass_ms;  // one per manifest pass, mips included where a pass has them
    float cpu_ms = 0.0f, cpu_read_ms = 0.0f, cpu_hist_ms = 0.0f;
    bool probe = false;
    float probe_hdr[3] = {}, probe_sdr[3] = {};
    bool hist = false;
    float bins[64] = {};
    uint64_t updated = 0;
};
readouts g_read;

// Requests from the panel, which is drawn on the same thread as the stages.
bool g_want_hist = false;
bool g_want_probe = false;
uint32_t g_probe_x = 0, g_probe_y = 0;
double g_cpu_this_frame = 0.0;

double g_cpu_read_this_frame = 0.0, g_cpu_hist_this_frame = 0.0;

// Adds the time it lives to a running total for this frame. The readout and
// histogram totals are parts of the whole, which is timed around them.
struct cpu_timer
{
    double &total;
    LARGE_INTEGER start;
    explicit cpu_timer(double &t = g_cpu_this_frame) : total(t) { QueryPerformanceCounter(&start); }
    ~cpu_timer()
    {
        LARGE_INTEGER end, freq;
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&freq);
        total += double(end.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
    }
};

uint32_t slot_of(uint64_t frame) { return uint32_t(frame % kSlots); }

uint32_t before_pass_count()
{
    uint32_t n = 0;
    for (const pass_def &p : g_passes)
        n += p.after ? 0 : 1;
    return n;
}
uint32_t ts_per_slot() { return 4 + uint32_t(g_passes.size()); }
uint32_t ts_after_start() { return 2 + before_pass_count(); }
uint32_t ts_of_pass(size_t pass) { return (g_passes[pass].after ? 4 : 2) + uint32_t(pass); }

void mark_time(command_list *cmd, uint32_t index)
{
    monitor &m = g_gpu.mon;
    if (!m.queries.handle)
        return;
    const uint32_t s = slot_of(g_frame_count == 0 ? 0 : g_frame_count - 1);
    cmd->end_query(m.queries, query_type::timestamp, s * ts_per_slot() + index);
    (index < ts_after_start() ? m.timed_before[s] : m.timed_after[s]) = true;
}

float half_to_float(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)
    {
        if (man == 0)
            bits = sign;
        else
        {
            int e = -1;
            uint32_t m2 = man;
            do { m2 <<= 1; ++e; } while ((m2 & 0x400) == 0);
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m2 & 0x3FF) << 13;
        }
    }
    else if (exp == 31)
        bits = sign | 0x7F800000u | man << 13;
    else
        bits = sign | (exp + 127 - 15) << 23 | man << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// First pixel of a mapped readback, as RGB floats.
void read_pixel(const subresource_data &d, format fmt, uint32_t x, uint32_t y, float out[3])
{
    const uint8_t *row = static_cast<const uint8_t *>(d.data) + size_t(y) * d.row_pitch;
    if (fmt == format::r32g32b32a32_float)
        memcpy(out, row + size_t(x) * 16, 12);
    else
    {
        const uint16_t *p = reinterpret_cast<const uint16_t *>(row + size_t(x) * 8);
        for (int i = 0; i < 3; ++i)
            out[i] = half_to_float(p[i]);
    }
}

double setting(const char *name)
{
    const uni_def *u = find_uni(name);
    return u ? u->value : 0.0;
}

// The shader's GainStops, for the panel. Kept in step with tonemap.hlsl: 203
// is its PaperWhiteReference and 1.0 its HighlightFreeStops.
double exposure_stops(double log_peak, double log_avg, double log_bright)
{
    (void)log_peak;
    double exposure = setting("Exposure");
    if (setting("MatchPaperWhite") != 0.0)
        exposure += std::log2(203.0 / std::max(setting("GamePaperWhite"), 1.0));
    const double to_key = std::log2(setting("AutoExposureKey") / std::exp(log_avg));
    const double auto_ev = to_key * (to_key > 0.0 ? setting("AutoExposureBrighten") : setting("AutoExposureDarken"));
    const double lead = std::max((log_bright - log_avg) / 0.6931472 - 1.0, 0.0);
    const double range = setting("AutoExposureRange");
    return exposure + std::clamp(auto_ev - setting("HighlightAdaptation") * lead, -range, range);
}

void collect_readouts(device *dev)
{
    monitor &m = g_gpu.mon;
    const uint32_t s = slot_of(g_frame_count);
    auto smooth = [](float &avg, double &frame) {
        const float v = float(frame);
        frame = 0.0;
        avg = avg == 0.0f ? v : avg + (v - avg) * 0.05f;
    };
    smooth(g_read.cpu_ms, g_cpu_this_frame);
    smooth(g_read.cpu_read_ms, g_cpu_read_this_frame);
    smooth(g_read.cpu_hist_ms, g_cpu_hist_this_frame);
    const cpu_timer read_timer(g_cpu_read_this_frame);

    if (m.timed_before[s] && m.frequency != 0)
    {
        const uint32_t n = ts_per_slot(), a0 = ts_after_start();
        std::vector<uint64_t> t(n, 0);
        const bool before_ok = dev->get_query_heap_results(m.queries, query_type::timestamp, s * n, a0, t.data(), sizeof(uint64_t));
        const bool after_ok = before_ok && m.timed_after[s] &&
            dev->get_query_heap_results(m.queries, query_type::timestamp, s * n + a0, n - a0, t.data() + a0, sizeof(uint64_t));
        if (before_ok)
        {
            const float to_ms = 1000.0f / float(m.frequency);
            auto ms = [&](uint32_t from, uint32_t to) { return t[to] >= t[from] ? float(t[to] - t[from]) * to_ms : -1.0f; };
            // Each pass is timed from the mark before it: the stage's copy for
            // its first pass, the previous pass for the rest.
            std::vector<float> pass(g_passes.size(), 0.0f);
            uint32_t prev_before = 1, prev_after = a0 + 1, last_before = 1, last_after = a0 + 1;
            bool sane = true;
            for (size_t i = 0; i < g_passes.size(); ++i)
            {
                const uint32_t idx = ts_of_pass(i);
                if (g_passes[i].after && !after_ok)
                    continue;
                uint32_t &prev = g_passes[i].after ? prev_after : prev_before;
                pass[i] = ms(prev, idx);
                sane = sane && pass[i] >= 0.0f && pass[i] < 100.0f;
                prev = idx;
                (g_passes[i].after ? last_after : last_before) = idx;
            }
            const float copy_in = ms(0, 1), copy_out = after_ok ? ms(a0, a0 + 1) : 0.0f;
            const float before = ms(0, last_before), after = after_ok ? ms(a0, last_after) : 0.0f;
            // A pair can straddle a GPU clock change or a reset; skip the absurd ones.
            if (sane && copy_in >= 0.0f && copy_out >= 0.0f && before >= 0.0f && before < 100.0f && after >= 0.0f && after < 100.0f)
            {
                const float k = g_read.gpu ? 0.05f : 1.0f;
                g_read.gpu_before_ms += (before - g_read.gpu_before_ms) * k;
                g_read.gpu_after_ms += (after - g_read.gpu_after_ms) * k;
                g_read.copy_in_ms += (copy_in - g_read.copy_in_ms) * k;
                g_read.copy_out_ms += (copy_out - g_read.copy_out_ms) * k;
                g_read.pass_ms.resize(g_passes.size(), 0.0f);
                for (size_t i = 0; i < pass.size(); ++i)
                    g_read.pass_ms[i] += (pass[i] - g_read.pass_ms[i]) * k;
                g_read.gpu = true;
            }
        }
    }
    m.timed_before[s] = m.timed_after[s] = false;

    const int stats_i = tex_index("TexStats");
    subresource_data d;
    if (m.copied[s] && stats_i >= 0 && dev->map_texture_region(m.stats[s], 0, nullptr, map_access::read_only, &d))
    {
        float v[3];
        read_pixel(d, g_gpu.textures[stats_i].fmt, 0, 0, v);
        dev->unmap_texture_region(m.stats[s], 0);
        if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]))
        {
            g_read.peak_nits = std::exp(v[0]);
            g_read.avg_nits = std::exp(v[1]);
            g_read.bright_nits = std::exp(v[2]);
            g_read.exposure_stops = float(exposure_stops(v[0], v[1], v[2]));
            g_read.stats = true;
        }
    }
    m.copied[s] = false;

    if (m.probed[s] && dev->map_texture_region(m.probe_hdr[s], 0, nullptr, map_access::read_only, &d))
    {
        read_pixel(d, g_gpu.textures[tex_index("TexHdr")].fmt, 0, 0, g_read.probe_hdr);
        dev->unmap_texture_region(m.probe_hdr[s], 0);
        if (dev->map_texture_region(m.probe_sdr[s], 0, nullptr, map_access::read_only, &d))
        {
            read_pixel(d, g_gpu.textures[tex_index("TexProxy")].fmt, 0, 0, g_read.probe_sdr);
            dev->unmap_texture_region(m.probe_sdr[s], 0);
            g_read.probe = true;
        }
    }
    m.probed[s] = false;

    // The histogram copy is read back once it is a few frames old.
    if (m.hist_pending && g_frame_count >= m.hist_frame + kSlots &&
        dev->map_texture_region(m.hist, 0, nullptr, map_access::read_only, &d))
    {
        const cpu_timer hist_timer(g_cpu_hist_this_frame);
        const format fmt = g_gpu.textures[tex_index("TexMeter")].fmt;
        // The meter keeps luminance over 80 in blue: scRGB units. Every half
        // float maps to one bin, so the bin comes from a table on its bits
        // rather than a conversion and a logarithm per pixel.
        auto bin_of = [](float v) {
            const float nits = std::max(v * 80.0f, 1e-4f);
            return uint8_t(std::clamp(int((std::log2(nits) + 4.0f) / 18.0f * 64.0f), 0, 63));
        };
        static std::vector<uint8_t> table;
        if (table.empty())
        {
            table.resize(65536);
            for (uint32_t h = 0; h < 65536; ++h)
            {
                const float v = half_to_float(uint16_t(h));
                table[h] = std::isfinite(v) ? bin_of(v) : 0;
            }
        }
        uint32_t counts[64] = {};
        for (uint32_t y = 0; y < m.hist_h; ++y)
        {
            const uint8_t *row = static_cast<const uint8_t *>(d.data) + size_t(y) * d.row_pitch;
            for (uint32_t x = 0; x < m.hist_w; ++x)
            {
                if (fmt == format::r16g16b16a16_float)
                {
                    uint16_t blue;
                    memcpy(&blue, row + size_t(x) * 8 + 4, 2);
                    counts[table[blue]]++;
                }
                else
                {
                    float px[3];
                    read_pixel(d, fmt, x, y, px);
                    counts[bin_of(px[2])]++;
                }
            }
        }
        float bins[64];
        for (int i = 0; i < 64; ++i)
            bins[i] = float(counts[i]);
        dev->unmap_texture_region(m.hist, 0);
        memcpy(g_read.bins, bins, sizeof(bins));
        g_read.hist = true;
        m.hist_pending = false;
    }
    g_read.updated = g_frame_count;
}

void copy_out(command_list *cmd, int tex, uint32_t mip, const subresource_box *box, resource dst)
{
    if (tex < 0 || !dst.handle)
        return;
    const resource src = g_gpu.textures[tex].res;
    cmd->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
    cmd->copy_texture_region(src, mip, box, dst, 0, nullptr);
    cmd->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);
}

void request_readouts(command_list *cmd)
{
    monitor &m = g_gpu.mon;
    const uint32_t s = slot_of(g_frame_count - 1);
    copy_out(cmd, tex_index("TexStats"), 0, nullptr, m.stats[s]);
    m.copied[s] = m.stats[s].handle != 0;

    if (g_want_probe && g_probe_x < g_gpu.width && g_probe_y < g_gpu.height)
    {
        const subresource_box box = {g_probe_x, g_probe_y, 0, g_probe_x + 1, g_probe_y + 1, 1};
        copy_out(cmd, tex_index(setting_on("EnableHdrDeband") ? "TexHdr" : "TexSource"), 0, &box, m.probe_hdr[s]);
        copy_out(cmd, tex_index("TexProxy"), 0, &box, m.probe_sdr[s]);
        m.probed[s] = m.probe_hdr[s].handle && m.probe_sdr[s].handle;
    }
    if (g_want_hist && !m.hist_pending && m.hist.handle && g_frame_count % 8 == 0)
    {
        copy_out(cmd, tex_index("TexMeter"), m.hist_mip, nullptr, m.hist);
        m.hist_frame = g_frame_count;
        m.hist_pending = true;
    }
    g_want_probe = false;
    g_want_hist = false;
}

void set_frame_values()
{
    const auto now = std::chrono::high_resolution_clock::now();
    const float ms = g_frame_count == 0 ? 0.0f : std::chrono::duration<float, std::milli>(now - g_last_frame).count();
    g_last_frame = now;
    if (uni_def *u = find_uni("FrameTime")) u->value = ms;
    if (uni_def *u = find_uni("FrameCount")) u->value = double(g_frame_count);
    ++g_frame_count;
}

void on_finish_effects(effect_runtime *runtime, command_list *cmd, resource_view rtv, resource_view)
{
    if (runtime != g_runtime || !g_converted || g_output_done)
        return;
    const cpu_timer timer;
    const resource back_buffer = runtime->get_device()->get_resource_from_view(rtv);
    mark_time(cmd, ts_after_start());
    copy_back_buffer(cmd, back_buffer);
    mark_time(cmd, ts_after_start() + 1);
    run_stage(cmd, true, rtv);
    g_output_done = true;
    note_route(route::after_effects);
}

// The end of present. Only reached with the output still to do when ReShade
// skipped its pass, which is while effects load or when there are none. Its
// overlay is already drawn by then, so a loading bar comes out a little dim.
void on_reshade_present(effect_runtime *runtime)
{
    if (runtime != g_runtime || !g_converted || g_output_done)
        return;
    run_at_present(runtime->get_command_queue()->get_immediate_command_list(), runtime->get_current_back_buffer(), false, true);
    if (g_output_done)
        note_route(route::end_of_present);
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    if (std::find(g_runtimes.begin(), g_runtimes.end(), runtime) == g_runtimes.end())
        g_runtimes.push_back(runtime);
    // A runtime for a splash or video swap chain does not take over from a
    // swap chain already showing HDR.
    if (g_runtime != nullptr && runtime != g_runtime && g_runtime_sc != nullptr && is_hdr(g_runtime_sc->get_color_space()))
    {
        LOG("ReShade effect runtime created for a second swap chain");
        return;
    }
    g_runtime = runtime;
    g_runtime_sc = nullptr;
    LOG("ReShade effect runtime created");
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    LOG("ReShade effect runtime destroyed");
    // The resources were made on g_runtime's device, and only that one can
    // release them.
    if (g_runtime == nullptr || runtime->get_device() == g_runtime->get_device())
        release_gpu(runtime->get_device());
    g_runtimes.erase(std::remove(g_runtimes.begin(), g_runtimes.end(), runtime), g_runtimes.end());
    if (runtime == g_runtime)
    {
        // Another swap chain's runtime, if one is left, so its frames are not
        // left unconverted.
        g_runtime = g_runtimes.empty() ? nullptr : g_runtimes.back();
        g_runtime_sc = nullptr;
    }
}

// D3D11's immediate context is the game's own, and outside ReShade's pass
// nothing restores it, so everything the passes touch is saved and put back.
struct d3d11_state
{
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView *dsv = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout *layout = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11HullShader *hs = nullptr;
    ID3D11DomainShader *ds = nullptr;
    ID3D11GeometryShader *gs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11ShaderResourceView *srvs[16] = {};
    ID3D11SamplerState *samplers[4] = {};
    ID3D11Buffer *cb = nullptr;
    ID3D11RasterizerState *rs = nullptr;
    ID3D11BlendState *blend = nullptr;
    FLOAT blend_factor[4] = {};
    UINT sample_mask = 0;
    ID3D11DepthStencilState *depth = nullptr;
    UINT stencil_ref = 0;

    void capture(ID3D11DeviceContext *c)
    {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->RSGetViewports(&viewport_count, viewports);
        c->RSGetScissorRects(&scissor_count, scissors);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetInputLayout(&layout);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 16, srvs);
        c->PSGetSamplers(0, 4, samplers);
        c->PSGetConstantBuffers(0, 1, &cb);
        c->RSGetState(&rs);
        c->OMGetBlendState(&blend, blend_factor, &sample_mask);
        c->OMGetDepthStencilState(&depth, &stencil_ref);
    }

    void apply(ID3D11DeviceContext *c)
    {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        c->RSSetViewports(viewport_count, viewports);
        c->RSSetScissorRects(scissor_count, scissors);
        c->IASetPrimitiveTopology(topology);
        c->IASetInputLayout(layout);
        c->VSSetShader(vs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetShaderResources(0, 16, srvs);
        c->PSSetSamplers(0, 4, samplers);
        c->PSSetConstantBuffers(0, 1, &cb);
        c->RSSetState(rs);
        c->OMSetBlendState(blend, blend_factor, sample_mask);
        c->OMSetDepthStencilState(depth, stencil_ref);
        IUnknown *refs[] = {dsv, layout, vs, hs, ds, gs, ps, cb, rs, blend, depth};
        for (IUnknown *r : refs)
            if (r) r->Release();
        for (IUnknown *r : rtvs)
            if (r) r->Release();
        for (IUnknown *r : srvs)
            if (r) r->Release();
        for (IUnknown *r : samplers)
            if (r) r->Release();
    }
};

void run_at_present(command_list *cmd, resource back_buffer, bool before, bool after)
{
    if (!prepare(g_runtime, back_buffer))
        return;
    device *const dev = cmd->get_device();
    // A failed view stays in the map as a null handle, so it is tried and
    // warned about once per back buffer until the resources are rebuilt.
    const auto [it, first] = g_gpu.back_rtvs.try_emplace(back_buffer.handle);
    resource_view &rtv = it->second;
    if (rtv.handle == 0 && !first)
        return;
    if (rtv.handle == 0)
    {
        const resource_desc bb = dev->get_resource_desc(back_buffer);
        if (!dev->create_resource_view(back_buffer, resource_usage::render_target,
                                       resource_view_desc(format_to_default_typed(bb.texture.format, 0)), &rtv))
        {
            WARN("could not make a render target view of the back buffer, format %u", static_cast<unsigned>(bb.texture.format));
            return;
        }
    }

    const bool d3d11 = dev->get_api() == device_api::d3d11;
    d3d11_state saved;
    if (d3d11)
        saved.capture(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));

    // A no-op on D3D11. On D3D12 the buffer is still in the present state here.
    cmd->barrier(back_buffer, resource_usage::present, resource_usage::render_target);
    const cpu_timer timer;
    if (before)
    {
        collect_readouts(dev);
        set_frame_values();
        query_hud(dev);
        mark_time(cmd, 0);
        g_direct_back_srv = direct_back_view(dev, back_buffer);
        if (g_direct_back_srv.handle != 0)
        {
            g_direct_back = back_buffer;
            cmd->barrier(back_buffer, resource_usage::render_target, resource_usage::shader_resource);
            g_back_readable = true;
        }
        else
        {
            copy_back_buffer(cmd, back_buffer);
        }
        mark_time(cmd, 1);
        run_stage(cmd, false, rtv);
        if (g_back_readable)
            cmd->barrier(back_buffer, resource_usage::shader_resource, resource_usage::render_target);
        g_back_readable = false;
        g_direct_back = {0};
        g_direct_back_srv = {0};
        request_readouts(cmd);
        g_converted = true;
    }
    if (after)
    {
        mark_time(cmd, ts_after_start());
        copy_back_buffer(cmd, back_buffer);
        mark_time(cmd, ts_after_start() + 1);
        run_stage(cmd, true, rtv);
        g_output_done = true;
    }
    cmd->barrier(back_buffer, resource_usage::render_target, resource_usage::present);

    if (d3d11)
        saved.apply(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
}

int decimals(float step)
{
    int d = 0;
    while (d < 4 && std::abs(step * std::pow(10.0f, float(d)) - std::round(step * std::pow(10.0f, float(d)))) > 1e-4f)
        ++d;
    return d;
}
}  // namespace

//----------------------------------------------------------------------------

void tonemap_init(HMODULE module)
{
    g_module = module;
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    g_cfg_path = path;
    g_cfg_path = g_cfg_path.substr(0, g_cfg_path.find_last_of(L"\\/") + 1) + L"hdrbridge.cfg";

    g_hlsl = load_resource(L"TONEMAP_HLSL");
    const std::string noise = load_resource(L"TONEMAP_NOISE");
    g_blue_noise.assign(noise.begin(), noise.end());
    parse_manifest(load_resource(L"TONEMAP_MANIFEST"));
    LOG("shader %d bytes, blue noise %d bytes, %d settings, %d passes, %d presets",
        int(g_hlsl.size()), int(g_blue_noise.size()), int(g_unis.size()), int(g_passes.size()), int(g_presets.size()));
    load_cfg();
}

static void draw_window(effect_runtime *);

void tonemap_register()
{
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_overlay("HDR Bridge", draw_window);
}

bool tonemap_outputs_srgb()
{
    return g_outputs_srgb;
}

// Objects made through a ReShade that has been unloaded cannot be released
// through it, so they are dropped.
void tonemap_forget()
{
    g_runtime = nullptr;
    g_runtimes.clear();
    g_runtime_sc = nullptr;
    for (pass_def &p : g_passes)
        p.pipe = {0};
    g_gpu = gpu_state();
    // HUD Mask goes with the ReShade that loaded it, so its export is looked
    // up again rather than called through a pointer into an unloaded module.
    g_hud_fn = nullptr;
    g_hud_next_lookup = 0;
    g_hud_lookups = 0;
    g_hud_mask = hud_mask::missing;
    g_hud_kind = -1;
    g_hud_logged = 0;
    g_hud_srv = {0};
    publish_output();
}

// ReShade's undo icon (Fork Awesome U+F0E2), from the font its overlay already loads.
static const char *ICON_RESET = "\xef\x83\xa2";

// A line in the Add-ons tab; the settings have a tab of their own.
void tonemap_draw_settings()
{
    ImGui::Separator();
    ImGui::TextDisabled("Tone mapping settings are in the HDR Bridge tab of the overlay.");
}

static bool draw_setting(uni_def &u)
{
    bool edited = false;
    const double was = u.value;
    const float reset_w = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    // Laid out as ReShade's own effect settings: control, label, and a reset
    // button after the label once the value has moved from its default, or
    // from the chosen preset's value.
    const double base = base_value(u);
    const bool modified = !same(u.value, base);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);

    if (u.widget == "check")
    {
        bool b = u.value != 0.0;
        if (ImGui::Checkbox("##v", &b))
        {
            u.value = b ? 1.0 : 0.0;
            edited = true;
        }
    }
    else if (u.widget == "combo")
    {
        int i = int(u.value);
        std::string items;
        for (const std::string &item : u.items)
            items += item + '\0';
        items += '\0';
        if (ImGui::Combo("##v", &i, items.c_str()))
        {
            u.value = i;
            edited = true;
        }
    }
    else if (u.type == "int")
    {
        int i = int(u.value);
        const std::string fmt = u.unit.empty() ? "%d" : "%d " + u.unit;
        if (ImGui::SliderInt("##v", &i, int(u.lo), int(u.hi), fmt.c_str()))
            u.value = i;
        edited = ImGui::IsItemDeactivatedAfterEdit();
    }
    else
    {
        char fmt[48];
        snprintf(fmt, sizeof(fmt), "%%.%df%s%s", decimals(u.step), u.unit.empty() ? "" : " ", u.unit.c_str());
        float f = float(u.value);
        if (ImGui::SliderFloat("##v", &f, u.lo, u.hi, fmt))
            u.value = f;
        edited = ImGui::IsItemDeactivatedAfterEdit();
    }
    const bool hovered = ImGui::IsItemHovered();

    ImGui::SameLine(0.0f, spacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(u.label.c_str());
    if (!u.tip.empty() && (hovered || ImGui::IsItemHovered()))
        ImGui::SetTooltip("%s", u.tip.c_str());

    if (modified)
    {
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button(ICON_RESET, ImVec2(reset_w, 0.0f)))
        {
            u.value = base;
            edited = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(in_preset(u) && g_preset > 0 ? "Reset to the preset's value" : "Reset to default");
    }
    // A slider reports its edit when released, after u.value already moved, so
    // the starting value is held across the drag.
    static std::string dragging;
    static double drag_start = 0.0;
    if (u.value != was && dragging != u.name)
    {
        dragging = u.name;
        drag_start = was;
    }
    if (edited)
    {
        const double from = dragging == u.name ? drag_start : was;
        if (!same(from, u.value))
            LOG("%s %g -> %g%s", u.name.c_str(), from, u.value,
                !same(u.value, base) ? "" : in_preset(u) && g_preset > 0 ? " (back to the preset's value)" : " (back to default)");
        dragging.clear();
    }
    return edited;
}

// Shows the preset the settings below started from, or Custom once any of
// them has moved. Picking a preset, the same one included, sets them all.
static bool draw_preset()
{
    bool edited = false;
    const bool custom = preset_edited();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::BeginCombo("##preset", custom ? "Custom" : g_presets[g_preset].name.c_str()))
    {
        for (int i = 0; i < int(g_presets.size()); i++)
        {
            if (ImGui::Selectable(g_presets[i].name.c_str(), !custom && i == g_preset))
            {
                apply_preset(i);
                edited = true;
            }
            if (!g_presets[i].tip.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", g_presets[i].tip.c_str());
        }
        ImGui::EndCombo();
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    if (hovered || ImGui::IsItemHovered())
        ImGui::SetTooltip("A starting point for the settings below. Moving any of them shows Custom;\n"
                          "picking a preset again puts its values back.");
    return edited;
}

// The tone curve as the panel draws it, kept in step with tonemap.hlsl: BT.2390
// on gray, without the shadow controls, which act around the scene average.
namespace curve
{
double pq_encode(double nits)
{
    const double y = std::pow(std::max(nits, 0.0) / 10000.0, 0.1593017578125);
    return std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

double pq_decode(double e)
{
    const double p = std::pow(std::max(e, 0.0), 1.0 / 78.84375);
    return 10000.0 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

double knee(double target)
{
    const double c = setting("HighlightCompression");
    return std::max((1.0 + c) * target - c, 0.0);
}

double bt2390(double nits, double src_peak, double dst_peak)
{
    const double src_e = pq_encode(src_peak);
    double e = std::min(pq_encode(nits) / src_e, 1.0);
    const double target = pq_encode(dst_peak) / src_e;
    const double ks = knee(target);
    if (e > ks)
    {
        const double t = (e - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
        e = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * target;
    }
    return pq_decode(e * src_e);
}

double srgb(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
}  // namespace curve

static const char *format_name(format f)
{
    switch (f)
    {
    case format::r16g16b16a16_float: return "RGBA16F";
    case format::r10g10b10a2_unorm: return "RGB10A2";
    case format::r8g8b8a8_unorm: return "RGBA8";
    case format::b8g8r8a8_unorm: return "BGRA8";
    default: return "other";
    }
}

static const char *dxgi_color_space_name(int cs)
{
    switch (cs)
    {
    case -1: return "not set by the game";
    case 0: return "sRGB (G22 P709)";
    case 1: return "scRGB (G10 P709)";
    case 12: return "HDR10 (G2084 P2020)";
    default: return "other";
    }
}

// Always visible: what the game sends and what it costs.
static void draw_status()
{
    if (g_gpu.failed)
    {
        ImGui::TextDisabled("Tone mapping could not be set up for this swap chain; see ReShade.log.");
        return;
    }
    if (g_color_space == 0)
    {
        ImGui::TextDisabled("The game is not in HDR, so there is nothing to tone map.");
        return;
    }
    if (!g_enabled)
    {
        ImGui::TextDisabled("Tone mapping is off.");
        return;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "%s %ux%u", g_color_space == 2 ? "scRGB" : "HDR10", g_gpu.width, g_gpu.height);
    if (g_read.stats)
        n += snprintf(line + n, sizeof(line) - n, "   peak %.0f nits   average %.1f nits   exposure %+.2f stops",
                      g_read.peak_nits, g_read.avg_nits, g_read.exposure_stops);
    else
        n += snprintf(line + n, sizeof(line) - n, "   measuring...");
    ImGui::TextUnformatted(line);
    if (g_read.gpu)
        snprintf(line, sizeof(line), "Cost per frame: GPU %.2f ms   CPU %.2f ms", g_read.gpu_before_ms + g_read.gpu_after_ms, g_read.cpu_ms);
    else
        snprintf(line, sizeof(line), "Cost per frame: CPU %.2f ms   GPU timing not available", g_read.cpu_ms);
    ImGui::TextUnformatted(line);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What HDR Bridge adds to each frame, averaged.\n"
                          "GPU, from timestamps: tone map stage %.2f ms, output stage %.2f ms.\n"
                          "CPU: %.2f ms in all, of which reading back the panel's numbers %.2f ms,\n"
                          "and building the histogram %.2f ms, only while the histogram is shown.",
                          g_read.gpu_before_ms, g_read.gpu_after_ms, g_read.cpu_ms, g_read.cpu_read_ms, g_read.cpu_hist_ms);
}

// The scene histogram in nits, log scale, with the tone curve over it.
static void draw_histogram()
{
    g_want_hist = true;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 150.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hist", size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 22, 28, 255));

    // x: log2 nits from 1/16 to 16384.
    auto x_of = [&](double nits) { return float(p0.x + (std::log2(std::max(nits, 1e-4)) + 4.0) / 18.0 * size.x); };
    for (double n : {0.1, 1.0, 10.0, 100.0, 1000.0, 10000.0})
    {
        const float x = x_of(n);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(60, 64, 72, 255));
        char t[16];
        snprintf(t, sizeof(t), n < 1.0 ? "%.1f" : "%.0f", n);
        dl->AddText(ImVec2(x + 2, p1.y - 14), IM_COL32(140, 144, 150, 255), t);
    }
    if (g_read.hist)
    {
        float peak = 1.0f;
        for (float b : g_read.bins)
            peak = std::max(peak, b);
        const float w = size.x / 64.0f;
        for (int i = 0; i < 64; ++i)
        {
            // Square root, so a small population still shows next to a large one.
            const float h = std::sqrt(g_read.bins[i] / peak) * (size.y - 18.0f);
            dl->AddRectFilled(ImVec2(p0.x + i * w, p1.y - 16.0f - h), ImVec2(p0.x + (i + 1) * w - 1.0f, p1.y - 16.0f),
                              IM_COL32(90, 140, 220, 200));
        }
    }
    if (g_read.stats)
    {
        // The curve this frame: scene nits to the SDR level it ends up at.
        const double gain = std::exp2(g_read.exposure_stops);
        const double white = setting("DisplayWhite");
        const double peak = std::max(double(g_read.peak_nits) * gain, white);
        ImVec2 prev;
        for (int i = 0; i <= 120; ++i)
        {
            const double nits = std::exp2(-4.0 + 18.0 * i / 120.0);
            const double out = curve::srgb(curve::bt2390(nits * gain, peak, white) / white);
            const ImVec2 pt(x_of(nits), float(p1.y - 16.0f - out * (size.y - 18.0f)));
            if (i > 0)
                dl->AddLine(prev, pt, IM_COL32(255, 210, 90, 255), 2.0f);
            prev = pt;
        }
        const double src_e = curve::pq_encode(peak);
        const double knee_nits = curve::pq_decode(curve::knee(curve::pq_encode(white) / src_e) * src_e) / gain;
        // Each label on its own row, so two markers close together stay readable.
        auto marker = [&](double nits, ImU32 col, const char *label, int row) {
            const float x = x_of(nits);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y - 16.0f), col, 1.5f);
            dl->AddText(ImVec2(x + 3, p0.y + 2 + row * ImGui::GetTextLineHeight()), col, label);
        };
        marker(g_read.avg_nits, IM_COL32(200, 200, 200, 255), "avg", 0);
        marker(knee_nits, IM_COL32(255, 160, 60, 255), "knee", 1);
        marker(g_read.peak_nits, IM_COL32(255, 80, 80, 255), "peak", 2);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scene brightness in nits, before exposure, from a quarter-size copy of the frame.\n"
                          "Yellow: the SDR level each brightness ends up at. Knee: where the highlight\n"
                          "roll-off starts. Shadow Contrast and Shadow Lift are not drawn.");
}

// Reads the pixel under the cursor while the overlay is open.
static void draw_probe()
{
    const ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || io.DisplaySize.x <= 0.0f)
    {
        ImGui::TextDisabled("Move the cursor over the game to read a pixel.");
        return;
    }
    g_want_probe = true;
    g_probe_x = uint32_t(std::clamp(io.MousePos.x / io.DisplaySize.x * float(g_gpu.width), 0.0f, float(g_gpu.width - 1)));
    g_probe_y = uint32_t(std::clamp(io.MousePos.y / io.DisplaySize.y * float(g_gpu.height), 0.0f, float(g_gpu.height - 1)));
    if (!g_read.probe)
        return;
    const float *h = g_read.probe_hdr, *o = g_read.probe_sdr;
    const float y = (0.2126f * h[0] + 0.7152f * h[1] + 0.0722f * h[2]) * 80.0f;
    ImGui::BeginTooltip();
    ImGui::Text("Pixel %u, %u", g_probe_x, g_probe_y);
    ImGui::Text("HDR    R %.1f   G %.1f   B %.1f nits", h[0] * 80.0f, h[1] * 80.0f, h[2] * 80.0f);
    ImGui::Text("       luminance %.1f nits%s", y, std::min(std::min(h[0], h[1]), h[2]) < 0.0f ? ", outside BT.709" : "");
    ImGui::Text("SDR    %d  %d  %d  (before effects)", int(o[0] * 255.0f + 0.5f), int(o[1] * 255.0f + 0.5f), int(o[2] * 255.0f + 0.5f));
    ImGui::EndTooltip();
}

// A legend for the false color view, in the shader's bands.
static void draw_false_color_legend()
{
    struct band { const char *label; float r, g, b; };
    static const band bands[] = {
        {"< 0.1", 0.10f, 0.02f, 0.20f}, {"0.1-1", 0.10f, 0.15f, 0.65f}, {"1-10", 0.00f, 0.50f, 0.85f},
        {"10-50", 0.10f, 0.70f, 0.30f}, {"50-100", 0.60f, 0.80f, 0.20f}, {"100-250", 0.95f, 0.90f, 0.30f},
        {"250-500", 1.00f, 0.60f, 0.10f}, {"500-1000", 1.00f, 0.25f, 0.10f}, {"1000-4000", 0.90f, 0.00f, 0.35f},
        {"4000+", 1.00f, 1.00f, 1.00f}};
    ImGui::TextDisabled("nits:");
    for (const band &b : bands)
    {
        ImGui::SameLine();
        ImGui::ColorButton(b.label, ImVec4(b.r, b.g, b.b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextUnformatted(b.label);
    }
}

// The compare line, dragged on screen while the overlay is open.
static bool draw_compare_line()
{
    uni_def *on = find_uni("EnableCompare"), *split = find_uni("CompareSplit");
    if (on == nullptr || split == nullptr || on->value == 0.0)
        return false;
    const ImGuiIO &io = ImGui::GetIO();
    const float x = float(split->value) * io.DisplaySize.x;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float mid = io.DisplaySize.y * 0.5f;
    // A handle with two arrows, placed from the line itself rather than from a
    // glyph's metrics, so it is centered in any font.
    const ImU32 arrow = IM_COL32(30, 30, 30, 255);
    dl->AddRectFilled(ImVec2(x - 8, mid - 22), ImVec2(x + 8, mid + 22), IM_COL32(255, 255, 255, 220), 4.0f);
    dl->AddTriangleFilled(ImVec2(x - 2, mid - 5), ImVec2(x - 2, mid + 5), ImVec2(x - 6, mid), arrow);
    dl->AddTriangleFilled(ImVec2(x + 2, mid - 5), ImVec2(x + 6, mid), ImVec2(x + 2, mid + 5), arrow);
    dl->AddText(ImVec2(x - 120, 12), IM_COL32(255, 255, 255, 230), "tone mapped");
    dl->AddText(ImVec2(x + 12, 12), IM_COL32(255, 255, 255, 230), "without");

    static bool dragging = false;
    const bool near_line = std::abs(io.MousePos.x - x) < 10.0f && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    if (near_line || dragging)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (near_line && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        dragging = true;
    if (!dragging)
        return false;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        split->value = std::clamp(double(io.MousePos.x / std::max(io.DisplaySize.x, 1.0f)), 0.0, 1.0);
        return false;
    }
    dragging = false;
    return true;  // released: save
}

// Each pass's share of the GPU time, so an optimization can aim at the biggest.
static void draw_pass_timing()
{
    if (!g_read.gpu || g_read.pass_ms.size() != g_passes.size())
    {
        ImGui::TextDisabled("Waiting for GPU timestamps.");
        return;
    }
    const float total = std::max(g_read.gpu_before_ms + g_read.gpu_after_ms, 1e-4f);
    auto row = [&](const char *name, float ms) {
        ImGui::Text("%-16s %6.3f ms  %5.1f%%", name, ms, 100.0f * ms / total);
        ImGui::SameLine(330.0f);
        const float w = 200.0f * ms / total;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3), ImVec2(p.x + w, p.y + ImGui::GetTextLineHeight() - 1),
                                                  IM_COL32(90, 140, 220, 220));
        ImGui::NewLine();
    };
    ImGui::TextDisabled("Tone map stage, before the effects");
    row(g_gpu.back_srvs.empty() || g_gpu.back_srvs.begin()->second.handle == 0 ? "Copy frame in" : "Read frame in", g_read.copy_in_ms);
    for (size_t i = 0; i < g_passes.size(); ++i)
        if (!g_passes[i].after)
            row(g_passes[i].name.c_str(), g_read.pass_ms[i]);
    ImGui::TextDisabled("Output stage, after the effects");
    row("Copy frame in", g_read.copy_out_ms);
    for (size_t i = 0; i < g_passes.size(); ++i)
        if (g_passes[i].after)
            row(g_passes[i].name.c_str(), g_read.pass_ms[i]);
}

static void draw_developer()
{
    if (find_uni("DebugView") != nullptr && setting("DebugView") == 1.0)
        draw_false_color_legend();

    static bool histogram = true, probe = false, info = false, timing = false;
    ImGui::Checkbox("Histogram", &histogram);
    ImGui::SameLine();
    ImGui::Checkbox("Pixel probe", &probe);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows the HDR and SDR values of the pixel under the cursor.");
    ImGui::SameLine();
    ImGui::Checkbox("Swap chain and display", &info);
    ImGui::SameLine();
    ImGui::Checkbox("GPU time per pass", &timing);
    if (timing)
        draw_pass_timing();
    if (histogram && g_color_space != 0 && g_enabled)
        draw_histogram();
    if (probe && g_color_space != 0 && g_enabled)
        draw_probe();
    if (info)
    {
        const bridge_info b = hdrbridge_info();
        ImGui::Text("Back buffer: %ux%u %s, ReShade sees color space %d", g_gpu.width, g_gpu.height,
                    format_name(g_gpu.back_format), g_color_space);
        ImGui::Text("Game asked for: %s", dxgi_color_space_name(b.game_color_space));
        if (b.hdr10_label >= 0)
            ImGui::Text("Windows is told: %s", dxgi_color_space_name(b.hdr10_label));
        if (b.nvapi_hdr_mode != 0)
            ImGui::Text("NVAPI HDR mode: %d", b.nvapi_hdr_mode);
        ImGui::Text("Display reported to the game: %.0f nits peak, %.0f full frame, %.3f black",
                    b.max_nits, b.max_frame_average, b.min_nits);
        ImGui::Text("Tone map stage %.2f ms, output stage %.2f ms (GPU), %.2f ms (CPU)",
                    g_read.gpu_before_ms, g_read.gpu_after_ms, g_read.cpu_ms);
    }
}

// The HDR Bridge tab.
static void draw_window(effect_runtime *)
{
    bool changed = false;
    draw_status();
    ImGui::Separator();
    if (ImGui::Checkbox("Tone map the HDR frame for effects", &g_enabled))
    {
        changed = true;
        publish_output();
        LOG("switched %s in the HDR Bridge tab", g_enabled ? "on" : "off");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Turns the game's HDR frame into SDR for ReShade's effects and back again after them.\n"
                          "Saved to hdrbridge.cfg beside the add-on.");

    // Sections in the manifest's order. Within one, basic settings come first
    // and the ones marked advanced fold away under them.
    std::vector<std::string> categories;
    for (const uni_def &u : g_unis)
        if (u.source.empty() && !u.widget.empty() &&
            std::find(categories.begin(), categories.end(), u.category) == categories.end())
            categories.push_back(u.category);

    for (const std::string &category : categories)
    {
        const bool tone = category == g_preset_category;
        if (!ImGui::CollapsingHeader(category.c_str(), tone ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            continue;
        if (tone && !g_presets.empty())
            changed |= draw_preset();
        // Protect HUD has nothing to work with until HUD Mask supplies a texture.
        const bool hud = category == "HUD";
        if (hud)
        {
            // Looked up from here too, so the line below is right while the
            // game is not in HDR and the stages do not run.
            find_hud_mask();
            ImGui::BeginDisabled(g_hud_mask != hud_mask::found || g_hud_kind == 2);
        }
        bool any_advanced = false;
        std::vector<std::string> groups;
        for (uni_def &u : g_unis)
        {
            if (!u.source.empty() || u.widget.empty() || u.category != category)
                continue;
            if (u.advanced)
            {
                any_advanced = true;
                continue;
            }
            if (!u.group.empty())
            {
                if (std::find(groups.begin(), groups.end(), u.group) == groups.end())
                    groups.push_back(u.group);
                continue;
            }
            ImGui::PushID(u.name.c_str());
            changed |= draw_setting(u);
            ImGui::PopID();
        }
        for (const std::string &group : groups)
        {
            if (!ImGui::TreeNode((group + "##" + category).c_str()))
                continue;
            for (uni_def &u : g_unis)
            {
                if (!u.source.empty() || u.widget.empty() || u.category != category || u.group != group || u.advanced)
                    continue;
                ImGui::PushID(u.name.c_str());
                changed |= draw_setting(u);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (any_advanced && ImGui::TreeNode((std::string("Advanced##") + category).c_str()))
        {
            for (uni_def &u : g_unis)
            {
                if (!u.source.empty() || u.widget.empty() || u.category != category || !u.advanced)
                    continue;
                ImGui::PushID(u.name.c_str());
                changed |= draw_setting(u);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (hud)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", g_hud_mask == hud_mask::missing ? "HUD Mask is not installed."
                                    : g_hud_mask == hud_mask::too_old ? "Protect HUD needs HUD Mask 0.3 or later."
                                    : g_hud_kind == -1 ? "HUD Mask is installed."
                                    : g_hud_kind == 1 ? "HUD Mask is supplying the game's HUD texture."
                                    : g_hud_kind == 2 ? "This game draws its HUD onto the back buffer, which Protect HUD cannot use."
                                    : "HUD Mask is installed but has no HUD this frame, or is switched off.");
        }
        if (category == "Developer")
            draw_developer();
    }
    changed |= draw_compare_line();

    ImGui::Spacing();
    if (ImGui::Button("Reset all to defaults"))
    {
        for (uni_def &u : g_unis)
            if (u.source.empty())
                u.value = u.fallback;
        g_preset = 0;
        changed = true;
        LOG("every setting reset to its default");
    }
    // Custom is derived rather than stored, so log when a finished edit flips
    // it. Checked only then: mid-drag a slider passes through the preset's
    // value and back without anything having been decided.
    static int last_custom = -1;
    if (changed && !g_presets.empty())
    {
        const int custom = int(preset_edited());
        if (custom != last_custom && last_custom != -1)
            LOG("preset shows %s", custom ? "Custom" : g_presets[g_preset].name.c_str());
        last_custom = custom;
    }
    else if (last_custom == -1 && !g_presets.empty())
        last_custom = int(preset_edited());

    if (changed)
        save_cfg();
}
