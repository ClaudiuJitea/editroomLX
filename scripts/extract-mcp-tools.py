#!/usr/bin/env python3
"""MCPTools.definitions (Swift) → resources/mcp-tools.json, so LX lists exactly the Mac's tools.

    scripts/extract-mcp-tools.py [--mac ../editroom] [--out resources/mcp-tools.json]

Reads the `tool(name, title, description, props, required:, readOnly:)` calls and the `str()`/`num()`
helpers, and the `instructions` string. Run in each sync; parity-diff.py then compares the names.
"""
import argparse
import json
import os
import re
import sys


def swift_string(src, i):
    """A Swift "…" literal at src[i] (with \\( interpolation refused); returns (value, end)."""
    assert src[i] == '"'
    if src.startswith('"""', i):
        end = src.index('"""', i + 3)
        body = src[i + 3:end]
        lines = body.split("\n")[1:-1]
        indent = min((len(l) - len(l.lstrip()) for l in lines if l.strip()), default=0)
        text = "\n".join(l[indent:] for l in lines)
        text = text.replace("\\\n", "")
        return unescape(text), end + 3
    j, out = i + 1, []
    while src[j] != '"':
        if src[j] == "\\":
            out.append(src[j:j + 2])
            j += 2
            continue
        out.append(src[j])
        j += 1
    return unescape("".join(out)), j + 1


def unescape(s):
    return s.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")


# Swift expressions in the definitions, evaluated by hand (re-check in a sync if parsing fails).
KNOWN = {
    "Marker.Tint.allCases.map(\\.rawValue)": ["white", "yellow", "green", "blue", "purple", "red"],
    "TransitionKind.allCases.map(\\.rawValue)": ["dissolve", "dipToBlack", "wipe"],
    "FadeCurve.allCases.map(\\.rawValue)": ["linear", "easeIn", "easeOut", "smooth"],
}


class Parser:
    def __init__(self, src):
        self.s, self.i = src, 0

    def ws(self):
        while self.i < len(self.s) and self.s[self.i] in " \t\r\n":
            self.i += 1
        if self.s.startswith("//", self.i):
            self.i = self.s.index("\n", self.i)
            self.ws()

    def peek(self, t):
        self.ws()
        return self.s.startswith(t, self.i)

    def eat(self, t):
        self.ws()
        if not self.s.startswith(t, self.i):
            raise SyntaxError(f"expected {t!r} at {self.s[self.i:self.i + 60]!r}")
        self.i += len(t)

    def value(self):
        self.ws()
        c = self.s[self.i]
        if c == '"':
            v, self.i = swift_string(self.s, self.i)
            # Adjacent "a" + "b" concatenation.
            while self.peek("+"):
                self.eat("+")
                self.ws()
                w, self.i = swift_string(self.s, self.i)
                v += w
            return v
        if c == "[":
            return self.collection()
        m = re.match(r"(str|num|int|bool)\(", self.s[self.i:])
        if m:
            kind = m.group(1)
            self.i += len(m.group(0))
            d = self.value()
            self.eat(")")
            return {"type": {"str": "string", "num": "number", "int": "integer", "bool": "boolean"}[kind], "description": d}
        for expr, val in KNOWN.items():
            if self.s.startswith(expr, self.i):
                self.i += len(expr)
                return val
        m = re.match(r"(true|false)", self.s[self.i:])
        if m:
            self.i += len(m.group(0))
            return m.group(0) == "true"
        m = re.match(r"-?\d+(\.\d+)?", self.s[self.i:])
        if m:
            self.i += len(m.group(0))
            return float(m.group(0)) if "." in m.group(0) else int(m.group(0))
        raise SyntaxError(f"value at {self.s[self.i:self.i + 80]!r}")

    def collection(self):
        self.eat("[")
        if self.peek(":"):
            self.eat(":")
            self.eat("]")
            return {}
        if self.peek("]"):
            self.eat("]")
            return []
        first = self.value()
        if self.peek(":"):
            out = {}
            self.eat(":")
            out[first] = self.value()
            while self.peek(","):
                self.eat(",")
                if self.peek("]"):
                    break
                k = self.value()
                self.eat(":")
                out[k] = self.value()
            self.ws()
            if self.peek("as"):
                self.eat("as")
                self.eat("[String: Any]")
            self.eat("]")
            self.ws()
            if self.s.startswith("as [String: Any]", self.i):
                self.i += len("as [String: Any]")
            return out
        out = [first]
        while self.peek(","):
            self.eat(",")
            if self.peek("]"):
                break
            out.append(self.value())
        self.eat("]")
        return out


def extract(mac):
    src = open(os.path.join(mac, "Editroom/MCP/MCPTools.swift"), encoding="utf-8").read()
    m = re.search(r'static let instructions = """', src)
    instructions, _ = swift_string(src, m.start() + len("static let instructions = "))
    tools = []
    for start, stop in (("static var definitions", "] + moreDefinitions"), ("static var moreDefinitions", "\n    }\n")):
        a = src.index(start)
        tools += parse_block(src[a:src.index(stop, a)])
    return {"instructions": instructions, "tools": tools}


def parse_block(block):
    tools = []
    for call in re.finditer(r"\btool\(", block):
        p = Parser(block)
        p.i = call.end()
        name = p.value(); p.eat(",")
        title = p.value(); p.eat(",")
        desc = p.value()
        props, required, read_only = {}, [], False
        while p.peek(","):
            p.eat(",")
            if p.peek("required:"):
                p.eat("required:")
                required = p.value()
            elif p.peek("readOnly:"):
                p.eat("readOnly:")
                read_only = p.value()
            else:
                props = p.value()
        p.eat(")")
        tools.append({"name": name, "title": title, "description": desc,
                      "inputSchema": {"type": "object", "properties": props, "required": required},
                      "annotations": {"readOnlyHint": read_only, "destructiveHint": False, "openWorldHint": False}})
    return tools


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--mac", default=os.path.join(here, "..", "editroom"))
    ap.add_argument("--out", default=os.path.join(here, "resources", "mcp-tools.json"))
    a = ap.parse_args()
    data = extract(a.mac)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"{len(data['tools'])} tools → {a.out}")


if __name__ == "__main__":
    sys.exit(main())
