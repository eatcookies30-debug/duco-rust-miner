#!/usr/bin/env bash
# Show the DUCO balance, trust score, registered miners, and whether the mining
# key in Settings.cfg is the one the account expects.
#   ./balance.sh                       # the account in Settings.cfg
#   ./balance.sh some_other_username
set -u
CONFIG="${DUCO_SETTINGS:-./Settings.cfg}"
USER="${1:-}"
if [ -z "$USER" ]; then
    USER=$(grep -o 'username *= *[^ ]*' "$CONFIG" | head -1 | cut -d' ' -f3)
fi
[ -z "$USER" ] && { echo "no username given and none found in Settings.cfg" >&2; exit 1; }

curl -s --max-time 20 "https://server.duinocoin.com/users/$USER" | python3 -c '
import base64, configparser, json, sys, urllib.parse, urllib.request

user, config_path = sys.argv[1], sys.argv[2]
d = json.load(sys.stdin)
b = d["result"]["balance"]
miners = d["result"].get("miners", [])
print("user:        " + b["username"])
print("balance:     %.6f DUCO" % b["balance"])
print("max miners:  %d (currently %d registered)" % (b["max_miners"], len(miners)))
print("trust score: %s" % b.get("trust_score", "?"))
for m in miners:
    print("  miner %s: %s H/s" % (m.get("minerid", "?"), m.get("hashrate", "?")))

raw = ""
try:
    cfg = configparser.ConfigParser()
    cfg.read(config_path, encoding="utf-8")
    raw = (cfg.get("AVR Miner", "mining_key", fallback="") or "").strip()
except Exception:
    pass

if not raw or raw == "None":
    print("mining key:  nothing to check - %s has no mining_key" % config_path)
else:
    padded = raw + "=" * (-len(raw) % 4)
    try:
        decoded = base64.b64decode(padded).decode("utf-8")
        key = decoded if decoded.isprintable() else raw
    except Exception:
        key = raw

    params = {"u": user, "k": key}
    try:
        req = urllib.request.Request(
            "https://server.duinocoin.com/mining_key?" + urllib.parse.urlencode(params),
            headers={"User-Agent": "Duino-Coin AVR Miner (Rust) 1.0"})
        r = json.load(urllib.request.urlopen(req, timeout=15))
    except Exception as e:
        print("mining key:  could not be checked (%s)" % e)
    else:
        if not r.get("has_key"):
            print("mining key:  none set on this account, so any key is accepted")
        elif r.get("success"):
            print("mining key:  valid - the key in Settings.cfg is accepted")
        else:
            print("mining key:  set on the account but the one in Settings.cfg is not it"
                  " - run ./set-mining-key.sh")
' "$USER" "$CONFIG"
