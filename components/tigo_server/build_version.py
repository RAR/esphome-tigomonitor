"""TigoMonitor's version for the ESP32 firmware, worked out at compile time.

The Pi's daemon is built from a full checkout, so CI takes `git describe`
(2.3.0, or 2.3.0+1.g8632d50 between releases). ESPHome fetches a git
external component as a shallow clone with no tags, so describe fails there.
Then the version is the `ref:` the config pinned (v2.3.0), or the ref and
the commit it fetched (main · 8632d50). A local `path:` checkout describes
like the Pi. Nothing is bumped at release time: the tag is the version.
"""
import re
import subprocess
from pathlib import Path

# What the version may carry into the JSON and the page.
_SAFE = re.compile(r"[^A-Za-z0-9._+/-]")
_DESCRIBE = re.compile(r"v?(?P<tag>.+?)(?:-(?P<n>\d+)-g(?P<sha>[0-9a-f]+))?(?P<dirty>-dirty)?")
_RELEASE = re.compile(r"v?\d+\.\d+\.\d+\S*")


def version_from(describe, ref, sha):
    """describe: `git describe --tags --dirty` (None if it failed); ref: the
    external_components ref (None for a local path); sha: the short commit."""
    if describe:
        m = _DESCRIBE.fullmatch(describe.strip())
        v = m.group("tag")
        if m.group("n"):
            v += f"+{m.group('n')}.g{m.group('sha')}"
        if m.group("dirty"):
            v += "+local"
        return _SAFE.sub("", v)
    ref = _SAFE.sub("", ref or "")
    sha = _SAFE.sub("", sha or "")
    if ref and _RELEASE.fullmatch(ref):
        return ref[1:] if ref.startswith("v") else ref
    if ref and sha:
        return f"{ref} · {sha}"
    return ref or sha


def _git(root, *args):
    try:
        r = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None


def _our_ref(config):
    """The ref of the external_components entry that fetched this repo."""
    for entry in config.get("external_components", []) or []:
        source = entry.get("source") or {}
        if source.get("type") == "git" and "tigomonitor" in str(source.get("url", "")):
            return source.get("ref")
    return None


def version(config):
    """The version string for this build, or "" if it can't be told."""
    root = Path(__file__).resolve().parents[2]  # <repo>/components/tigo_server/
    top = _git(root, "rev-parse", "--show-toplevel")
    # Only this repo's own git: not a config repo the files were copied into.
    if top is None or Path(top).resolve() != root:
        return version_from(None, _our_ref(config), None)
    return version_from(_git(root, "describe", "--tags", "--dirty"), _our_ref(config),
                        _git(root, "rev-parse", "--short", "HEAD"))
