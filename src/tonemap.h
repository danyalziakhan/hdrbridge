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
