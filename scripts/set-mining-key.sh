#!/usr/bin/env bash
# Set (and verify) the Duino-Coin mining key for the account in Settings.cfg.
#
#   ./set-mining-key.sh                 # ask for the key, check it, store it
#   ./set-mining-key.sh "mykey"         # set it non-interactively
#   ./set-mining-key.sh --clear         # remove it (mining_key = None)
#
# The key is the one you set on your Duino-Coin account page (duinocoin.com ->
# your profile -> "Mining key"). It is stored base64-encoded in Settings.cfg,
# exactly the way the official AVR Miner stores it.
set -u

# The settings file is the official Duino-Coin AVR Miner Settings.cfg. Point at
# it with DUCO_SETTINGS, or put a Settings.cfg next to this script.
CONFIG="${DUCO_SETTINGS:-./Settings.cfg}"

if [ ! -f "$CONFIG" ]; then
    echo "Settings.cfg not found at: $CONFIG" >&2
    exit 1
fi

KEY="${1:-}"
if [ "$KEY" = "--clear" ]; then
    KEY=""
elif [ -z "$KEY" ]; then
    if [ -t 0 ]; then
        read -r -p "Mining key (leave empty if your account has none): " KEY
    else
        echo "No key given and this is not an interactive terminal." >&2
        echo "Run: ./set-mining-key.sh \"<your key>\"" >&2
        exit 1
    fi
fi

python3 - "$CONFIG" "$KEY" <<'PYEOF'
import base64
import configparser
import sys
import urllib.parse
import urllib.request

def api(path, params):
    url = "https://server.duinocoin.com" + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    # The account service returns 403 for a request with no user agent.
    req = urllib.request.Request(url, headers={"User-Agent": "duco-miner key check"})
    return json_load(req)

def json_load(req):
    import json
    return json.load(urllib.request.urlopen(req, timeout=15))

config_path, key = sys.argv[1], sys.argv[2]

config = configparser.ConfigParser()
config.read(config_path, encoding="utf-8")
section = config.setdefault("AVR Miner", {})
user = (section.get("username") or "").strip()
if not user:
    print("Settings.cfg has no username - can't check the key", file=sys.stderr)
    sys.exit(1)

if key:
    try:
        r = api("/mining_key", {"u": user, "k": key})
    except Exception as e:
        print(f"Could not reach the account service ({e}). Storing the key anyway.",
              file=sys.stderr)
        r = {}
    if r.get("success"):
        section["mining_key"] = base64.b64encode(key.encode("utf-8")).decode("ascii")
        print(f"Key accepted by the pool for {user}; saved to {config_path}")
    elif r.get("message") == "Too many requests":
        section["mining_key"] = base64.b64encode(key.encode("utf-8")).decode("ascii")
        print(f"Could not verify right now (rate limited); saved anyway to {config_path}")
    else:
        print(f"REJECTED: the pool says that key is not valid for {user}.")
        print("Check the key on your Duino-Coin account page and try again.")
        sys.exit(1)
else:
    try:
        r = api("/mining_key", {"u": user})
    except Exception:
        r = {}
    section["mining_key"] = "None"
    print(f"Mining key cleared for {user} (the account service says the account "
          f"has a key: {r.get('has_key', 'unknown')}).")

with open(config_path, "w", encoding="utf-8") as f:
    config.write(f)
PYEOF
