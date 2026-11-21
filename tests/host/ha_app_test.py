"""The Home Assistant app's version and the images it's built on agree.

ha-app/config.yaml's version is what Home Assistant offers as an update;
ha-app/build.yaml names the released images the app is built from. A release
that moves one and not the other offers an update that installs the old
monitor, or a new monitor no one is told about. Plain text, no YAML library:
the dev container has none.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
config = (root / "ha-app/config.yaml").read_text()
build = (root / "ha-app/build.yaml").read_text()
fail = 0


def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ("" if ok else f", want {want!r}"))
    fail |= not ok


version = re.search(r'^version:\s*"?([^"\s]+)"?\s*$', config, re.M).group(1)
arches = re.findall(r"^\s+-\s+(\w+)\s*$", config.split("arch:", 1)[1].split("\n\n", 1)[0] if "arch:" in config else "", re.M)
images = dict(re.findall(r"^\s+(\w+):\s*(\S+)\s*$", build.split("build_from:", 1)[1], re.M))
docker_arch = {"aarch64": "arm64", "amd64": "amd64"}

expect("a release version", bool(re.fullmatch(r"\d+\.\d+\.\d+", version)), True)
expect("an image for each architecture", sorted(images), sorted(arches))
for arch in arches:
    expect(f"{arch} is built on the release", images.get(arch), f"ghcr.io/rar/tigomonitord:{version}-{docker_arch[arch]}")
expect("ingress on the port the daemon is given", bool(re.search(r"^ingress_port:\s*8080\s*$", config, re.M)), True)
expect("run.sh starts it on that port", "--port 8080" in (root / "ha-app/run.sh").read_text(), True)

if fail:
    sys.exit(1)
print(f"ha_app_test: all passed ({version}, {', '.join(arches)})")
