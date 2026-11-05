#!/bin/bash
# Runs one end-to-end scenario of the AI chat in the real OpenCAD, offscreen, with throwaway settings.
#
#   scripts/ai/e2e/run.sh SCENARIO_DIR [OUT_DIR]
#
# SCENARIO_DIR holds steps.json (the driver's steps, see lc_aie2etest.cpp) and model.json (the scripted model's replies,
# see fake_model.py). In steps.json, ${OUT} is the output folder and ${FIXTURES} the dummy files (make_fixtures.py).
# Results: OUT_DIR/results.json, app.log, the model's request log requests.jsonl, and the screenshots the steps take.
# Env: BIN (build folder), MODE (approval mode: supervised, edits, auto, full; default auto), TIMEOUT (seconds),
# KEEP_HOME (a folder whose settings and data survive between runs), REAL=gemini (use the Gemini key stored in your own
# OpenCAD settings instead of the fake model; costs money; the key is copied into the throwaway settings, never printed).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SCEN=$(cd "$1" && pwd)
OUT=${2:-$SCEN/out}
BIN=${BIN:-$ROOT/build/x64-debug}
MODE=${MODE:-auto}
FIXTURES=${FIXTURES:-$HERE/fixtures}
mkdir -p "$OUT"
[ -f "$FIXTURES/spec-sheet.pdf" ] || python3 "$HERE/make_fixtures.py" "$FIXTURES" > /dev/null
rm -f "$OUT/requests.jsonl" "$OUT/results.json"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
if [ -n "${KEEP_HOME:-}" ]; then T=$KEEP_HOME; else T=$(mktemp -d /tmp/opencad-e2e-XXXXXX); fi
CONF=$T/config/LibreCAD
mkdir -p "$CONF/LibreCAD-2"
[ -f "$CONF/LibreCAD-2.conf" ] || printf '[General]\n_schemaMajor=2\n_schemaMinor=0\n\n[Defaults]\nUseQtFileOpenDialog=1\n\n[Startup]\nFirstLoad=0\n\n[WindowOptions]\nSubWindowMode=0\n' > "$CONF/LibreCAD-2.conf"
FAKE=
if [ "${REAL:-}" = "gemini" ]; then
    python3 - "$CONF/LibreCAD-2/ai-auth.json" "$MODE" <<'PY'
import json, os, sys
src = json.load(open(os.path.expanduser("~/.config/LibreCAD/LibreCAD-2/ai-auth.json")))
g = src["providers"]["google"]
out = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}
out.setdefault("providers", {})["google"] = {"key": g["key"], "model": g.get("model", "")}
out["selectedProvider"] = "google"
out["approvalMode"] = sys.argv[2]
json.dump(out, open(sys.argv[1], "w"))
PY
else
    python3 "$HERE/fake_model.py" "$PORT" "$SCEN/model.json" "$OUT/requests.jsonl" & FAKE=$!
    python3 - "$CONF/LibreCAD-2/ai-auth.json" "$MODE" "http://127.0.0.1:$PORT/v1" <<'PY'
import json, os, sys
out = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}
out.setdefault("providers", {})["custom"] = {"key": "test-key", "model": "m", "baseUrl": sys.argv[3]}
out["selectedProvider"] = "custom"
out["approvalMode"] = sys.argv[2]
json.dump(out, open(sys.argv[1], "w"))
PY
    sleep 0.4
fi
sed -e "s#\${OUT}#$OUT#g" -e "s#\${FIXTURES}#$FIXTURES#g" "$SCEN/steps.json" > "$OUT/steps.run.json"
cd "$BIN" && OPENCAD_AI_E2E="$OUT/steps.run.json" OPENCAD_AI_E2E_OUT="$OUT/results.json" XDG_CONFIG_HOME="$T/config" \
    XDG_DATA_HOME="$T/data" QT_QPA_PLATFORM=offscreen QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --disable-gpu-compositing" \
    timeout "${TIMEOUT:-300}" ./librecad > "$OUT/app.log" 2>&1
CODE=$?
[ -n "$FAKE" ] && kill "$FAKE" 2>/dev/null
[ -z "${KEEP_HOME:-}" ] && rm -rf "$T"
python3 - "$OUT/results.json" "$CODE" <<'PY'
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except Exception as e:
    print("FAILED: no results (app exit %s): %s" % (sys.argv[2], e)); sys.exit(1)
bad = [x for x in r if not x["ok"]]
print(("passed" if not bad else "FAILED") + ": %d steps, %d failed" % (len(r), len(bad)))
for x in bad:
    print("  step", x["step"], x["do"], "::", x["detail"].split("\n")[0][:300])
sys.exit(1 if bad else 0)
PY
