#!/usr/bin/env python3
"""Compare Editroom LX with the Mac source, for a sync (IMPLEMENTATION.md §15).

    scripts/parity-diff.py [--mac ../editroom] [--lx .] [--bin build/debug/src/app/editroom-lx]
                           [--out report.md] [--update-strings]

Checks:
  schema     Codable stored properties of the Swift model structs vs `editroom-lx --dump-schema`.
  shortcuts  ShortcutAction titles, groups, menu flags and default combos vs `editroom-lx --dump-shortcuts`.
  strings    user-visible string literals in the Swift UI/App/Magic/MCP sources vs resources/strings.json.
  menus      `editroom --dump-menu` output (tests/parity/mac-menu.txt, captured on a Mac) vs `editroom-lx --dump-menu`.
  mcp        MCP tool names (MCPTools.swift `tool("name", …)`) vs `editroom-lx --dump-mcp-tools`.

Exit status 1 when schema or shortcuts differ (those are ported); the others report what is still to port.
"""
import argparse
import json
import os
import re
import subprocess
import sys

MODEL_TYPES = [
    "SavedDocument", "Project", "Track", "Clip", "RGBA", "TextStyle", "ColorGrade", "WheelPoint", "ClipTransform",
    "ZoomMove", "Transition", "ReverseSource", "SpeedRamp", "MediaAsset", "Marker", "Entry", "CaptionTrack",
    "CaptionStyle", "CaptionFix", "MediaFolder", "Transcript", "Word",
]
# Structs named the same as a model type in another file, which aren't the one in the .room file.
SCHEMA_FILES = ["Model/Project.swift", "Model/ColorGrade.swift", "Model/SpeedRamp.swift", "Model/ZoomMove.swift",
                "Model/Captions.swift", "Model/MediaFolders.swift", "Model/EditorModel.swift",
                "Magic/MagicHistory.swift", "Engine/Transcripts.swift"]


def run(binary, flag):
    try:
        out = subprocess.run([binary, flag], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    return out.stdout, out.stderr


# MARK: Swift parsing

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def struct_bodies(src):
    """Yields (name, body) for each `struct Name ... {` with its brace-matched body."""
    for m in re.finditer(r"\bstruct\s+(\w+)[^{]*\{", src):
        i, depth = m.end(), 1
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), src[m.end():i - 1]


def stored_properties(body):
    """`var`/`let` declarations at the top level of a struct body that store a value (not computed, not static)."""
    props, depth, line = [], 0, ""
    for ch in body:
        if ch == "{":
            if depth == 0:
                line += "{"
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            continue
        if depth == 0:
            if ch == "\n":
                props += parse_decl(line)
                line = ""
            else:
                line += ch
    props += parse_decl(line)
    return props


def parse_decl(line):
    s = line.strip()
    s = re.sub(r"^(@\w+(\([^)]*\))?\s+)*", "", s)
    s = re.sub(r"^(private|fileprivate|internal|public)(\(set\))?\s+", "", s)
    if s.startswith(("static ", "class ", "lazy ")):
        return []
    m = re.match(r"(var|let)\s+(.*)$", s)
    if not m:
        return []
    rest = m.group(2)
    if rest.rstrip().endswith("{"):
        return []  # computed property
    if m.group(1) == "let" and "=" in rest:
        return []  # a constant with a value is never decoded (Codable skips it with a warning)
    names = []
    # Split "r: Double, g: Double" but not commas inside brackets or parentheses.
    depth, part, parts = 0, "", []
    for ch in rest:
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(part)
            part = ""
        else:
            part += ch
    parts.append(part)
    for p in parts:
        n = re.match(r"\s*(\w+)\s*[:=]", p)
        if n:
            names.append(n.group(1))
    return names


def mac_schema(mac):
    root = os.path.join(mac, "Editroom")
    out = {}
    for rel in SCHEMA_FILES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        src = strip_comments(open(path, encoding="utf-8").read())
        for name, body in struct_bodies(src):
            if name in MODEL_TYPES and name not in out:
                # Only Codable structs (the header sits just before the body).
                header = src[:src.find(body)][-300:]
                if re.search(r"struct\s+%s\s*:[^{]*Codable" % name, header):
                    out[name] = sorted(stored_properties(body))
    return out


