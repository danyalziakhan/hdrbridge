"""Make a copy of shaders/tonemap.hlsl and its manifest that fxshot can run.

The add-on fills in the back buffer size and color space when it compiles the
shader. fxshot takes the file as it is, so this writes both with those filled
in. Only the "before" stage is kept by default, which is what reference frames
are measured on.

    python tools\\fxshot_tonemap.py OUTDIR [--width 1920] [--height 1200] [--color-space 2] [--stage before|after|all]
"""
import argparse
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent / "shaders"


def meter_mips(w, h):
    # Matches the METER_MIPS ladder in tonemap.hlsl.
    big = max(w, h)
    for limit, mips in ((4096, 13), (2048, 12), (1024, 11), (512, 10)):
        if big >= limit:
            return mips
    return 9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1200)
    ap.add_argument("--color-space", type=int, default=2)
    ap.add_argument("--stage", choices=("before", "after", "all"), default="before")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    defines = [
        f"#define BUFFER_WIDTH ({a.width})",
        f"#define BUFFER_HEIGHT ({a.height})",
        f"#define BUFFER_RCP_WIDTH ((1.0/{a.width}))",
        f"#define BUFFER_RCP_HEIGHT ((1.0/{a.height}))",
        f"#define BUFFER_COLOR_SPACE ({a.color_space})",
    ]
    hlsl = (HERE / "tonemap.hlsl").read_text(encoding="utf-8")
    (a.out / "effect.hlsl").write_text("\n".join(defines) + "\n" + hlsl, encoding="utf-8")

    back = "R16G16B16A16_FLOAT" if a.color_space == 2 else "R10G10B10A2_UNORM"
    stage, out = None, []
    for line in (HERE / "tonemap.manifest").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        if line.startswith("STAGE "):
            stage = line.split()[1]
            continue
        if line.startswith("PASS ") and a.stage != "all" and stage != a.stage:
            continue
        line = line.replace("BACKBUFFER 0 0 R16G16B16A16_FLOAT", f"BACKBUFFER 0 0 {back}")
        parts = line.split()
        if parts[0] == "SIZE":
            line = f"SIZE {a.width} {a.height}"
        elif parts[0] == "TEX":
            parts = [str(a.width) if p == "W" else str(a.height) if p == "H" else
                     str(meter_mips(a.width, a.height)) if p == "METER" else p for p in parts]
            line = " ".join(parts)
        out.append(line)
    (a.out / "manifest.txt").write_text("\n".join(out) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
