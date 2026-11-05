#!/usr/bin/env python3
"""Check local file links in Git-visible Markdown using only the standard library.

Historical reports retain the paths of their original workspace, so they are
excluded. Fenced code, inline code, web URLs and heading fragments are not
validated. Targets must exist in the checkout and be visible to Git, avoiding
false success from an ignored local App or results directory.
"""

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
INLINE_LINK = re.compile(r"\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s)\n]+)(?:\s+[\'\"][^)\n]*[\'\"])?\s*\)")
REFERENCE_LINK = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>\n]+>|[^\s]+)")
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")


def prose_lines(text):
    """Yield original line numbers, skipping fenced blocks and inline code."""
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        marker = FENCE.match(line)
        if fence:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence) and not line[marker.end():].strip():
                fence = None
            continue
        if marker:
            fence = marker[1]
            continue
        yield number, re.sub(r"(`+).*?\1", "", line)


def main():
    output = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    )
    visible = set(output.decode("utf-8").rstrip("\0").split("\0"))
    visible.discard("")
    errors = []
    documents = 0
    links = 0
    for name in sorted(visible):
        if not name.endswith(".md") or name.startswith("docs/history/"):
            continue
        source = ROOT / name
        if not source.is_file():  # An unstaged move may still have its old path in the index.
            continue
        documents += 1
        for number, line in prose_lines(source.read_text(encoding="utf-8")):
            targets = [m[1] for m in INLINE_LINK.finditer(line)]
            definition = REFERENCE_LINK.match(line)
            if definition:
                targets.append(definition[1])
            for raw in targets:
                url = urlsplit(raw.strip("<>"))
                if url.scheme or url.netloc or not url.path:
                    continue
                links += 1
                path = unquote(url.path)
                target = (ROOT / path.lstrip("/") if path.startswith("/") else source.parent / path).resolve()
                try:
                    relative = target.relative_to(ROOT).as_posix()
                except ValueError:
                    errors.append(f"{name}:{number}: target outside repository: {raw}")
                    continue
                tracked = relative in visible or any(p.startswith(relative.rstrip("/") + "/") for p in visible)
                if not target.exists() or not tracked:
                    errors.append(f"{name}:{number}: missing or Git-ignored target: {raw}")
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Checked {links} local file links in {documents} current Markdown files; {len(errors)} errors.")
    return int(bool(errors))


if __name__ == "__main__":
    sys.exit(main())
