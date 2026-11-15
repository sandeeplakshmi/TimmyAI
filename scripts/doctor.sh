#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
#
# Checks a Timmy install and prints a summary, or JSON with --json (for AI agents).
#   ./install.sh doctor [--json]
# Exit: 0 everything required is OK, 1 something required needs fixing (each item says how).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
import json, os, shutil, subprocess, sys, time, urllib.request
from pathlib import Path

root = Path(sys.argv[1]); as_json = "--json" in sys.argv[2:]
home = Path(os.environ.get("TIMMY_HOME", Path.home() / ".timmy"))
hermes_home = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
port = int(os.environ.get("TIMMY_PORT", "8000"))
gport = int(os.environ.get("TIMMY_GADGET_PORT", "8790"))
checks = []


def add(name, ok, detail, fix="", required=True):
    checks.append({"name": name, "ok": bool(ok), "required": required, "detail": detail, "fix": "" if ok else fix})


def sh(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        return ""


def http(url):
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            return r.status, r.read(4096)
    except Exception as exc:
        return None, str(exc).encode()


def env_keys(path):
    out = {}
    try:
        for line in path.read_text().splitlines():
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = bool(v.strip())
    except OSError:
        pass
    return out

add("macOS", sys.platform == "darwin", sh("sw_vers", "-productVersion") or sys.platform, "Timmy's installer targets macOS")
add("Homebrew", shutil.which("brew"), sh("brew", "--version").split("\n")[0] or "missing",
    'install it: /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"')
add("Python 3.10+", sys.version_info >= (3, 10), sys.version.split()[0], "brew install python@3.12")
hermes = shutil.which("hermes") or (str(Path.home() / ".local/bin/hermes") if (Path.home() / ".local/bin/hermes").exists() else None)
add("Hermes Agent", hermes, sh(hermes, "--version").split("\n")[0] if hermes else "missing", "./install.sh --yes (step 2)")
add("Timmy folder", home.is_dir(), str(home), "./install.sh --yes (step 3)")
if home.is_dir():
    mode = oct(home.stat().st_mode & 0o777)
    add("Timmy folder is private", (home.stat().st_mode & 0o077) == 0, f"mode {mode}", f"chmod 700 {home}", required=False)
    add("Denylist for commit checks", (home / "denylist.txt").exists(), str(home / "denylist.txt"),
        "add your private strings, one per line (see templates/timmy-home/denylist.txt)", required=False)
h = env_keys(hermes_home / ".env")
add("Brain configured", h.get("ANTHROPIC_API_KEY") or env_keys(home / ".env").get("TIMMY_LOCAL_MODEL"),
    "Claude key set" if h.get("ANTHROPIC_API_KEY") else "no Claude key / local model yet",
    f"the human opens http://localhost:{port}/setup.html and enters it (agents never handle the key)")
add("Hermes API server enabled", h.get("API_SERVER_ENABLED") and h.get("API_SERVER_KEY"), "API_SERVER_ENABLED and API_SERVER_KEY",
    f"save once on http://localhost:{port}/setup.html, then: hermes gateway restart")
code, _ = http("http://127.0.0.1:8642/health")
add("Hermes gateway running", code == 200, "http://127.0.0.1:8642/health" + (" OK" if code == 200 else " not answering"),
    "hermes gateway start (or ./install.sh --yes for the login service)")
code, body = http(f"http://localhost:{port}/setup/status")
add("Timmy web page", code == 200, f"http://localhost:{port}" + (" OK" if code == 200 else " not answering"),
    "./start-timmy.sh, or ./install.sh --yes for the login service; logs: ~/.hermes/logs/timmy-web.out")
code, body = http(f"http://localhost:{port}/voice/status")
try:
    vs = json.loads(body) if code == 200 else {}
except ValueError:
    vs = {}
add("Voice (speech in and out)", vs.get("ok"), f"stt={vs.get('stt')} tts={vs.get('tts')} voice={vs.get('voice')}" if vs else "unknown (web page down)",
    "./install.sh --yes (step 4); check Hermes' voice settings", required=False)
hooks = root / ".git/hooks/pre-commit"
add("Commit checks installed", hooks.exists(), str(hooks), "scripts/install-hooks.sh", required=False)
add("gitleaks", shutil.which("gitleaks"), shutil.which("gitleaks") or "missing", "brew install gitleaks", required=False)
idf = next((p for p in [os.environ.get("IDF_PATH", ""), str(Path.home() / "esp/esp-idf-v6.0.3"), str(Path.home() / "esp/esp-idf-v6")]
            if p and Path(p, "export.sh").exists()), None)
add("ESP-IDF (firmware builds only)", idf, idf or "not installed", "./install.sh --with-firmware-tools", required=False)
# Board: USB, then its last connection to the server.
usb = sorted(str(p) for p in Path("/dev").glob("cu.usbmodem*"))
add("Board on USB", usb, ", ".join(usb) or "not plugged in", "plug it in with a USB-C data cable (only needed to flash)", required=False)
keys = home / "keys" / "gadget-keys.json"
add("Gadget device key", keys.exists(), str(keys), "python3 server/gadget_server.py --new-key timmy-desk", required=False)
try:
    st = json.loads((home / "gadget-status.json").read_text())
except (OSError, ValueError):
    st = {}
if st:
    dev, info = max(st.items(), key=lambda kv: kv[1].get("epoch", 0))
    age = int(time.time()) - int(info.get("epoch", 0))
    add("Board reached the server", age < 86400, f"{dev}: last connected {info.get('last_connected')} "
        f"(firmware {info.get('firmware') or 'unknown'}, from {info.get('peer')})",
        "same 2.4 GHz Wi-Fi as the Mac; >timmy over USB serial shows its settings; allow Python in the macOS firewall", required=False)
else:
    add("Board reached the server", False, "never", "flash it (./install.sh flash) and set it up (firmware/README.md)", required=False)
listening = sh("lsof", "-nP", f"-iTCP:{gport}", "-sTCP:LISTEN")
add(f"Gadget endpoint (port {gport})", listening, "listening" if listening else "off (it starts once a device key exists)",
    "create a device key, then restart the web page", required=False)
addons = sorted(p.parent.name for p in (home / "addons").glob("*/addon.py")) if home.is_dir() else []
add("Private add-ons", True, ", ".join(addons) or "none", required=False)

ok = all(c["ok"] for c in checks if c["required"])
if as_json:
    print(json.dumps({"ok": ok, "checks": checks}, indent=1))
else:
    for c in checks:
        mark = "✓" if c["ok"] else ("✗" if c["required"] else "!")
        print(f" {mark} {c['name']}: {c['detail']}")
        if c["fix"]:
            print(f"     fix: {c['fix']}")
    print("\nAll required checks pass." if ok else "\nSome required checks need fixing (✗ above).")
sys.exit(0 if ok else 1)
PY