def mac_shortcuts(mac):
    src = open(os.path.join(mac, "Editroom/App/Shortcuts.swift"), encoding="utf-8").read()
    enum = re.search(r"enum ShortcutAction[^{]*\{(.*?)\n    var id", src, re.S).group(1)
    raws = re.findall(r"\b(\w+)\b", " ".join(re.findall(r"case ([^\n]+)", enum)))
    titles = dict(re.findall(r"case \.(\w+): \"([^\"]*)\"", src.split("var title")[1].split("enum Group")[0]))
    groups_src = src.split("var group: Group")[1].split("var inMenu")[0]
    groups = {}
    group_enum = src.split("enum Group")[1].split("}")[0]
    names = dict(re.findall(r"(\w+) = \"(\w+)\"", group_enum))
    for line in re.findall(r"case ([^\n]+): \.(\w+)", groups_src):
        for a in re.findall(r"\.(\w+)", line[0]):
            groups[a] = names.get(line[1], line[1])
    not_menu = set(re.findall(r"\.(\w+)", src.split("var inMenu")[1].split(": false")[0]))
    defaults_src = src.split("var defaults: [KeyCombo]")[1].split("/// The shortcuts in use")[0]
    defaults = {}
    for a, body in re.findall(r"case \.(\w+): \[(.*)\]", defaults_src):
        defaults[a] = [mac_combo(x) for x in split_combos(body)]
    rows = {}
    for a in raws:
        rows[a] = "|".join([a, groups.get(a, "?"), "-" if a in not_menu else "menu", titles.get(a, "?"), " ".join(defaults.get(a, []))])
    return rows


def split_combos(body):
    out, depth, cur = [], 0, ""
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


NAMED = {"space": "Space", "delete": "⌫", "forwardDelete": "⌦", "left": "←", "right": "→", "up": "↑", "down": "↓",
         "home": "Home", "end": "End", "return": "↩", "tab": "⇥", "pageUp": "Page Up", "pageDown": "Page Down"}


def mac_combo(expr):
    m = re.match(r'(\.cmd|KeyCombo)\("((?:\\\\|[^"])*)"(.*)\)$', expr)
    if not m:
        return "?" + expr
    key = m.group(2).replace("\\\\", "\\")
    opts = m.group(3)
    cmd = m.group(1) == ".cmd" or "command: true" in opts
    s = ""
    if "control: true" in opts:
        s += "⌃"
    if "option: true" in opts:
        s += "⌥"
    if "shift: true" in opts:
        s += "⇧"
    if cmd:
        s += "⌘"
    return s + NAMED.get(key, key.upper())


STRING_CALLS = r'(?:Text|Button|Toggle|Label|Picker|Section|Menu|Link|LabeledContent|TextField|SecureField|Stepper|CommandMenu|\.help|\.navigationTitle|\.alert|\.confirmationDialog|\.accessibilityLabel)'


def mac_strings(mac):
    """User-visible literals: arguments of SwiftUI text-taking views and modifiers, alert text, menu titles."""
    found = {}
    pats = [
        re.compile(STRING_CALLS + r'\(\s*"((?:\\.|[^"\\])*)"'),
        re.compile(r'(?:messageText|informativeText|title|prompt|message|placeholder|label|subtitle)\s*[:=]\s*"((?:\\.|[^"\\])*)"'),
        re.compile(r'NSMenuItem\(title:\s*"((?:\\.|[^"\\])*)"'),
        re.compile(r'addButton\(withTitle:\s*"((?:\\.|[^"\\])*)"'),
    ]
    for sub in ("UI", "App", "Magic", "MCP", "Engine", "Model"):
        d = os.path.join(mac, "Editroom", sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".swift") or fn in ("SelfTest.swift", "FormatCheck.swift"):
                continue
            src = strip_comments(open(os.path.join(d, fn), encoding="utf-8").read())
            for pat in pats:
                for m in pat.finditer(src):
                    s = m.group(1)
                    if not s or "\\(" in s and s.replace("\\(", "").strip() == "":
                        continue
                    if re.fullmatch(r"[\w.]+", s) and s.islower() and len(s) < 3:
                        continue
                    found.setdefault(s, set()).add(f"{sub}/{fn}")
    return {k: sorted(v) for k, v in sorted(found.items())}


def mac_tools(mac):
    src = open(os.path.join(mac, "Editroom/MCP/MCPTools.swift"), encoding="utf-8").read()
    return re.findall(r'tool\("(\w+)", "([^"]*)"', src)


