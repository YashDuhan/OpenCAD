#!/bin/bash
# Presses every OpenCAD toolbar button, and uses every menu entry, panel, status bar field, right-click menu and AI
# tool, with its recipe in the real app, offscreen and with throwaway settings; then writes the report and the
# checklist docs/ai/toolbar-buttons-checklist.md.
#
#   scripts/ai/test_toolbar_buttons.sh [build dir] [comma,separated,ids]   (an id ending in * is a prefix: "menu:*")
#
# The recipes are librecad/res/ai/toolbar_recipes.json (built into OpenCAD; the same tests run from the
# Agent Progress window), plus the tests OpenCAD makes for menu entries, panels and the status bar. Takes a while.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build/x64-debug}
ONLY=${2:-}
TMP=$(mktemp -d /tmp/opencad-button-test-XXXXXX)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$ROOT/docs/ai" "$TMP/config/LibreCAD"
# the settings the recipes were written with: no first-run dialog, sub-window mode (a 740 x 533 px drawing view
# offscreen), Qt's own file dialogs (the AI can fill those in)
cat > "$TMP/config/LibreCAD/LibreCAD-2.conf" <<'CONF'
[General]
_schemaMajor=2
_schemaMinor=0

[Defaults]
UseQtFileOpenDialog=1

[Startup]
FirstLoad=0

[WindowOptions]
SubWindowMode=0
CONF
cd "$BUILD" || exit 1
OPENCAD_AI_TOOLBAR_RECIPES="$ROOT/librecad/res/ai/toolbar_recipes.json" \
OPENCAD_AI_TOOLBAR_TEST="$TMP/report.json" \
OPENCAD_AI_TOOLBAR_INVENTORY="$TMP/inventory.json" \
OPENCAD_AI_SURFACES_INVENTORY="${OPENCAD_AI_SURFACES_INVENTORY:-$TMP/surfaces.json}" \
OPENCAD_AI_TOOLBAR_CHECKLIST="$ROOT/docs/ai/toolbar-buttons-checklist.md" \
OPENCAD_AI_TOOLBAR_ONLY="$ONLY" \
XDG_CONFIG_HOME="$TMP/config" XDG_DATA_HOME="$TMP/data" QT_QPA_PLATFORM=offscreen \
  timeout 5400 ./librecad > "$TMP/app.log" 2>&1

if [ ! -s "$TMP/report.json" ]; then
    echo "No report was written. Last lines of the app's output:"
    tail -30 "$TMP/app.log"
    exit 1
fi
python3 - "$TMP/report.json" <<'PY'
import json, sys
from collections import Counter
results = json.load(open(sys.argv[1]))
print(dict(Counter(r["status"] for r in results)))
for r in results:
    if r["status"] == "failed":
        print("FAILED", r["id"], "::", r["detail"].split("\n")[0])
PY
# a full run becomes the baseline the Agent Progress window shows (built into OpenCAD with the recipes)
if [ -z "$ONLY" ]; then
    python3 - "$TMP/report.json" "$ROOT/librecad/res/ai/toolbar_test_report.json" <<'PY'
import json, sys, datetime
results = json.load(open(sys.argv[1]))
slim = [{"id": r["id"], "status": r["status"], "detail": r["detail"].split("\n")[0][:300]} for r in results]
json.dump({"when": datetime.datetime.now().isoformat(timespec="minutes"), "results": slim}, open(sys.argv[2], "w"), indent=1)
PY
    echo "Baseline for the Agent Progress window: $ROOT/librecad/res/ai/toolbar_test_report.json (rebuild to include it)"
fi
echo "Checklist: $ROOT/docs/ai/toolbar-buttons-checklist.md"
