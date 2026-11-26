"""The Home Assistant app's Configuration tab: its options in
ha-app/config.yaml, their names in translations/en.yaml, and what the start
script, ha-app/run.sh, passes the daemon for each.

Home Assistant writes the tab to /data/options.json. run.sh runs here with
its paths moved into a temporary folder: /data, and /usr/bin/tigomonitord,
which a stand-in replaces that writes down its arguments. Without a
Supervisor token it skips the MQTT offer, so only the options are tested.
Needs sh and jq, as the app's image has. Plain text, no YAML library: the
dev container has none.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
script = (root / "ha-app/run.sh").read_text()
config = (root / "ha-app/config.yaml").read_text()
translations_path = root / "ha-app/translations/en.yaml"
fail = 0


def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ("" if ok else f", want {want!r}"))
    fail |= not ok


def block(text, key, indent=""):
    """The lines indented under `key:` (at indent), as {name: value}."""
    m = re.search(rf"^{indent}{key}:\s*\n((?:{indent}  .*\n?)+)", text, re.M)
    if not m:
        return {}
    return dict(re.findall(rf"^{indent}  (\w+):\s*(.*?)\s*$", m.group(1), re.M))


OPTIONS = ["dangerous_options", "debug_logging"]
expect("the Configuration tab's options", sorted(block(config, "options")), OPTIONS)
expect("each off until it's turned on", block(config, "options"), {k: "false" for k in OPTIONS})
expect("each a checkbox", block(config, "schema"), {k: "bool" for k in OPTIONS})
translations = translations_path.read_text() if translations_path.is_file() else ""
for k in OPTIONS:
    named = block(translations, k, "  ")
    expect(f"{k} has a name and a description in en.yaml", sorted(named), ["description", "name"])
    expect(f"and run.sh reads it", f"option {k}" in script, True)

# The daemon's --help, as a daemon that has the flag prints it, and as the
# 2.5.0 one, which hasn't: a rebuilt app can run a new run.sh on that.
NEW_HELP = "  --trust-ingress ADDR\n  --dangerous-options on|off\n  --verbose\n"
OLD_HELP = "  --trust-ingress ADDR\n  --verbose\n"


def run(options, help_text=NEW_HELP):
    """run.sh's arguments to the daemon, and whether settings.json exists after."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        data = tmp / "data"
        data.mkdir()
        if options is not None:
            (data / "options.json").write_text(json.dumps(options))
        (tmp / "help.txt").write_text(help_text)
        daemon = tmp / "tigomonitord"
        daemon.write_text(
            "#!/bin/sh\n"
            f'if [ "$1" = --help ]; then cat {tmp}/help.txt >&2; exit 0; fi\n'
            f'printf "%s\\n" "$@" > {tmp}/args\n'
        )
        daemon.chmod(0o755)
        moved = script.replace("state=/data", f"state={data}").replace("/usr/bin/tigomonitord", str(daemon))
        if "state=/data" not in script or "/usr/bin/tigomonitord" not in script:
            print("  FAIL run.sh no longer sets state=/data and runs /usr/bin/tigomonitord; update this test")
            sys.exit(1)
        (tmp / "run.sh").write_text(moved)
        env = {k: v for k, v in os.environ.items() if k != "SUPERVISOR_TOKEN"}
        r = subprocess.run(["sh", str(tmp / "run.sh")], env=env, capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            print(f"  FAIL run.sh exited {r.returncode}: {r.stderr.strip()}")
            sys.exit(1)
        args = (tmp / "args").read_text().splitlines() if (tmp / "args").exists() else None
        return args, (data / "settings.json").exists()


def flag(args, name):
    """The value after name, True for a bare flag, None if it's not there."""
    if args is None or name not in args:
        return None
    i = args.index(name)
    return args[i + 1] if i + 1 < len(args) and not args[i + 1].startswith("--") else True


args, settings = run({"dangerous_options": True, "debug_logging": True})
expect("the daemon is started on /data and the ingress port", (flag(args, "--port"), args is not None and "--state-dir" in args), ("8080", True))
expect("Home Assistant's ingress proxy is trusted", flag(args, "--trust-ingress"), "172.30.32.2")
expect("dangerous options on", flag(args, "--dangerous-options"), "on")
expect("debug logging on", flag(args, "--verbose"), True)
# The daemon counts a start without settings.json as a new install, so the
# options mustn't be written there.
expect("no settings.json written", settings, False)

args, settings = run({"dangerous_options": False, "debug_logging": False})
expect("dangerous options off", flag(args, "--dangerous-options"), "off")
expect("no debug logging", flag(args, "--verbose"), None)

args, settings = run(None)
expect("without options.json: settings.json decides", flag(args, "--dangerous-options"), None)
expect("and no debug logging", flag(args, "--verbose"), None)

# A daemon that doesn't know the flag would refuse to start on it.
args, settings = run({"dangerous_options": True, "debug_logging": True}, OLD_HELP)
expect("a daemon without the flag isn't given it", flag(args, "--dangerous-options"), None)
expect("but still gets debug logging", flag(args, "--verbose"), True)

if fail:
    sys.exit(1)
print("ha_app_options_test: all passed")
