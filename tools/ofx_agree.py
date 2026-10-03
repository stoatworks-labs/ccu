#!/usr/bin/env python3
"""The FFGL build against the OpenFX build, through their real entry points.

    python3 tools/ofx_agree.py [--build build-universal] [--size 320x180]

`cctest --cpu` holds the OpenFX build's CPU copy of the two passes to the GPU
in float, inside the harness. This is the same question asked from outside,
of the shipped artefacts: the FFGL plugin rendered by the real plugin class on
the GPU (`cctest --pipe`, 8-bit in, 8-bit out) against the CCU.ofx bundle
loaded and rendered by an OFX host (`ofxprobe`, from resolume-ofx-bridge,
8-bit in, 8-bit out), on the same picture, at the same settings, compared
byte for byte.

The picture is ofxprobe's own -- it renders a fixed ramp (R = 4x, G = 8y,
B = 128, rows bottom-up as OFX has them) at time 0 and cannot be handed
another -- so the same ramp is built here and piped to cctest, flipped to its
top-first rows. Time 0 is frame 0, where the drift is exactly zero in both
builds; `cctest --cpu` covers a drifting frame.

The last case gives the OFX side a different Detail Level and MUST differ, so
the comparison is shown able to fail. Exit 0 when every case agrees exactly
and the control differs; 1 otherwise; 2 when a tool is missing.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDENTIFIER = "com.stoatworks.ccu"

# FFGL display name -> OFX script name. The OFX names are permanent (a saved
# project refers to them); see source/ofx/CcuOFX.cpp.
NAMES = {
    "Master Gain": "masterGain", "Master Black": "masterBlack", "White Clip": "whiteClip",
    "R Gain": "rGain", "B Gain": "bGain", "Drift": "drift", "Matrix": "matrix",
    "Saturation": "saturation", "Detail Level": "detailLevel", "Crispening Freq": "crispeningFreq",
    "H/V Ratio": "hvRatio", "Coring": "coring", "Level Dependence": "levelDependence",
    "Skin Detail": "skinDetail", "Skin Hue": "skinHue", "Skin Width": "skinWidth",
    "Knee On": "kneeOn", "Knee Point": "kneePoint", "Knee Slope": "kneeSlope",
    "Gamma": "gamma", "Black Gamma": "blackGamma", "Mix": "mix", "Show Detail": "showDetail",
}

CASES = [
    ("the defaults", {}, None),
    ("everything moved", {"Master Gain": 0.6, "Master Black": 0.6, "White Clip": 0.2, "R Gain": 0.7,
                          "B Gain": 0.3, "Matrix": 3, "Saturation": 0.8, "Detail Level": 0.8,
                          "Crispening Freq": 0.33, "H/V Ratio": 0.7, "Coring": 0.1,
                          "Level Dependence": 0.9, "Knee Point": 0.2, "Knee Slope": 0.1, "Gamma": 0.8,
                          "Black Gamma": 0.9, "Mix": 0.7}, None),
    ("the skin window moved, the detail view", {"Skin Detail": 1, "Skin Hue": 0.5, "Skin Width": 1,
                                                 "Show Detail": 1, "H/V Ratio": 0.2, "Matrix": 2,
                                                 "Crispening Freq": 1}, None),
    ("the knee off at +18 dB, low gamma", {"Knee On": 0, "Master Gain": 1, "Gamma": 0.1,
                                           "Saturation": 1}, None),
    ("high saturation, a wide delay, no coring", {"Matrix": 2, "Saturation": 0.9, "Crispening Freq": 0.9,
                                                  "Coring": 0, "Detail Level": 1,
                                                  "Level Dependence": 0}, None),
    ("CONTROL: Detail Level 0.30 against 0.40", {}, {"Detail Level": 0.4}),
]


def default_ofxprobe():
    """$OFXPROBE, else the sibling checkout, else the canonical one (a worktree's
    parent is not the repo's)."""
    for candidate in (os.environ.get("OFXPROBE", ""),
                      os.path.join(ROOT, "..", "resolume-ofx-bridge", "build", "ofxprobe"),
                      os.path.expanduser("~/Projects/resolume/resolume-ofx-bridge/build/ofxprobe")):
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    return "ofxprobe"


def ramp(width, height):
    """ofxprobe's input, as rows bottom-up."""
    return [b"".join(bytes(((x * 4) & 255, (y * 8) & 255, 128, 255)) for x in range(width))
            for y in range(height)]


def through_ffgl(cctest, width, height, rows, settings):
    args = [cctest, "--pipe", "--size", f"{width}x{height}"]
    for name, value in settings.items():
        args += ["--set", f"{name}={value}"]
    frame = subprocess.run(args, input=b"".join(reversed(rows)), capture_output=True,
                           check=True).stdout
    stride = width * 4
    return [frame[(height - 1 - y) * stride:(height - y) * stride] for y in range(height)]


def through_ofx(ofxprobe, build, width, height, settings, bmp):
    args = [ofxprobe, "--dir", build, "--render", IDENTIFIER, "--size", f"{width}x{height}",
            "--out", bmp]
    for name, value in settings.items():
        args += ["--set", f"{NAMES[name]}={value}"]
    run = subprocess.run(args, capture_output=True, text=True)
    if run.returncode != 0 or "WARNING" in run.stderr:
        raise RuntimeError(f"ofxprobe: {run.stdout}{run.stderr}")
    data = open(bmp, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    bmp_w, bmp_h = struct.unpack_from("<ii", data, 18)
    if bmp_w != 2 * width + 8 or bmp_h != height:
        raise RuntimeError(f"unexpected comparison BMP {bmp_w}x{bmp_h}")
    stride = (bmp_w * 3 + 3) & ~3
    rows = []
    for y in range(height):  # BMP rows run bottom-up, as OFX's do; output is the right half
        base = offset + y * stride + (width + 8) * 3
        row = bytearray()
        for x in range(width):
            b, g, r = data[base + 3 * x:base + 3 * x + 3]
            row += bytes((r, g, b, 255))
        rows.append(bytes(row))
    return rows


def compare(a, b, width):
    worst, pixels = 0, 0
    for ra, rb in zip(a, b):
        for x in range(width):
            d = max(abs(ra[4 * x + c] - rb[4 * x + c]) for c in range(3))
            worst = max(worst, d)
            pixels += d > 0
    return worst, pixels


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "build-universal"))
    ap.add_argument("--cctest", default=None)
    ap.add_argument("--ofxprobe", default=default_ofxprobe())
    ap.add_argument("--size", default="320x180")
    a = ap.parse_args()
    cctest = a.cctest or os.path.join(a.build, "cctest")
    for tool in (cctest, a.ofxprobe):
        if not os.access(tool, os.X_OK):
            print(f"missing: {tool}")
            return 2
    width, height = map(int, a.size.split("x"))
    rows = ramp(width, height)

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, settings, ofx_override in CASES:
            ffgl = through_ffgl(cctest, width, height, rows, settings)
            ofx_settings = dict(settings, **(ofx_override or {}))
            ofx = through_ofx(a.ofxprobe, a.build, width, height, ofx_settings, os.path.join(tmp, "o.bmp"))
            worst, pixels = compare(ffgl, ofx, width)
            moved = compare(rows, ffgl, width)[1]
            control = ofx_override is not None
            ok = (pixels > 0) if control else (pixels == 0 and moved > 0)
            failed += not ok
            print(f"   {'ok' if ok else 'FAIL':4s} {name}: {pixels} of {width * height} pixels differ, "
                  f"worst {worst}/255 (the effect moved {moved})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
