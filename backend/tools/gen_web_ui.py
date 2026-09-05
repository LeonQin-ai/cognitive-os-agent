#!/usr/bin/env python3
"""Regenerate include/cognitive-os-agent/api/web_ui.h from apps/web/index.html.

Usage: python3 tools/gen_web_ui.py
Keeps the embedded web UI string in sync with the canonical HTML file.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "web", "index.html")
DST = os.path.join(ROOT, "include", "cognitive-os-agent", "api", "web_ui.h")

with open(SRC, "r", encoding="utf-8") as f:
    text = f.read()

lines = []
lines.append("/* web_ui.h - GENERATED from apps/web/index.html by tools/gen_web_ui.py.")
lines.append(" * Do not edit by hand; re-run the generator after changing the HTML. */")
lines.append("#pragma once")
lines.append("static const char coa_web_index_html[] =")
for line in text.split("\n"):
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append('    "%s\\n"' % esc)
lines.append("    ;")

with open(DST, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")

print("wrote %s (%d bytes)" % (DST, os.path.getsize(DST)))
