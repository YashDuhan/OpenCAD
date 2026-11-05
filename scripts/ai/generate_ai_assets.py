#!/usr/bin/env python3
"""Regenerate librecad/src/ui/dock_widgets/ai_assistant/lc_aigenerated.cpp from a Cypher (Sorvex) checkout.

It copies, so the AI Providers modal looks and lists models like Cypher's desktop Providers page:
  * provider logos  - react-icons (Simple Icons: CC0-1.0, Remix Icon: Apache-2.0)
  * UI icons        - lucide-react (ISC)
  * model catalog   - packages/ai/src/providers/data/<provider>.json (chat models only)

Usage: scripts/ai/generate_ai_assets.py [path/to/Cypher]   (default: ../Cypher next to this repository)
"""
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CYPHER = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO.parent / "Cypher"
OUT = REPO / "librecad/src/ui/dock_widgets/ai_assistant/lc_aigenerated.cpp"

# provider id in OpenCAD -> react-icons export (same mapping as Cypher's ModelPicker PROVIDER_ICONS)
PROVIDER_ICONS = {
    "anthropic": ("si", "SiAnthropic"),
    "openai": ("ri", "RiOpenaiFill"),
    "google": ("si", "SiGooglegemini"),
    "openrouter": ("si", "SiOpenrouter"),
    "xai": ("si", "SiX"),
    "deepseek": ("si", "SiDeepseek"),
    "mistral": ("si", "SiMistralai"),
    "nvidia": ("si", "SiNvidia"),
    "huggingface": ("si", "SiHuggingface"),
    "minimax": ("si", "SiMinimax"),
    "vercel-ai-gateway": ("si", "SiVercel"),
    "ollama": ("si", "SiOllama"),
}
# lucide icon names used by the Providers page, the composer and the model picker (cpu: Cypher's local-model icon)
LUCIDE_ICONS = ["key-round", "trash-2", "search", "check", "refresh-cw", "cpu", "x", "external-link",
                "chevron-down", "send-horizontal", "square", "star", "clock",
                "plug", "plus", "rotate-ccw", "upload", "folder-open", "pencil",
                "chevron-right", "wrench", "loader", "circle-alert", "maximize-2", "brain",
                "lock", "lock-open", "sparkles", "hourglass", "shield-alert", "shield-check",
                "globe", "arrow-left", "arrow-right", "rotate-cw", "grip-horizontal", "minimize-2",
                "circle-help", "chevron-up"]
CATALOG_PROVIDERS = ["anthropic", "openai", "google", "openrouter", "xai", "deepseek", "mistral", "groq",
                     "cerebras", "together", "fireworks", "nvidia", "huggingface", "moonshotai", "zai", "minimax",
                     "vercel-ai-gateway"]


def svg_from_react_icon(pack: str, name: str) -> str:
    source = (CYPHER / "node_modules/react-icons" / pack / "index.js").read_text()
    match = re.search(r"module\.exports\.%s = function %s \(props\) \{\s*return GenIcon\((\{.*?\})\)\(props\);" % (name, name),
                      source, re.S)
    if not match:
        raise SystemExit(f"icon {name} not found in react-icons/{pack}")
    tree = json.loads(match.group(1))

    def render(node):
        attrs = "".join(f' {k}="{v}"' for k, v in node["attr"].items() if k != "role")
        children = "".join(render(c) for c in node.get("child", []))
        return f"<{node['tag']}{attrs}>{children}</{node['tag']}>"

    tree["attr"].setdefault("fill", "currentColor")  # filled shapes; Remix icons already set it
    tree["attr"]["xmlns"] = "http://www.w3.org/2000/svg"
    return render(tree)


def svg_from_lucide(name: str) -> str:
    source = (CYPHER / "node_modules/lucide-react/dist/esm/icons" / f"{name}.mjs").read_text()
    alias = re.search(r"export \{ default \} from '\./([\w-]+)\.mjs'", source)
    if alias:  # renamed icons re-export another file (e.g. trash-2 -> trash)
        return svg_from_lucide(alias.group(1))
    start = source.find("node:")
    if start < 0:
        raise SystemExit(f"lucide icon {name} not found")
    start = source.index("[", start)
    depth, end = 0, start
    for end in range(start, len(source)):  # the matching bracket of the node list
        depth += {"[": 1, "]": -1}.get(source[end], 0)
        if depth == 0:
            break
    literal = re.sub(r"(\b[a-zA-Z_][\w-]*):\s", r'"\1": ', source[start:end + 1])  # quote object keys
    literal = re.sub(r",(\s*[\]}])", r"\1", literal)                       # drop trailing commas
    nodes = json.loads(literal)
    body = ""
    for tag, attrs in nodes:
        body += f"<{tag}" + "".join(f' {k}="{v}"' for k, v in attrs.items() if k != "key") + "/>"
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
            'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' + body + "</svg>")


def cpp_string(text: str) -> str:
    return json.dumps(text)  # a JSON string is a valid C++ string literal for this ASCII content


def main():
    icons = {pid: svg_from_react_icon(*ref) for pid, ref in PROVIDER_ICONS.items()}
    icons.update({f"lucide:{name}": svg_from_lucide(name) for name in LUCIDE_ICONS})

    models = []
    for provider in CATALOG_PROVIDERS:
        data = json.loads((CYPHER / "packages/ai/src/providers/data" / f"{provider}.json").read_text())
        for api in data.values():
            for model in api.values():
                if model.get("type") != "chat":
                    continue
                models.append((provider, model["id"], model.get("name") or model["id"],
                               int(model.get("contextWindow") or 0), int(model.get("maxTokens") or 0),
                               bool(model.get("reasoning")), "image" in (model.get("input") or [])))

    lines = [
        "// This file is generated by scripts/ai/generate_ai_assets.py from the Cypher (Sorvex) repository.",
        "// Do not edit by hand: rerun the script to update the icons or the model catalog.",
        "// Icons: Simple Icons (CC0-1.0), Remix Icon (Apache-2.0), Lucide (ISC). Model data: Cypher model catalog.",
        "",
        '#include "lc_aigenerated.h"',
        "",
        "namespace LC_AIGenerated {",
        "",
        "const IconEntry ICONS[] = {",
    ]
    lines += [f"    {{{cpp_string(name)}, {cpp_string(svg)}}}," for name, svg in icons.items()]
    lines += ["};", "const int ICON_COUNT = int(sizeof(ICONS) / sizeof(ICONS[0]));", "", "const ModelEntry MODELS[] = {"]
    for provider, mid, name, ctx, max_tokens, reasoning, images in models:
        lines.append(f"    {{{cpp_string(provider)}, {cpp_string(mid)}, {cpp_string(name)}, {ctx}, {max_tokens}, "
                     f"{'true' if reasoning else 'false'}, {'true' if images else 'false'}}},")
    lines += ["};", "const int MODEL_COUNT = int(sizeof(MODELS) / sizeof(MODELS[0]));", "", "}", ""]
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT.relative_to(REPO)}: {len(icons)} icons, {len(models)} models")


if __name__ == "__main__":
    main()