def mac_head(mac):
    try:
        return subprocess.run(["git", "-C", mac, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return "?"


# MARK: Report

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--mac", default=os.path.join(here, "..", "editroom"))
    ap.add_argument("--lx", default=here)
    ap.add_argument("--bin", default=os.path.join(here, "build", "debug", "src", "app", "editroom-lx"))
    ap.add_argument("--out")
    ap.add_argument("--update-strings", action="store_true", help="write the Mac's strings into resources/strings.json")
    a = ap.parse_args()

    lines = [f"# Parity diff — Mac editroom@{mac_head(a.mac)} vs LX", ""]
    failed = False

    # Schema
    mac = mac_schema(a.mac)
    out, err = run(a.bin, "--dump-schema")
    lx = {}
    for l in (out or "").splitlines():
        if l.startswith("SCHEMA "):
            _, t, keys = l.split(" ", 2)
            lx[t] = sorted(k for k in keys.split(",") if k)
    lines.append("## Schema (.room Codable fields)")
    diffs = []
    for t in MODEL_TYPES:
        m, x = set(mac.get(t, [])), set(lx.get(t, []))
        if t not in mac:
            diffs.append(f"- `{t}`: not found in the Mac source")
        elif t not in lx:
            diffs.append(f"- `{t}`: not dumped by LX")
        elif m != x:
            diffs.append(f"- `{t}`: Mac only {sorted(m - x)}, LX only {sorted(x - m)}")
    lines += diffs or [f"All equal ({len(MODEL_TYPES)} types, {sum(len(v) for v in mac.values())} fields)."]
    failed |= bool(diffs)
    lines.append("")

    # Shortcuts
    macs = mac_shortcuts(a.mac)
    out, _ = run(a.bin, "--dump-shortcuts")
    lxs = {}
    for l in (out or "").splitlines():
        if l.startswith("SHORTCUT "):
            row = l[len("SHORTCUT "):]
            lxs[row.split("|")[0]] = row
    lines.append("## Shortcuts (ShortcutAction)")
    diffs = []
    order_mac, order_lx = list(macs), list(lxs)
    if order_mac != order_lx:
        diffs.append(f"- order or set differs: Mac {order_mac} / LX {order_lx}")
    for k in order_mac:
        if macs[k] != lxs.get(k):
            diffs.append(f"- `{k}`: Mac `{macs[k]}` / LX `{lxs.get(k)}`")
    lines += diffs or [f"All equal ({len(macs)} actions)."]
    failed |= bool(diffs)
    lines.append("")

    # Strings
    strings = mac_strings(a.mac)
    table_path = os.path.join(a.lx, "resources", "strings.json")
    if a.update_strings:
        os.makedirs(os.path.dirname(table_path), exist_ok=True)
        with open(table_path, "w", encoding="utf-8") as f:
            json.dump({"mac": mac_head(a.mac), "strings": {k: {"mac": v} for k, v in strings.items()}}, f,
                      ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
    table = {}
    if os.path.exists(table_path):
        table = json.load(open(table_path, encoding="utf-8")).get("strings", {})
    added = sorted(set(strings) - set(table))
    removed = sorted(set(table) - set(strings))
    lines.append("## Strings (user-visible literals)")
    lines.append(f"Mac: {len(strings)}; LX table: {len(table)}; new on the Mac: {len(added)}; gone from the Mac: {len(removed)}.")
    for s in added[:200]:
        lines.append(f"- new: `{s}` ({', '.join(strings[s])})")
    for s in removed[:200]:
        lines.append(f"- gone: `{s}`")
    lines.append("")

    # Menus
    lines.append("## Menus")
    mac_menu = os.path.join(a.lx, "tests", "parity", "mac-menu.txt")
    out, _ = run(a.bin, "--dump-menu")
    lx_menu = [l for l in (out or "").splitlines() if l.startswith("MENU")]
    if not os.path.exists(mac_menu):
        lines.append("No Mac capture yet: run `Editroom --dump-menu` on a Mac and save the MENU lines to tests/parity/mac-menu.txt.")
    elif not lx_menu:
        lines.append("LX has no menus yet (phase P3).")
    else:
        mm = [l for l in open(mac_menu, encoding="utf-8").read().splitlines() if l.startswith("MENU")]
        lines.append("All equal." if mm == lx_menu else "Differ:\n```\n" + "\n".join(
            f"{x!r:60} | {y!r}" for x, y in zip(mm + [""] * len(lx_menu), lx_menu + [""] * len(mm)) if x != y) + "\n```")
    lines.append("")

    # MCP
    tools = mac_tools(a.mac)
    out, _ = run(a.bin, "--dump-mcp-tools")
    lx_tools = [l.split()[1] for l in (out or "").splitlines() if l.startswith("TOOL ")]
    lines.append("## MCP tools")
    missing = [n for n, _ in tools if n not in lx_tools]
    extra = [n for n in lx_tools if n not in dict(tools)]
    lines.append(f"Mac: {len(tools)}; LX: {len(lx_tools)}; still to port: {len(missing)}; LX only: {len(extra)}.")
    if missing and lx_tools:
        lines.append("- to port: " + ", ".join(f"`{m}`" for m in missing))
    lines.append("")

    report = "\n".join(lines)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(report + "\n")
    print(report)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
