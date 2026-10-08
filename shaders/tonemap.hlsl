// HDR Bridge tone mapping: turns a game's HDR frame into SDR for ReShade
// effects, then hands their result back to the HDR swap chain.
//
// Shared by the add-on and by fxshot. The add-on compiles it with
// BUFFER_WIDTH, BUFFER_HEIGHT, BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT and
// BUFFER_COLOR_SPACE defined, binds textures and settings as tonemap.manifest
// lays out, and runs its passes. fxshot does the same from the manifest, so
// the shader can be measured against reference frames without a game.
//
// The passes are written in ReShade's tex2D style, through the three macros
// below, so they read like the effects that run between them.

// Pixel settings are authored against this screen height and converted at the
// point of use, so the same settings cover the same fraction of any monitor.
#define REFERENCE_HEIGHT 1080.0

static const float3 LUMA_709 = float3(0.2126, 0.7152, 0.0722);

float GetLuminance(float3 color)
{
    return dot(color, LUMA_709);
}

float ToPixels(float authored)
{
    return max(1.0, authored * (float(BUFFER_HEIGHT) / REFERENCE_HEIGHT));
}

//---------------------------|
// :: Source Color Space :: |
//---------------------------|

// The add-on compiles this for the game's swap chain: 2 is scRGB, linear with
// BT.709 primaries and 1.0 = 80 nits in a half float swap chain, and 3 is
// HDR10, PQ and BT.2020 in a 10-bit one. It does not run on an SDR one.
#ifndef BUFFER_COLOR_SPACE
    #define BUFFER_COLOR_SPACE 2
#endif

Texture2D sTexColor_t : register(t0);
Texture2D sTexBlueNoise_t : register(t1);
Texture2D sTexSource_t : register(t2);
Texture2D sTexHdr_t : register(t3);
Texture2D sTexProxy_t : register(t4);
Texture2D sTexMeter_t : register(t5);
Texture2D sTexTiles_t : register(t6);
Texture2D sTexStats_t : register(t7);
Texture2D sTexStatsLast_t : register(t8);
Texture2D sTexMeterHud_t : register(t9);
Texture2D sTexTilesHud_t : register(t10);
Texture2D sTexHud_t : register(t11);

SamplerState _smp0 : register(s0);  // linear, clamp
SamplerState _smp1 : register(s1);  // point, wrap
SamplerState _smp2 : register(s2);  // point, clamp
#define sTexColor_s _smp0
#define sTexBlueNoise_s _smp1
#define sTexSource_s _smp0
#define sTexHdr_s _smp0
#define sTexProxy_s _smp0
#define sTexMeter_s _smp0
#define sTexTiles_s _smp2
#define sTexStats_s _smp2
#define sTexStatsLast_s _smp2
#define sTexMeterHud_s _smp0
#define sTexTilesHud_s _smp2
#define sTexHud_s _smp0

#define tex2D(s, uv)      s##_t.Sample(s##_s, (uv))
#define tex2Dlod(s, c)    s##_t.SampleLevel(s##_s, (c).xy, (c).w)
#define tex2Dfetch(s, c)  s##_t.Load(int3((int2)(c), 0))

// Settings, in the order the manifest lists them. Booleans are ints.
cbuffer Settings : register(b0)
{
    float DisplayWhite : packoffset(c0.x);
    float Exposure : packoffset(c0.y);
    float AutoExposureBrighten : packoffset(c0.z);
    float AutoExposureDarken : packoffset(c0.w);
    float AutoExposureKey : packoffset(c1.x);
    float HighlightAdaptation : packoffset(c1.y);
    float AdaptBrighter : packoffset(c1.z);
    float AdaptDarker : packoffset(c1.w);
    float HighlightColor : packoffset(c2.x);
    float Saturation : packoffset(c2.y);
    float ShadowContrast : packoffset(c2.z);
    float ShadowSpan : packoffset(c2.w);
    float ShadowLift : packoffset(c3.x);
    int EnableHdrDeband : packoffset(c3.y);
    float HdrDebandThreshold : packoffset(c3.z);
    float HdrDebandRadius : packoffset(c3.w);
    int HdrDebandPasses : packoffset(c4.x);
    int HdrDebandSamples : packoffset(c4.y);
    float HdrDebandDetail : packoffset(c4.z);
    float HdrDebandCorrection : packoffset(c4.w);
    float ColorDeband : packoffset(c5.x);
    int EnableDeband : packoffset(c5.y);
    float DebandMaxCorrection : packoffset(c5.z);
    float DebandSplit : packoffset(c5.w);
    int DebandTaps : packoffset(c6.x);
    int EnableDebandEffect : packoffset(c6.y);
    float DebandEffectThreshold : packoffset(c6.z);
    float DebandEffectRadius : packoffset(c6.w);
    int DebandEffectIterations : packoffset(c7.x);
    float DebandEffectDetail : packoffset(c7.y);
    int EnableDebandSource : packoffset(c7.z);
    float DebandSourceThreshold : packoffset(c7.w);
    float DebandSourceRadius : packoffset(c8.x);
    int DebandSourceIterations : packoffset(c8.y);
    float DebandSourceDetail : packoffset(c8.z);
    int EnableDithering : packoffset(c8.w);
    float DitherStrength : packoffset(c9.x);
    int DebugMeter : packoffset(c9.y);
    int DebugDeband : packoffset(c9.z);
    float FrameTime : packoffset(c9.w);
    int FrameCount : packoffset(c10.x);
    float HighlightCompression : packoffset(c10.y);
    float AutoExposureRange : packoffset(c10.z);
    int MatchPaperWhite : packoffset(c10.w);
    float GamePaperWhite : packoffset(c11.x);
    int StaticGrain : packoffset(c11.y);
    int DebugView : packoffset(c11.z);
    int EnableCompare : packoffset(c11.w);
    float CompareSplit : packoffset(c12.x);
    int ProtectHud : packoffset(c12.y);
    float HudBrightness : packoffset(c12.z);
    int HudAvailable : packoffset(c12.w);
    int HudActive : packoffset(c13.x);
};

// Windows cuts the picture to 8 bits on its way to an SDR display whatever the
// swap chain holds, so that is the step the dither has to hide.
static const float OutputSteps = 255.0;

// Layout of the blue noise atlas. Kept in sync with tools/make_stbn.py.
#define STBN_SIZE  64
#define STBN_COLS  8
#define STBN_DEPTH 32

// Enough mip levels for a full resolution chain to reach 1x1.
#if (BUFFER_WIDTH >= 4096) || (BUFFER_HEIGHT >= 4096)
    #define METER_MIPS 13
