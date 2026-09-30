#!/usr/bin/env python3
"""Bundle the website into one self-contained file: docs/index.html

    python3 tools/build_web.py
    python3 tools/build_web.py --author "Your Name" --link https://github.com/you

Inlines web/engine.js, the WebAssembly engine (base64), the C sources shown
in the "C source" tab and the two sample datasets, so the page works on any
static host (GitHub Pages, Netlify, Vercel) with no build step there.
"""
import argparse, base64, html, json, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ["fraud.h", "fraud.c", "wasm_api.c", "main.c", "namemap.c"]


def js(value):
    return json.dumps(value).replace("</", "<\\/")


def build(author=None, link=None):
    tpl = (ROOT / "web/template.html").read_text(encoding="utf-8")
    wasm = base64.b64encode((ROOT / "web/looptrace.wasm").read_bytes()).decode()
    sources = {n: (ROOT / "src" / n).read_text(encoding="utf-8") for n in SOURCES}
    parts = {
        "/*__ENGINE__*/": (ROOT / "web/engine.js").read_text(encoding="utf-8"),
        "__WASM_B64__": wasm,
        "/*__SOURCES__*/null": js(sources),
        '/*__SAMPLE__*/""': js((ROOT / "data/sample.csv").read_text(encoding="utf-8")),
        '/*__CLEAN__*/""': js((ROOT / "data/clean.csv").read_text(encoding="utf-8")),
    }
    for key, value in parts.items():
        assert tpl.count(key) == 1, "template marker missing: " + key
        tpl = tpl.replace(key, value)

    if author:
        name = html.escape(author)
        credit = (f'Built by <a href="{html.escape(link, quote=True)}">{name}</a>' if link
                  else f"Built by {name}")
        tpl = tpl.replace("<!-- author -->", credit + " · ")
    return tpl


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--author", help="your name, shown in the page footer")
    ap.add_argument("--link", help="link for your name (GitHub, LinkedIn, ...)")
    ap.add_argument("--out", default=str(ROOT / "docs/index.html"), help="output file")
    args = ap.parse_args()

    head, body = build(args.author, args.link).split("<!--/HEAD-->", 1)
    page = ('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
            '<meta name="description" content="Detect money-laundering loops in a transaction graph '
            "with Tarjan's SCC algorithm and a time-pruned DFS, written in C and compiled to WebAssembly.\">\n"
            + head.strip() + "\n</head>\n<body>" + body + "</body>\n</html>\n")
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out} ({len(page) // 1024} KB)")


if __name__ == "__main__":
    main()
