#!/usr/bin/env python3
"""Make one release's developer-doc pages for the GitHub wiki (NU-Hachimi/alia-engine.wiki), from that release's tag:

    python3 tools/wiki_pages.py v0.2.1 ~/Projects/alia-engine-wiki        (v0.2-zh-*.md and the English sources)

Pages are named <minor>-<lang>-<Page> (v0.2-zh-API-World); docs/api.md is split by section. Cross-references become
wiki links in the same language, repository files GitHub links at the tag. Pages come out in their source language
(Chinese, and English for formats.md and stats-protocol.md); the other language is a translation kept beside them,
and Home.md / _Sidebar.md list the versions. docs/architecture.md (the engine's design) is not a developer page.
"""

import os
import re
import subprocess
import sys

SEC2PAGE = {"0": "API-Quick-Start", "1": "API-Loop-and-Engine", "2": "API-Materials", "3": "API-World", "4": "API-Animation",
            "5": "API-Collision", "6": "API-Particles", "7": "API-Systems", "7.1": "API-alia_kit",
            "8": "API-Roadmap-and-Decisions", "9": "API-Roadmap-and-Decisions"}
DOCS = {"docs/getting-started.md": "Getting-Started", "docs/project.md": "Projects-and-alia", "docs/plugins.md": "Plugins",
        "docs/formats.md": "File-Formats", "docs/stats-protocol.md": "Stats-Output", "apps/sample/README.md": "Sample-Game",
        "docs/api.md": "API-Quick-Start"}
ENGLISH_SOURCES = ("docs/formats.md", "docs/stats-protocol.md")


def main():
    tag, out = sys.argv[1], sys.argv[2]
    minor = ".".join(tag.split(".")[:2])
    blob = f"https://github.com/NU-Hachimi/alia-engine/blob/{tag}/"

    def show(path):
        return subprocess.run(["git", "show", f"{tag}:{path}"], capture_output=True, text=True, check=True).stdout

    def exists(path):  # a game project's own src/main.py etc. aren't the repository's
        return subprocess.run(["git", "cat-file", "-e", f"{tag}:{path.rstrip('/')}"], capture_output=True).returncode == 0

    def page(lang, name):
        return f"{minor}-{lang}-{name}"

    def sec_page(sec):
        return SEC2PAGE.get(sec) or SEC2PAGE.get(sec.split(".")[0], "API-Quick-Start")

    def rewrite(text, lang, in_api):
        # architecture.md first, with § as &sect; so the bare-§ pass below leaves those links alone
        text = re.sub(r"`?(?:docs/)?architecture\.md`?(\s*§\s*[0-9]+(?:\.[0-9]+)*)?",
                      lambda m: f"[architecture.md{(' &sect;' + m.group(1).strip()[1:].strip()) if m.group(1) else ''}]"
                                f"({blob}docs/architecture.md)", text)
        text = re.sub(r"(tools\.md`?\s*)§", r"\1&sect;", text)  # the devkit's docs/tools.md, not the API
        text = re.sub(r"`?(?:docs/)?api\.md`?\s*§\s*([0-9]+(?:\.[0-9]+)?)",
                      lambda m: f"[&sect;{m.group(1)}]({page(lang, sec_page(m.group(1)))})", text)
        for src, name in DOCS.items():
            base = os.path.basename(src)
            pat = (r"`?(?:\.\./)?(?:docs/)?" + re.escape(base) + r"`?") if src.startswith("docs/") else r"`?apps/sample/(?:README\.md)?`?(?![\w./-])"
            text = re.sub(pat, f"[{name.replace('-', ' ')}]({page(lang, name)})", text)
        if in_api:
            text = re.sub(r"(?<![\[\w])§\s*([0-9]+(?:\.[0-9]+)?)(?!\])",
                          lambda m: f"[&sect;{m.group(1)}]({page(lang, sec_page(m.group(1)))})", text)
        return re.sub(r"`((?:src|tools|tests|python|plugins|apps|cmake|third_party)/[\w./-]+)`",
                      lambda m: f"[`{m.group(1)}`]({blob}{m.group(1)})" if exists(m.group(1)) else m.group(0), text)

    def links(text, lang, in_api):
        """rewrite() outside code blocks only."""
        out, fence = [], False
        for part in re.split(r"(^```.*$)", text, flags=re.M):
            if part.startswith("```"):
                fence = not fence
                out.append(part)
            else:
                out.append(part if fence else rewrite(part, lang, in_api))
        return "".join(out)

    def write(name, lang, body):
        other = "en" if lang == "zh" else "zh"
        header = f"> alia-engine **{minor}** ({tag}) · [{'English' if lang == 'zh' else '中文'}]({page(other, name)}) · [Home](Home)\n\n"
        with open(os.path.join(out, page(lang, name) + ".md"), "w") as f:
            f.write(header + body.rstrip() + "\n")

    pages, cur = {}, "API-Quick-Start"
    for line in show("docs/api.md").splitlines(keepends=True):
        m = re.match(r"^(##|###) ([0-9]+(?:\.[0-9]+)?)\.? ", line)
        if m and (m.group(1) == "##" or m.group(2) == "7.1"):
            cur = sec_page(m.group(2))
        pages.setdefault(cur, []).append(line)
    for name, lines in pages.items():
        write(name, "zh", links("".join(lines), "zh", True))
    for src, name in DOCS.items():
        if src != "docs/api.md":
            lang = "en" if src in ENGLISH_SOURCES else "zh"
            write(name, lang, links(show(src), lang, False))


if __name__ == "__main__":
    main()