#elif (BUFFER_WIDTH >= 2048) || (BUFFER_HEIGHT >= 2048)
    #define METER_MIPS 12
#elif (BUFFER_WIDTH >= 1024) || (BUFFER_HEIGHT >= 1024)
    #define METER_MIPS 11
#elif (BUFFER_WIDTH >= 512) || (BUFFER_HEIGHT >= 512)
    #define METER_MIPS 10
#else
    #define METER_MIPS 9
#endif

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

static const float2 PixelSize  = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
static const float2 ScreenSize = float2(BUFFER_WIDTH, BUFFER_HEIGHT);

//-----------------|
// :: Functions :: |
//-----------------|

// One voxel of the blue noise volume for this pixel, on the given slice. The
// stored bytes are rank order, so shifting to the center of each bin turns the
// 256 levels into an unbiased [0,1) rather than a ramp that reaches both ends.
float3 SampleBlueNoise(int2 pixel, int slice)
{
    int2 cell  = int2(slice % STBN_COLS, slice / STBN_COLS);
    int2 coord = cell * STBN_SIZE + (pixel & int2(STBN_SIZE - 1, STBN_SIZE - 1));
    float3 raw = tex2Dfetch(sTexBlueNoise, coord).rgb;
    return (raw * 255.0 + 0.5) / 256.0;
}

// Reshape a uniform sample into a triangular one over [-0.5, 1.5]. Remapped,
// not summed: a sum of two lookups averages away the pattern's arrangement.
float ReshapeUniformToTriangle(float v)
{
    v = frac(v + 0.5);
    float orig = v * 2.0 - 1.0;
    float rnd  = (orig == 0.0) ? -1.0 : (orig * rsqrt(abs(orig)));
    return rnd - sign(orig) + 0.5;
}

// The sRGB piecewise curve, both ways. Measured: DWM encodes an scRGB swap
// chain for an SDR display with exactly this curve.
float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r,
                  c.g <= 0.04045 ? lo.g : hi.g,
                  c.b <= 0.04045 ? lo.b : hi.b);
}

float3 LinearToSrgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r,
                  c.g <= 0.0031308 ? lo.g : hi.g,
                  c.b <= 0.0031308 ? lo.b : hi.b);
}

