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
};

struct uni_def
{
    std::string type, name, source;
    // Doubles, so the frame counter stays exact however long the game runs.
    double value = 0.0, fallback = 0.0;
    int offset = 0;
    // How the overlay shows it
    std::string widget, label, category, tip;
    float lo = 0.0f, hi = 1.0f, step = 0.01f;
    std::vector<std::string> items;
};

struct pass_def
{
    std::string name, ps, target;
    bool after = false;
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
std::vector<int> g_sam_state;  // per texture slot, which sampler
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
    std::map<std::string, int> sampler_of;
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
            t.fmt = format_from(fmt);
            t.blue_noise = source != "-";
            g_texdefs.push_back(t);
        }
        else if (tag == "SAMSTATE")
        {
            int idx;
            std::string mn, mg, mp, au, av;
            ls >> idx >> mn >> mg >> mp >> au >> av;
            if (int(g_samstates.size()) <= idx)
                g_samstates.resize(idx + 1);
            g_samstates[idx] = {mn == "POINT", au == "WRAP"};
        }
        else if (tag == "SAM")
        {
            std::string sam, tex;
            int state;
            ls >> sam >> tex >> state;
            sampler_of[tex] = state;
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
    for (const tex_def &t : g_texdefs)
        g_sam_state.push_back(sampler_of.count(t.name) ? sampler_of[t.name] : 0);
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
    return !g_presets.empty() && u.category == g_preset_category;
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
            const double asked = atof(l.c_str() + eq + 1);
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
            LOG("%s=%g", key.c_str(), v);
        }
        else
            WARN("%s in the settings file is not a setting, ignored", key.c_str());
    }
    LOG("%s, preset %s%s", g_enabled ? "on" : "off (Enabled=0)",
        g_presets.empty() ? "none" : g_presets[g_preset].name.c_str(), preset_edited() ? " with edits (Custom)" : "");
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

struct gpu_state
{
    uint32_t width = 0, height = 0;
    format back_format = format::unknown;
    int color_space = 0;
    std::vector<texture> textures;  // manifest order; the back buffer copy is a texture too
    texture dummy;                  // stands in for a pass's own target among its inputs
    std::map<uint64_t, resource_view> back_rtvs;  // for running without ReShade's pass
    sampler samplers[3] = {};
    pipeline_layout layout = {0};
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
        dev->destroy_resource_view(rtv);
    if (g_gpu.dummy.res.handle) dev->destroy_resource(g_gpu.dummy.res);
    for (sampler s : g_gpu.samplers)
        if (s.handle) dev->destroy_sampler(s);
    for (pass_def &p : g_passes)
    {
        if (p.pipe.handle) dev->destroy_pipeline(p.pipe);
        p.pipe = {0};
    }
    if (g_gpu.layout.handle) dev->destroy_pipeline_layout(g_gpu.layout);
    g_gpu = gpu_state();
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
        format rt_format = back_typed;
        for (size_t i = 0; i < g_texdefs.size(); ++i)
            if (g_texdefs[i].name == p.target)
                rt_format = g.textures[i].fmt;
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
            {pipeline_subobject_type::render_target_formats, 1, &rt_format},
            {pipeline_subobject_type::primitive_topology, 1, &topology},
            {pipeline_subobject_type::rasterizer_state, 1, &rasterizer},
            {pipeline_subobject_type::depth_stencil_state, 1, &depth},
        };
        if (!dev->create_pipeline(g.layout, uint32_t(std::size(subobjects)), subobjects, &p.pipe))
            return false;
    }
    g.ready = true;
    return true;
}

// The swap chain's color space, read at present, which is where ReShade gets
// its BUFFER_COLOR_SPACE from. The enum's values are that macro's: 2 is scRGB,
// 3 is HDR10. Anything else is stored as 0 and the frame is left alone.
int g_color_space = 0;
effect_runtime *g_runtime = nullptr;

// The conversion to SDR does not wait for ReShade's effect pass. ReShade skips
// that pass while effects are loading, toggled off or absent, and the raw HDR
// frame would reach the screen. So the "before" stage runs at present, ahead
// of everything ReShade does, and the "after" stage runs wherever the frame
// ends up: after the effects when they ran, otherwise at the end of present.
bool g_converted = false;
bool g_output_done = false;

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

