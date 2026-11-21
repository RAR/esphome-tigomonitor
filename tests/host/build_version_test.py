"""The ESP32's TigoMonitor version, as components/tigo_server/build_version.py
works it out at compile time. Run: python3 -I tests/host/build_version_test.py"""
import importlib.util
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_version", root / "components/tigo_server/build_version.py")
bv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bv)

cases = [
    # A full checkout: git describe, written as the Pi's CI writes it.
    (("v2.3.0", None, None), "2.3.0"),
    (("v2.3.0-1-g8632d50", None, None), "2.3.0+1.g8632d50"),
    (("v2.3.0-1-g8632d50-dirty", None, None), "2.3.0+1.g8632d50+local"),
    (("v2.0.0-rc.1-3-gabc1234", None, None), "2.0.0-rc.1+3.gabc1234"),
    # ESPHome's shallow clone has no tags: the external_components ref.
    ((None, "v2.3.0", "8632d50"), "2.3.0"),
    ((None, "2.3.0", "8632d50"), "2.3.0"),
    ((None, "main", "8632d50"), "main · 8632d50"),
    ((None, "feature/x", "8632d50"), "feature/x · 8632d50"),
    # A ref with characters the JSON and the page shouldn't carry.
    ((None, 'ma"in<b>', "8632d50"), "mainb · 8632d50"),
    ((None, None, "8632d50"), "8632d50"),
    ((None, None, None), ""),
]

failed = 0
for args, want in cases:
    got = bv.version_from(*args)
    if got != want:
        failed += 1
        print(f"FAIL version_from{args!r}: got {got!r}, want {want!r}")
if failed:
    sys.exit(1)
print(f"build_version_test: all passed ({len(cases)})")
