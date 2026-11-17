#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inline erisian-upload.js into index.html, giving one self-contained page
that works when opened from disk (Chrome blocks ES modules on file://)."""
import re
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
module = (here / "erisian-upload.js").read_text()
names = re.findall(r"^export (?:const|function) (\w+)", module, re.M)
body = re.sub(r"^export ", "", module, flags=re.M)
script = ("(() => {\n" + body + "\nwindow.ErisianUpload = { "
          + ", ".join(names) + " };\n})();")
page = (here / "index.html").read_text().replace("/*PROTOCOL*/", script)
out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/erisian-uploader.html")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(page)
print(f"uploader page: {out} ({len(page)} bytes)")
