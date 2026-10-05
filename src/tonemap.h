#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Tone mapping of the game's HDR frame to SDR for ReShade effects, and back
// again after them. The shader and its manifest are embedded resources; the
// settings live in hdrbridge.cfg beside the add-on.

void tonemap_init(HMODULE module);
void tonemap_register();
void tonemap_forget();
void tonemap_draw_settings();

// True while the tone mapping is set up and switched on, so the frame reaching
// the swap chain is sRGB whatever color space the game asked for.
bool tonemap_outputs_srgb();

// What the HDR side of the add-on knows, for the developer panel.
struct bridge_info
{
    int game_color_space = -1;  // DXGI color space the game asked for, -1 before it asked
    int hdr10_label = -1;       // what an HDR10 swap chain is really labeled, -1 unknown
    int nvapi_hdr_mode = 0;     // NVAPI HDR mode the game switched to, 0 for none
    float max_nits = 0.0f, min_nits = 0.0f, max_frame_average = 0.0f;
};
bridge_info hdrbridge_info();