// SMPTE ST 2084, nits to signal and back.
float PqEncode(float nits)
{
    float y = pow(max(nits, 0.0) / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float PqDecode(float e)
{
    float p = pow(max(e, 0.0), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * p, 1e-6), 1.0 / 0.1593017578125);
}

// The game's frame as scRGB, whichever HDR form it arrived in. HDR10 is PQ in
// BT.2020, decoded to nits and converted to BT.709; a color outside BT.709
// comes out with a negative channel, as it would in scRGB.
float3 DecodeSource(int2 pixel)
{
    float3 c = tex2Dfetch(sTexColor, clamp(pixel, int2(0, 0), int2(BUFFER_WIDTH - 1, BUFFER_HEIGHT - 1))).rgb;
#if BUFFER_COLOR_SPACE == 3
    float3 nits = float3(PqDecode(c.r), PqDecode(c.g), PqDecode(c.b));
    c = float3(dot(nits, float3( 1.6604910, -0.5876411, -0.0728499)),
               dot(nits, float3(-0.1245505,  1.1328999, -0.0083494)),
               dot(nits, float3(-0.0181508, -0.1005789,  1.1187297))) / 80.0;
#endif
    return c;
}

// A float swap chain can hold NaN and infinity, which some renderers leave in
// single pixels for a frame at a time. Clamping them is not enough: min and max
// turn a NaN channel into one of the bounds, the gamut step then zeroes it and
// keeps the others, and a pixel whose green and blue were NaN comes out pure
// red, flickering as it comes and goes. The bits are tested directly, since
// without strict IEEE mode the compiler may fold isnan away. Only NaN counts:
// an infinity is an overflowed light and is clamped below like any other.
bool Invalid(float3 c)
{
    uint3 b = asuint(c);
    return any((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u);
}

// Beyond 10000 in scRGB, 800000 nits, a value is an engine's overflow rather
// than a light, but it is still the brightest thing on screen. Some engines
// clamp to the half float maximum, and a sun drawn there has to come out
// white. Treated as invalid, it would be filled from neighbors that are
// invalid too, and come out as a black disc.
static const float SourceLimit = 10000.0;

// Above PQ's 10000 nits a value is an effect drawn as bright as the format
// allows, not light: Odyssey draws its hit indicators and lock-on reticle at
// half float maximum. It is drawn like any other highlight, but left out of
// the metering, where a thin ring of it would refit the roll-off to a light
// thousands of times brighter than the sun.
static const float MeterLimit = 10000.0 / 80.0;

float3 ReadScRgb(float2 uv)
{
    return tex2Dlod(sTexSource, float4(uv, 0.0, 0.0)).rgb;
}

// A color with a negative channel is outside BT.709. This is the shape of the
// ACES reference gamut compression: each channel's distance from the brightest
// one is left alone up to a threshold, and the stretch from there out to a limit
// is squeezed into what is left below the gamut edge. The brightest channel
// never moves, so saturated sea and foliage end up on the edge at full
// strength instead of pulled toward gray. The limits are how far the BT.2020
// primaries sit outside BT.709, so anything a game can send lands inside.
static const float3 GamutThreshold = float3(0.95, 0.95, 0.95);
static const float3 GamutLimit     = float3(1.594, 1.087, 1.117);
static const float  GamutPower     = 1.2;

float3 CompressGamut(float3 c)
{
    float ach = max(max(c.r, c.g), c.b);
    if (ach <= 0.0)
        return 0.0;

    float3 d     = (ach - c) / ach;
    float3 range = GamutLimit - GamutThreshold;
    float3 scale = range / pow(pow((1.0 - GamutThreshold) / range, -GamutPower) - 1.0, 1.0 / GamutPower);
    float3 over  = max(d - GamutThreshold, 0.0) / scale;
    d = min(d, GamutThreshold) + scale * over / pow(1.0 + pow(over, GamutPower), 1.0 / GamutPower);

    // Past the limit the distance is still above 1, so clamp what remains.
    return max(ach - d * ach, 0.0);
}

// ITU-R BT.2390 EETF: maps [0, src_peak] onto [0, dst_peak], both in nits. In
// PQ, where steps are perceptually even, it is the identity up to a knee and a
// Hermite spline above that, arriving at the target peak with zero slope. At
// the default Highlight Compression the knee is at 1.5 * target - 0.5 of the
// source range. Anything above the source peak clips there. With the target at
// or above the source the knee sits past the top, so a dim scene passes
// through at its own brightness.
float Bt2390(float nits, float src_peak, float dst_peak)
{
    float src_e  = PqEncode(src_peak);
    float e      = min(PqEncode(nits) / src_e, 1.0);
    float target = PqEncode(dst_peak) / src_e;
    float ks     = max((1.0 + HighlightCompression) * target - HighlightCompression, 0.0);

    if (e > ks)
    {
        float t  = (e - ks) / (1.0 - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        e = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
          + (t3 - 2.0 * t2 + t) * (1.0 - ks)
          + (-2.0 * t3 + 3.0 * t2) * target;
    }
    return PqDecode(e * src_e);
}

// The floor keeps black bars and night sky from dragging the log average to
// nothing. PS_Stats explains why the peak rises faster than it falls.
static const float MeterFloor   = 0.1;  // nits
static const float MeterTime    = 0.5;  // seconds
static const float PeakRiseTime = 0.1;  // seconds

// BT.2408's reference white, which Match Game Paper White compensates toward.
static const float PaperWhiteReference = 203.0;  // nits

// Below the knee BT.2390 passes nits straight through, which leaves the
// deepest shadows well under where an SDR grade usually puts them, most
// visibly in backlit scenes. This lifts the brightest channel by 1 + Shadow
// Lift at black, easing back to no change at ToeEnd of white with a matching
// slope there, and scales the other two with it so hue and saturation hold.
static const float ToeEnd = 0.05;

// Linear SDR level Shadow Contrast never darkens below, about code 2 in sRGB.
static const float ShadowFloor = 0.0005;

float3 Toe(float3 c)
{
    float u = saturate(max(max(c.r, c.g), c.b) / ToeEnd);
    return c * (1.0 + ShadowLift * (1.0 - u) * (1.0 - u));
}

// Manual exposure, auto exposure on the average, and highlight adaptation, in
// stops. A log average hardly moves for a small bright area however intense it
// is, so on its own it barely reacts to looking at the sun. The bright
// level is a center-weighted linear average, which a bright area does move,
// and its lead over the log average is how much of the view is taken by
// bright light. About a stop of lead is ordinary in any scene, so only what is
// beyond that darkens, and scenes with no standout light are left alone.
static const float HighlightFreeStops = 1.0;

float GainStops(float4 stats)
{
    // Separate strengths either side of the Key, so dark scenes can be lifted
    // toward it without pulling daylight down by the same measure. Exposure
    // scales every tone alike and keeps texture, where a shadow curve that
    // lifts the dark end compresses it.
    float to_key  = log2(AutoExposureKey / exp(stats.y));
    float auto_ev = to_key * (to_key > 0.0 ? AutoExposureBrighten : AutoExposureDarken);
    float lead    = max((stats.z - stats.y) / 0.6931472 - HighlightFreeStops, 0.0);
    float exposure = Exposure;
    if (MatchPaperWhite)
        exposure += log2(PaperWhiteReference / max(GamePaperWhite, 1.0));
    return exposure + clamp(auto_ev - HighlightAdaptation * lead, -AutoExposureRange, AutoExposureRange);
}

// scRGB to a gamma encoded SDR image, for the effects to treat as the game's own.
// stats is TexStats: smoothed log peak, log average and log bright level.
float3 ToneMap(float3 scrgb, float4 stats)
{
    float  gain = exp2(GainStops(stats));
    float3 nits = CompressGamut(scrgb) * (80.0 * gain);

    // The measured peak moves with the exposure, so it still lands on white and
    // only what sits below it gets brighter or darker.
    float white = DisplayWhite;
    float peak  = max(exp(stats.x) * gain, white);

    // Per channel, a bright color runs its strongest channel into the knee
    // first and bleaches toward white, which is what the default Highlight
    // Color of 0 gives. Curving the brightest channel and scaling the other
    // two with it keeps hue and saturation instead. Neither can pass white.
    // At 0 the blend returns the per channel result unchanged, so the hue kept
    // one is skipped. Above 0 both are needed, 1 included, since a + (b - a)
    // does not round to b. Keep the else branch as it is: rearranged, the
    // compiler rounds the blend differently.
    float3 c;
    [branch]
    if (HighlightColor <= 0.0)
        c = float3(Bt2390(nits.r, peak, white),
                   Bt2390(nits.g, peak, white),
                   Bt2390(nits.b, peak, white)) / white;
    else
    {
        float3 per_channel = float3(Bt2390(nits.r, peak, white),
                                    Bt2390(nits.g, peak, white),
                                    Bt2390(nits.b, peak, white));
        float  m        = max(max(nits.r, nits.g), nits.b);
        float3 hue_kept = nits * (Bt2390(m, peak, white) / max(m, 1e-6));
        c = lerp(per_channel, hue_kept, HighlightColor) / white;
    }

    // An SDR grade is steeper in the shadows than BT.2390, which passes them
    // through unchanged. With Shadow Lift at 0 and Exposure doing the
    // brightening, HDR shadows usually already match the game's SDR frame and
    // every step of this pushes more of the frame toward black, so it is off
    // by default and kept for games that grade harder. It steepens the stops
    // just below the scene average and hands the slope back further down, so
    // the average and the deepest shadows stay where they were. With x the
    // stops below the average over Shadow Span, the offset is -contrast * span
    // * x^2 exp(-x^2): no crease at the average, the deepest dip one span
    // down, and monotonic up to a contrast of about 2.5, so the slider stops
    // at 2.0, where the steepest shadow stop still keeps a fifth of its slope.
    //
    // Stops from the average say nothing about how close a pixel is to black.
    // On one game with dim HDR output the full dip sent 6% of a frame to code 2
    // or below. So the span shrinks to fit between the average and
    // ShadowFloor, and the dip d is rolled off against h, the stops a pixel has
    // left above the floor: d (1 - exp(-h/d)) never exceeds h.
    if (ShadowContrast > 0.0)
    {
        float y    = max(GetLuminance(c), 1e-7);
        float avg  = max(exp(stats.y) * gain / white, 1e-6);
        float span = clamp(log2(avg / ShadowFloor) * 0.5, 0.5, ShadowSpan);
        float x    = max(-log2(y / avg), 0.0) / span;
        float d    = ShadowContrast * span * x * x * exp(-x * x);
        float h    = max(log2(y / ShadowFloor), 0.0);
        c *= exp2(-d * (1.0 - exp(-h / max(d, 1e-6))));
    }

    c = Toe(c);

    // Saturation about luminance in linear light. A channel pushed below zero
    // goes back to the gamut edge here; one pushed past 1.0 is dealt with next.
    float Y = GetLuminance(c);
    c = CompressGamut(Y + (c - Y) * Saturation);

    // Anything past white is pulled toward luminance until it fits. A pixel
    // whose luminance is itself past white has no color left to give.
    Y = GetLuminance(c);
    float hi = max(max(c.r, c.g), c.b);
    if (Y >= 1.0)
        c = 1.0;
    else if (hi > 1.0)
        c = Y + (c - Y) * ((1.0 - Y) / (hi - Y));

    return LinearToSrgb(saturate(c));
}

float3 SoftLimit(float3 v, float limit)
{
    float3 a    = abs(v);
    float knee  = limit * 0.5;
    float3 over = max(a - knee, 0.0);
    return sign(v) * min(a, knee + over * knee / (knee + over));
}

// An HDR frame can arrive already banded, each channel stepping on its own by
// 1 to 2%, and tone mapping turns that into colored contours two or three SDR
// levels tall. One such case came from an upscaler model rather than the
// game. On clean frames this takes about a tenth of the faintest texture, so
// it is off by default. A step that size
// is a ratio, so the debander works on the log of each channel. One step in
// its settings is 4%: some games dither their band edges, and at 1% the
// Detail Guard read that dither as texture and left the bands alone.
static const float BandStep  = 0.04;  // log units per step
static const float BandFloor = 1e-4;  // scRGB; below this a ratio means nothing

float3 LogScRgb(float2 uv, float lod = 0.0)
{
    return log(clamp(tex2Dlod(sTexSource, float4(uv, 0.0, lod)).rgb, BandFloor, 65504.0));
}

float3 Deband(float2 uv, float3 center, float jitter, int taps)
{
    float2 ps = PixelSize;

    // Channels at or below the floor, which includes the negative ones outside
    // BT.709, have no log and are left exactly as they came.
    float3 valid = step(BandFloor, center);
    float3 lc    = log(clamp(center, BandFloor, 65504.0));

    // Brightness and color are debanded separately. Rainbow contours are color
    // steps, each channel stepping at its own place, while the texture worth
    // keeping lives almost entirely in brightness, so color can be smoothed
    // harder before anything real is lost. Brightness is the mean of the three
    // log channels, color each channel's offset from it.
    const float third = 1.0 / 3.0;
    float cm = max(ColorDeband, 1.0);

    float3 hf = abs(LogScRgb(uv + float2( ps.x, 0.0)) - lc)
              + abs(LogScRgb(uv + float2(-ps.x, 0.0)) - lc)
              + abs(LogScRgb(uv + float2(0.0,  ps.y)) - lc)
              + abs(LogScRgb(uv + float2(0.0, -ps.y)) - lc);
    float  hf_l = dot(hf, third);
    float3 hf_c = abs(hf - hf_l);

    float guard_l = 1.0, guard_c = 1.0;
    if (HdrDebandDetail > 0.0)
    {
        // Brightness is judged by its spread over a 5x5 area rather than the
        // step to the four neighbors. A band's dithered edge is a thin line in
        // a flat plateau and averages out over an area, while texture varies
        // everywhere. A per-pixel test had to sit high to let that dither
        // through, and faint texture, dark stone at night above all, fell
        // under it and was smoothed. Measured in 4% steps on one game: banded
        // sky 0.45 at the 90th percentile, night stone and skin 0.9 to 1.1 at
        // the 10th.
        float s = 0.0, s2 = 0.0;
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            [unroll]
            for (int x = -1; x <= 1; x++)
            {
                float v = dot(LogScRgb(uv + float2(x, y) * 2.0 * ps), third);
                s += v; s2 += v * v;
            }
        }
        float spread     = sqrt(max(s2 / 9.0 - (s / 9.0) * (s / 9.0), 0.0)) / BandStep;
        float measured_c = max(max(hf_c.r, hf_c.g), hf_c.b) * 0.25 / (BandStep * cm);
        guard_l = 1.0 - smoothstep(HdrDebandDetail * 0.6, HdrDebandDetail, spread);
        guard_c = 1.0 - smoothstep(HdrDebandDetail * 0.5, HdrDebandDetail, measured_c);
    }

    if (guard_l <= 0.0 && guard_c <= 0.0)
        return center;

    float  l0    = dot(lc, third);
    float3 c0    = lc - l0;
    float  res_l = l0;
    float3 res_c = c0;

    [loop]
    for (int i = 1; i <= HdrDebandPasses; i++)
    {
        float r_i   = ToPixels(HdrDebandRadius) * float(i);
        float bound = HdrDebandThreshold * BandStep / float(i);
        float angle = (jitter + float(i) * 0.618034) * 6.2831853;

        // Past the first pass the taps land 64 px and more apart, scattered by
        // the jitter, and nearly every one missed the cache, which made this
        // over half the cost of the chain. Bands that wide lose nothing at half
        // resolution: the output moved a quarter as much as one frame's jitter
        // already moves it, and 1 ms was saved at 1200p.
        float  lod     = i > 1 ? 1.0 : 0.0;
        float  bound_c = bound * cm;
        float  sum_w = 0.0, sum_l = 0.0, sumsq_l = 0.0;
        float3 sum_c = 0.0, sumsq_c = 0.0;

        // Each sample counts by how much it resembles this pixel, so a sample
        // across an edge drops out and only the pixel's own side is averaged.
        // An unweighted average let a disc half on a dark pole and half on sky
        // pass as flat, and left a glow of the pole's color along the edge.
        [loop]
        for (int k = 0; k < taps; k++)
        {
            float  t  = (float(k) + 0.5) / float(taps);
            float  a  = angle + float(k) * 2.39996323;
            float2 at = uv + float2(cos(a), sin(a)) * (r_i * sqrt(t)) * ps;

            float3 d   = LogScRgb(at, lod) - lc;
            float  d_l = dot(d, third);
            float3 d_c = d - d_l;
            float  w   = (1.0 - smoothstep(bound, bound * 2.0, abs(d_l)))
                       * (1.0 - smoothstep(bound_c, bound_c * 2.0, max(max(abs(d_c.r), abs(d_c.g)), abs(d_c.b))));
            sum_w += w;
            sum_l += w * d_l;  sumsq_l += w * d_l * d_l;
            sum_c += w * d_c;  sumsq_c += w * d_c * d_c;
        }

        // Too few similar samples means a detail rather than a band.
        float  cover  = smoothstep(0.3, 0.6, sum_w / float(taps));
        float  inv    = 1.0 / max(sum_w, 1e-4);
        float  mean_l = sum_l * inv;
        float3 mean_c = sum_c * inv;
        float  sd_l   = sqrt(max(sumsq_l * inv - mean_l * mean_l, 0.0));
        float3 sd_c   = sqrt(max(sumsq_c * inv - mean_c * mean_c, 0.0));

        float w_l = (1.0 - smoothstep(bound * 0.5, bound, abs(mean_l)))
                  * (1.0 - smoothstep(bound, bound * 2.0, sd_l)) * cover;
        float3 w_c = (1.0 - smoothstep(bound_c * 0.5, bound_c, abs(mean_c)))
                   * (1.0 - smoothstep(bound_c, bound_c * 2.0, sd_c)) * cover;

        res_l = lerp(res_l, l0 + mean_l, w_l * guard_l);
        res_c = lerp(res_c, c0 + mean_c, w_c * guard_c);
    }

    float  limit = HdrDebandCorrection * BandStep;
    float3 res   = (l0 + SoftLimit(res_l - l0, limit)) + (c0 + SoftLimit(res_c - c0, limit * cm));
    return lerp(center, exp(res), valid);
}

// The SDR frame debander, counting in 8-bit steps because that is the cut
// Windows makes on the way out. Three tests have to agree: the neighborhood
// average sits close to the pixel, the samples agree with each other, and the
// value does not change pixel to pixel. The tests read the tone mapped frame;
// the repair lands in what the effects made of it.
float3 DebandSdr(float2 uv, float3 out_center, float3 src_center, float jitter,
                 float threshold, float radius, int iterations, int taps, float detail)
{
    float2 ps = PixelSize;
    float step_size = 1.0 / OutputSteps;

    float guard = 1.0;
    if (detail > 0.0)
    {
        float3 hf = abs(tex2D(sTexProxy, uv + float2( ps.x, 0.0)).rgb - src_center)
                  + abs(tex2D(sTexProxy, uv + float2(-ps.x, 0.0)).rgb - src_center)
                  + abs(tex2D(sTexProxy, uv + float2(0.0,  ps.y)).rgb - src_center)
                  + abs(tex2D(sTexProxy, uv + float2(0.0, -ps.y)).rgb - src_center);

        float measured = max(max(hf.r, hf.g), hf.b) * 0.25 * OutputSteps;
        guard = 1.0 - smoothstep(detail * 0.5, detail, measured);
    }

    if (guard <= 0.0)
        return out_center;

    float3 res = out_center;

    [loop]
    for (int i = 1; i <= iterations; i++)
    {
        float r_i   = ToPixels(radius) * float(i);
        float bound = threshold * step_size / float(i);
        float angle = (jitter + float(i) * 0.618034) * 6.2831853;

        float3 src_sum   = 0.0;
        float3 src_sumsq = 0.0;
        float3 out_sum   = 0.0;

        [loop]
        for (int k = 0; k < taps; k++)
        {
            float  t  = (float(k) + 0.5) / float(taps);
            float  a  = angle + float(k) * 2.39996323;
            float4 at = float4(uv + float2(cos(a), sin(a)) * (r_i * sqrt(t)) * ps, 0.0, 0.0);

            float3 sd_ = tex2Dlod(sTexProxy, at).rgb - src_center;
            float3 od_ = saturate(tex2Dlod(sTexColor, at).rgb) - out_center;

            src_sum   += sd_;
            src_sumsq += sd_ * sd_;
            out_sum   += od_;
        }

        float  inv      = 1.0 / float(taps);
        float3 src_mean = src_sum * inv;
        float3 src_sd   = sqrt(max(src_sumsq * inv - src_mean * src_mean, 0.0));
        float3 out_avg  = out_center + out_sum * inv;

        float3 flat_weight = 1.0 - smoothstep(bound * 0.5, bound, abs(src_mean));
        float3 calm_weight = 1.0 - smoothstep(bound, bound * 2.0, src_sd);

        res = lerp(res, out_avg, flat_weight * calm_weight * guard);
    }

    return out_center + SoftLimit(res - out_center, DebandMaxCorrection / OutputSteps);
}

// Debug meter, top left, four bars 400 px long at 1080p:
//   1. scene log average, smoothed    log scale, 0.1 to 10000 nits
//   2. center-weighted bright level   same scale, smoothed
//   3. measured peak, smoothed        same scale
//   4. exposure gain                  -4 to +4 stops, filled from 0
// Tall ticks mark each decade (0.1, 1, 10, 100, 1000, 10000 nits) or each
// stop, short ticks mark 2 and 5 within a decade.
float3 DrawMeter(float2 uv, float3 c)
{
    float  s     = float(BUFFER_HEIGHT) / REFERENCE_HEIGHT;
    float2 p     = uv * ScreenSize / s;
    float2 o     = float2(16.0, 16.0);
    float  len   = 400.0;
    float  bar_h = 12.0;
    float  pitch = 18.0;

    if (p.x < o.x - 4.0 || p.x > o.x + len + 4.0 || p.y < o.y - 4.0 || p.y > o.y + 4.0 * pitch)
        return c;

    float4 stats = tex2Dfetch(sTexStats, 0);
    int    row   = int(floor((p.y - o.y) / pitch));
    float  y     = p.y - o.y - float(row) * pitch;
    float  x     = (p.x - o.x) / len;
    float3 col   = float3(0.08, 0.08, 0.08);

    if (row < 0 || row > 3 || y > bar_h || x < 0.0 || x > 1.0)
        return col;

    float  value;
    float3 fill;
    bool   stops = (row == 3);
    if (row == 0)      { value = stats.y; fill = float3(0.2, 0.8, 0.3); }
    else if (row == 1) { value = stats.z; fill = float3(0.6, 0.95, 0.6); }
    else if (row == 2) { value = stats.x; fill = float3(1.0, 0.6, 0.15); }
    else               { value = GainStops(stats); fill = float3(0.3, 0.55, 1.0); }

    // log nits to [0, 1] over five decades, or stops to [0, 1] over -4 to +4
    float t = stops ? saturate(value / 8.0 + 0.5) : saturate((value / 2.302585 + 1.0) / 5.0);
    bool  lit = stops ? (x >= min(t, 0.5) && x <= max(t, 0.5)) : (x <= t);
    col = lit ? fill : float3(0.25, 0.25, 0.25);

    float px = 1.0 / len;
    [loop]
    for (int k = 0; k <= 8; k++)
    {
        float tall = stops ? float(k) / 8.0 : float(k) / 5.0;
        if (!stops && k > 5)
            break;
        if (abs(x - tall) < px * 0.75)
            col = float3(1.0, 1.0, 1.0);
        if (!stops && k < 5 && y > bar_h * 0.5)
        {
            if (abs(x - (float(k) + 0.30103) / 5.0) < px * 0.75 || abs(x - (float(k) + 0.69897) / 5.0) < px * 0.75)
                col = float3(0.85, 0.85, 0.85);
        }
    }
    return col;
}

//---------------------|
// :: Pixel Shaders :: |
//---------------------|

// Decode once, and replace an invalid pixel with the mean of its valid direct
// neighbors, or black when none of them is valid either.
float4 PS_Source(VS_OUTPUT input) : SV_Target
{
    int2   pixel = int2(input.pos.xy);
    float3 c     = DecodeSource(pixel);
    float  valid = 1.0;
    if (Invalid(c))
    {
        valid = 0.0;
        float3 sum = 0.0;
        float  n   = 0.0;
        const int2 offsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        [unroll]
        for (int i = 0; i < 4; i++)
        {
            float3 v = DecodeSource(pixel + offsets[i]);
            if (!Invalid(v))
            {
                sum += clamp(v, -SourceLimit, SourceLimit);
                n   += 1.0;
            }
        }
        c = n > 0.0 ? sum / n : 0.0;
    }
    return float4(clamp(c, -SourceLimit, SourceLimit), valid);
}

float4 PS_Deband(VS_OUTPUT input) : SV_Target
{
    float3 c = ReadScRgb(input.uv);

    if (EnableHdrDeband)
    {
        int   slice  = int((uint(FrameCount) + uint(STBN_DEPTH / 2)) % uint(STBN_DEPTH));
        float jitter = SampleBlueNoise(int2(input.uv * ScreenSize), slice).g;
        int   taps   = 8 * (clamp(HdrDebandSamples, 0, 3) + 1);
        c = Deband(input.uv, c, jitter, taps);
    }
    return float4(c, tex2Dlod(sTexSource, float4(input.uv, 0.0, 0.0)).a);
}

// The HDR frame as the later passes see it: debanded, or straight from the
// source when debanding is off and its pass is skipped.
float4 HdrFrame(float2 uv)
{
    return EnableHdrDeband ? tex2D(sTexHdr, uv) : tex2Dlod(sTexSource, float4(uv, 0.0, 0.0));
}

float4 MeterValues(float2 uv)
{
    float3 c = HdrFrame(uv).rgb;
    float  hi = max(max(c.r, c.g), c.b);
    if (hi > MeterLimit)
        return float4(0.0, log(MeterFloor), 0.0, 0.0);
    float  Y = GetLuminance(c) * 80.0;
    return float4(max(hi, 0.0), log(max(Y, MeterFloor)), max(Y, 0.0) / 80.0, 0.0);
}

float4 PS_Meter(VS_OUTPUT input) : SV_Target
{
    return MeterValues(input.uv);
}

// How much of this pixel the game's HUD covers, from HUD Mask: its own HUD
// texture's alpha, so translucent panels count in part.
float HudAlpha(float2 uv)
{
    return HudAvailable ? saturate(tex2Dlod(sTexHud, float4(uv, 0.0, 0.0)).a) : 0.0;
}

// Run in place of PS_Meter while Protect HUD is on. The second target holds the
// same values weighted by how much of each pixel the HUD leaves visible, with
// that weight in alpha, so a block's mip average divided by its alpha is the
// average of the scene alone.
float4 PS_MeterHud(VS_OUTPUT input, out float4 visible : SV_Target1) : SV_Target0
{
    float4 m = MeterValues(input.uv);
    float  v = 1.0 - HudAlpha(input.uv);
    visible  = float4(m.rgb * v, v);
    return m;
}

// Blocks are up to 16 pixels across at 1080p, so the peak describes a lit
// region rather than one hot pixel. Rounded down, since rounding to nearest
// gives 22 px blocks at 768 lines, wide enough to average a small light away.
int BlockMip()
{
    return int(max(0.0, floor(log2(16.0 * float(BUFFER_HEIGHT) / REFERENCE_HEIGHT))));
}

int2 BlockGrid()
{
    int mip = BlockMip();
    return int2(max(BUFFER_WIDTH >> mip, 1), max(BUFFER_HEIGHT >> mip, 1));
}

// The brightest block inside this texel's tile, and the sum of the blocks' log
// luminance. The mean is taken here rather than from the top mip because an
// odd sized level drops its last row or column, and by the top of the chain
// that leaves a reading of the middle of the screen.
float4 PS_Tiles(VS_OUTPUT input) : SV_Target
{
    int  mip  = BlockMip();
    int2 size = BlockGrid();
    int2 tile = int2(input.pos.xy);
    int2 lo   = tile * size / 16;
    int2 hi   = (tile + 1) * size / 16;

    float peak    = 0.0;
    float log_sum = 0.0;
    float lin_sum = 0.0;
    float w_sum   = 0.0;
    [loop]
    for (int y = lo.y; y < hi.y; y++)
    {
        [loop]
        for (int x = lo.x; x < hi.x; x++)
        {
            float2 uv = (float2(x, y) + 0.5) / float2(size);
            float3 m  = tex2Dlod(sTexMeter, float4(uv, 0.0, mip)).rgb;
            peak     = max(peak, m.r);
            log_sum += m.g;

            // Center weight for the bright level, falling to about a third at
            // the top and bottom edges, measured in screen heights.
            float2 d = (float2(x, y) + 0.5 - float2(size) * 0.5) / float(size.y);
            float  w = exp(-dot(d, d) / (2.0 * 0.35 * 0.35));
            lin_sum += w * m.b;
            w_sum   += w;
        }
    }
    return float4(peak, log_sum, lin_sum, w_sum);
}

// PS_Tiles over the HUD weighted meter, two texels per tile: the sums as
// PS_Tiles keeps them, then how much of the tile the HUD leaves visible. A
// block more than three quarters HUD is left out of the peak, since what is
// left of it is too little to describe a lit region.
float4 PS_TilesHud(VS_OUTPUT input) : SV_Target
{
    int  mip  = BlockMip();
    int2 size = BlockGrid();
    int2 tile = int2(input.pos.x / 2, input.pos.y);
    int2 lo   = tile * size / 16;
    int2 hi   = (tile + 1) * size / 16;

    float peak    = 0.0;
    float log_sum = 0.0;
    float lin_sum = 0.0;
    float w_sum   = 0.0;
    float v_sum   = 0.0;
    [loop]
    for (int y = lo.y; y < hi.y; y++)
    {
        [loop]
        for (int x = lo.x; x < hi.x; x++)
        {
            float2 uv = (float2(x, y) + 0.5) / float2(size);
            float4 m  = tex2Dlod(sTexMeterHud, float4(uv, 0.0, mip));
            if (m.a >= 0.25)
                peak = max(peak, m.r / m.a);
            log_sum += m.g;
            v_sum   += m.a;

            float2 d = (float2(x, y) + 0.5 - float2(size) * 0.5) / float(size.y);
            float  w = exp(-dot(d, d) / (2.0 * 0.35 * 0.35));
            lin_sum += w * m.b;
            w_sum   += w * m.a;
        }
    }
    return int(input.pos.x) % 2 == 0 ? float4(peak, log_sum, lin_sum, w_sum) : float4(v_sum, 0.0, 0.0, 0.0);
}

float4 PS_Stats(VS_OUTPUT input) : SV_Target
{
    float peak    = 0.0;
    float log_sum = 0.0;
    float lin_sum = 0.0;
    float w_sum   = 0.0;
    [loop]
    for (int i = 0; i < 256; i++)
    {
        float4 t = tex2Dfetch(sTexTiles, int2(i % 16, i / 16));
        peak     = max(peak, t.r);
        log_sum += t.g;
        lin_sum += t.b;
        w_sum   += t.a;
    }

    int2  grid       = BlockGrid();
    float log_peak   = log(max(peak * 80.0, MeterFloor));
    float log_avg    = log_sum / float(grid.x * grid.y);
    float log_bright = log(max(lin_sum / max(w_sum, 1e-6) * 80.0, MeterFloor));

    // With Protect HUD on, the HUD is metered out, so bright text does not set
    // the peak and a dark panel does not lift the exposure. A full-screen menu
    // leaves nothing to meter and would freeze the exposure, so below 15% of
    // the screen visible it is the whole frame again, and the hand-over is
    // complete by 40%.
    [branch]
    if (HudActive)
    {
        float hpeak = 0.0, hlog = 0.0, hlin = 0.0, hw = 0.0, hv = 0.0;
        [loop]
        for (int j = 0; j < 256; j++)
        {
            float4 t = tex2Dfetch(sTexTilesHud, int2((j % 16) * 2, j / 16));
            hpeak = max(hpeak, t.r);
            hlog += t.g;
            hlin += t.b;
            hw   += t.a;
            hv   += tex2Dfetch(sTexTilesHud, int2((j % 16) * 2 + 1, j / 16)).r;
        }
        float k = smoothstep(0.15, 0.4, hv / float(grid.x * grid.y));
        if (k > 0.0)
        {
            if (hpeak > 0.0)
                log_peak = lerp(log_peak, log(max(hpeak * 80.0, MeterFloor)), k);
            log_avg    = lerp(log_avg, hlog / hv, k);
            log_bright = lerp(log_bright, log(max(hlin / max(hw, 1e-6) * 80.0, MeterFloor)), k);
        }
    }

    // The history starts afresh unless it holds a reading this pass could have
    // written: finite, log nits within reach, and marked. A NaN would otherwise
    // stay for the session, and the texture's first contents are whatever the
    // driver left there. Exponent bits again, so the test survives the compiler.
    float4 last  = tex2Dfetch(sTexStatsLast, 0);
    bool   valid = last.w == 1.0 && !any((asuint(last.xyz) & 0x7F800000u) == 0x7F800000u)
                && all(abs(last.xyz) < 32.0);
    if (!valid)
        return float4(log_peak, log_avg, log_bright, 1.0);

    // Getting brighter is followed faster than getting darker, as eyes do. A
    // rising peak is faster still, since until the filter catches up it clips.
    float dt  = FrameTime * 0.001;
    float tau = (log_peak > last.x) ? PeakRiseTime : MeterTime;
    float lp  = lerp(last.x, log_peak, 1.0 - exp(-dt / tau));
    float la  = lerp(last.y, log_avg,    1.0 - exp(-dt / ((log_avg    > last.y) ? AdaptBrighter : AdaptDarker)));
    float lb  = lerp(last.z, log_bright, 1.0 - exp(-dt / ((log_bright > last.z) ? AdaptBrighter : AdaptDarker)));

    // Nor is a broken result stored: with a time constant that is not a number
    // every update is NaN, and the frame would alternate with the fresh one.
    float3 next = float3(lp, la, lb);
    if (any((asuint(next) & 0x7F800000u) == 0x7F800000u))
        next = float3(log_peak, log_avg, log_bright);
    return float4(next, 1.0);
}

float4 PS_SaveStats(VS_OUTPUT input) : SV_Target
{
    return tex2Dfetch(sTexStats, 0);
}

// The second target is the back buffer, for the effects to work on. It gets the
// value rounded to half precision, as TexProxy stores it, so the effects see
// exactly what the output stage later compares against.
float4 PS_ToneMap(VS_OUTPUT input, out float4 back : SV_Target1) : SV_Target0
{
    float4 stats = tex2Dfetch(sTexStats, 0);
    float4 c     = float4(ToneMap(HdrFrame(input.uv).rgb, stats), 1.0);

    // With Protect HUD on, the HUD is tone mapped like the scene but at the
    // manual exposure alone, from the frame before HDR debanding. Metering that
    // sits exactly at the Key with no standout light makes auto exposure and
    // highlight adaptation add nothing. The scene's peak is kept, so the
    // roll-off still follows it, but that only reaches the HUD's brightest parts.
    [branch]
    if (HudActive)
    {
        float hud = HudAlpha(input.uv);
        if (hud > 0.0)
        {
            float  key   = log(AutoExposureKey);
            float4 still = float4(stats.x, key, key, stats.w);
            float3 own   = tex2Dlod(sTexSource, float4(input.uv, 0.0, 0.0)).rgb * exp2(HudBrightness);
            c.rgb = lerp(c.rgb, ToneMap(own, still), hud);
        }
    }
    back = f16tof32(f32tof16(c));
    return c;
}

//-----------------------|
// :: Developer Views :: |
//-----------------------|

// Scene brightness in bands, read straight from the HDR frame before exposure.
// The panel shows the same bands as a legend.
float3 FalseColor(float nits)
{
    if (nits < 0.1)    return float3(0.10, 0.02, 0.20);
    if (nits < 1.0)    return float3(0.10, 0.15, 0.65);
    if (nits < 10.0)   return float3(0.00, 0.50, 0.85);
    if (nits < 50.0)   return float3(0.10, 0.70, 0.30);
    if (nits < 100.0)  return float3(0.60, 0.80, 0.20);
    if (nits < 250.0)  return float3(0.95, 0.90, 0.30);
    if (nits < 500.0)  return float3(1.00, 0.60, 0.10);
    if (nits < 1000.0) return float3(1.00, 0.25, 0.10);
    if (nits < 4000.0) return float3(0.90, 0.00, 0.35);
    return 1.0;
}

// c is the finished SDR frame; the views read the HDR frame and the tone mapped
// one from before the effects, both still held from the first stage.
float3 DeveloperView(float2 uv, float3 c)
{
    float4 hdr  = HdrFrame(uv);
    float3 sdr  = tex2D(sTexProxy, uv).rgb;
    float  gray = GetLuminance(c) * 0.6;

    if (DebugView == 1)
        c = FalseColor(GetLuminance(hdr.rgb) * 80.0) * (0.6 + 0.4 * GetLuminance(c));
    else if (DebugView == 2)
    {
        // Clipped where any channel reached white, crushed where all reached black.
        c = gray;
        if (any(sdr >= 254.5 / OutputSteps))
            c = float3(1.0, 0.15, 0.15);
        else if (all(sdr <= 0.5 / OutputSteps))
            c = float3(0.15, 0.35, 1.0);
    }
    else if (DebugView == 3)
    {
        // Outside BT.709 a channel goes negative; brighter magenta is further out.
        float out_of = max(max(-hdr.r, -hdr.g), -hdr.b) / max(max(max(hdr.r, hdr.g), hdr.b), 1e-4);
        c = out_of > 0.0 ? lerp(float3(0.5, 0.0, 0.5), float3(1.0, 0.2, 1.0), saturate(out_of * 4.0)) : gray;
    }
    else if (DebugView == 4)
        c = hdr.a < 0.5 ? float3(0.1, 1.0, 0.1) : gray;
    else if (DebugView == 5)
        c = LinearToSrgb(saturate(hdr.rgb));
    else if (DebugView == 6)
        c = lerp(gray * 0.5, float3(0.2, 0.9, 1.0), HudAlpha(uv));

    // The right of the line is what Windows makes of a float frame on an SDR
    // display: linear light clipped at SDR white and sRGB encoded.
    if (EnableCompare)
    {
        if (uv.x > CompareSplit)
            c = LinearToSrgb(saturate(hdr.rgb));
        if (abs(uv.x - CompareSplit) * ScreenSize.x < 1.0)
            c = 1.0;
    }
    return c;
}

// After the effects the frame is gamma encoded SDR, which is not what the swap
// chain holds. Deband and dither against the 8-bit cut Windows makes on the
// way to the monitor, then hand the frame back in the form the swap chain is
// shown as: linear light for scRGB, and the sRGB values themselves for HDR10,
// which the add-on labels sRGB while tone mapping runs. An effect's own dither
// would be lost in that conversion, which is why the dither lives here.
float4 PS_Output(VS_OUTPUT input) : SV_Target
{
    float3 c       = saturate(tex2D(sTexColor, input.uv).rgb);
    float3 effects = c;

    if (EnableDeband || DebugDeband)
    {
        float3 src  = tex2D(sTexProxy, input.uv).rgb;
        int    taps = 8 * (clamp(DebandTaps, 0, 3) + 1);

        // Half a loop from the dither's slice, and on a channel the HDR frame
        // debander does not use, so neither lines up with the other or the grain.
        int   slice  = int((uint(FrameCount) + uint(STBN_DEPTH / 2)) % uint(STBN_DEPTH));
        float jitter = SampleBlueNoise(int2(input.uv * ScreenSize), slice).b;

        // How far the effects moved this pixel, in 8-bit steps. Banding it opened
        // up is gone after hard and banding it left alone more gently.
        float3 moved  = abs(c - src);
        float  effect = smoothstep(DebandSplit * 0.5, DebandSplit, max(max(moved.r, moved.g), moved.b) * OutputSteps);

        float3 strong = c;
        float3 gentle = c;

        [branch]
        if (EnableDebandEffect && effect > 0.0)
            strong = DebandSdr(input.uv, c, src, jitter, DebandEffectThreshold, DebandEffectRadius,
                               DebandEffectIterations, taps, DebandEffectDetail);

        [branch]
        if (EnableDebandSource && effect < 1.0)
            gentle = DebandSdr(input.uv, c, src, jitter, DebandSourceThreshold, DebandSourceRadius,
                               DebandSourceIterations, taps, DebandSourceDetail);

        float3 debanded = saturate(lerp(gentle, strong, effect));

        if (DebugDeband)
        {
            // Magenta: where the output debander moves pixels, or would if it
            // were on, full at two 8-bit steps. Green: where the HDR frame
            // debander moved them, full at two of its own steps. Over the frame
            // in dim gray, so a scene with nothing to mark is not a black screen.
            float3 moved = saturate(abs(debanded - c) * OutputSteps * 0.5);
            float  hdr   = 0.0;
            if (EnableHdrDeband)
            {
                float3 a = log(max(tex2D(sTexHdr, input.uv).rgb, BandFloor));
                float3 b = log(max(tex2Dlod(sTexSource, float4(input.uv, 0.0, 0.0)).rgb, BandFloor));
                hdr = saturate(max(max(abs(a.r - b.r), abs(a.g - b.g)), abs(a.b - b.b)) / BandStep * 0.5);
            }
            float  out_moved = max(max(moved.r, moved.g), moved.b);
            float  picture   = 0.25 * dot(c, float3(0.2126, 0.7152, 0.0722));
            c = max(picture, float3(out_moved, hdr, out_moved));
        }
        else
            c = debanded;
    }

    if (EnableDithering)
    {
        int2   pixel = int2(input.uv * ScreenSize);
        float3 n     = SampleBlueNoise(pixel, StaticGrain ? 0 : int(uint(FrameCount) % uint(STBN_DEPTH)));
        float3 d     = float3(ReshapeUniformToTriangle(n.r),
                              ReshapeUniformToTriangle(n.g),
                              ReshapeUniformToTriangle(n.b)) - 0.5;

        // Banding needs a stretch of near-constant color, so gate on the
        // screen-space slope and leave textured regions alone.
        float g    = abs(ddx(GetLuminance(c))) + abs(ddy(GetLuminance(c)));
        float mask = saturate(1.0 - g * 64.0);
        c = saturate(c + d * (mask * mask * DitherStrength / OutputSteps));
    }

    // The debanding and the dither are taken back off the HUD. The effects stay:
    // one that should leave the HUD alone can read HUD Mask's texture itself.
    [branch]
    if (HudActive)
    {
        float hud = HudAlpha(input.uv);
        if (hud > 0.0)
            c = lerp(c, effects, hud);
    }

    if (DebugView > 0 || EnableCompare)
        c = DeveloperView(input.uv, c);

    if (DebugMeter)
        c = DrawMeter(input.uv, c);

#if BUFFER_COLOR_SPACE == 2
    return float4(SrgbToLinear(c), 1.0);
#else
    return float4(c, 1.0);
#endif
}