void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
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
    LOG("set up for %ux%u, back buffer format %u, %s, %d passes", bb.texture.width, bb.texture.height,
        static_cast<unsigned>(bb.texture.format), cs == 2 ? "scRGB" : "HDR10", int(g_passes.size()));
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

    for (const pass_def &p : g_passes)
    {
        if (p.after != after)
            continue;
        int target = -1;
        for (size_t i = 0; i < g_texdefs.size(); ++i)
            if (g_texdefs[i].name == p.target)
                target = int(i);

        resource_view rtv = back_rtv;
        uint32_t w = g.width, h = g.height;
        if (target >= 0)
        {
            const texture &t = g.textures[target];
            rtv = t.rtv;
            w = t.width;
            h = t.height;
            cmd->barrier(t.res, resource_usage::shader_resource, resource_usage::render_target);
        }

        std::vector<resource_view> srvs;
        for (size_t i = 0; i < g.textures.size(); ++i)
            srvs.push_back(int(i) == target ? g.dummy.srv : g.textures[i].srv);
        std::vector<sampler> samplers;
        for (int i = 0; i < 3; ++i)
            samplers.push_back(g.samplers[i]);

        cmd->bind_render_targets_and_depth_stencil(1, &rtv);
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

        if (target >= 0)
        {
            const texture &t = g.textures[target];
            cmd->barrier(t.res, resource_usage::render_target, resource_usage::shader_resource);
            if (t.levels > 1)
                cmd->generate_mipmaps(t.srv);
        }
    }
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
    if (!g_converted || g_output_done)
        return;
    const resource back_buffer = runtime->get_device()->get_resource_from_view(rtv);
    copy_back_buffer(cmd, back_buffer);
    run_stage(cmd, true, rtv);
    g_output_done = true;
    note_route(route::after_effects);
}

// The end of present. Only reached with the output still to do when ReShade
// skipped its pass, which is while effects load or when there are none. Its
// overlay is already drawn by then, so a loading bar comes out a little dim.
void on_reshade_present(effect_runtime *runtime)
{
    if (!g_converted || g_output_done)
        return;
    run_at_present(runtime->get_command_queue()->get_immediate_command_list(), runtime->get_current_back_buffer(), false, true);
    if (g_output_done)
        note_route(route::end_of_present);
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    g_runtime = runtime;
    LOG("ReShade effect runtime created");
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    LOG("ReShade effect runtime destroyed");
    release_gpu(runtime->get_device());
    if (runtime == g_runtime)
        g_runtime = nullptr;
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
    resource_view &rtv = g_gpu.back_rtvs[back_buffer.handle];
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
    if (before)
    {
        set_frame_values();
        copy_back_buffer(cmd, back_buffer);
        run_stage(cmd, false, rtv);
        g_converted = true;
    }
    if (after)
    {
        copy_back_buffer(cmd, back_buffer);
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
    return g_enabled && g_gpu.ready;
}

// Objects made through a ReShade that has been unloaded cannot be released
// through it, so they are dropped.
void tonemap_forget()
{
    g_runtime = nullptr;
    for (pass_def &p : g_passes)
        p.pipe = {0};
    g_gpu = gpu_state();
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
        if (ImGui::SliderInt("##v", &i, int(u.lo), int(u.hi)))
            u.value = i;
        edited = ImGui::IsItemDeactivatedAfterEdit();
    }
    else
    {
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%.%df", decimals(u.step));
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

// The HDR Bridge tab.
static void draw_window(effect_runtime *)
{
    bool changed = false;
    if (ImGui::Checkbox("Tone map the HDR frame for effects", &g_enabled))
    {
        changed = true;
        LOG("switched %s in the HDR Bridge tab", g_enabled ? "on" : "off");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Turns the game's HDR frame into SDR for ReShade's effects and back again after them.\n"
                          "Saved to hdrbridge.cfg beside the add-on.");
    if (g_gpu.failed)
        ImGui::TextDisabled("Tone mapping could not be set up for this swap chain; see ReShade.log.");
    else if (g_color_space == 0)
        ImGui::TextDisabled("The game is not in HDR, so there is nothing to tone map.");

    std::string category;
    bool open = false;
    for (uni_def &u : g_unis)
    {
        if (!u.source.empty() || u.widget.empty())
            continue;
        if (u.category != category)
        {
            category = u.category;
            open = ImGui::CollapsingHeader(category.c_str(), category == "Tone Mapping" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (open && !g_presets.empty() && category == g_preset_category)
                changed |= draw_preset();
        }
        if (!open)
            continue;
        ImGui::PushID(u.name.c_str());
        changed |= draw_setting(u);
        ImGui::PopID();
    }

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
