#!/usr/bin/env python3
"""Push a "how full is each session's context" panel to a SmallTV.

    claude_context_panel.py --device smalltv-c31f.local

The device cannot know any of this, and until panels existed a screen like it
meant a firmware build and a flash. It is now just this script: it reads what
Claude Code already writes, turns it into rows, and POSTs them to /api/panel.

Where the numbers come from
---------------------------
Every assistant turn in ~/.claude/projects/<project>/<session>.jsonl carries a
usage record:

    "usage": {"input_tokens": 2, "cache_creation_input_tokens": 1117,
              "cache_read_input_tokens": 289681, "output_tokens": 3378}

The context a turn was sent is the sum of the three input figures — the cached
prefix plus whatever was new. The newest such record in a transcript is
therefore how full that session is right now.

The limit is a flag because nothing in the transcript states it. The default
comes from measurement rather than a guess: across these transcripts the largest
context ever sent was 999,317 tokens, so the ceiling is 1M.

Run it from a hook, not a launchd agent
---------------------------------------
macOS denies a launchd agent Local Network access, which is why the usage daemon
had to be polled by the device instead of pushing to it. A Claude Code hook runs
in your own session and can reach the LAN — and it fires exactly when a session's
context changes, which is exactly when this panel is worth refreshing.
"""
import argparse
import glob
import json
import os
import sys
import urllib.request

PROJECTS = os.path.expanduser("~/.claude/projects")
DEFAULT_LIMIT = 1_000_000
TIMEOUT_S = 2.0
PANEL_ROWS = 6          # the device keeps six; sending more just wastes bytes


def tail_usage(path, probe_bytes=400_000):
    """The newest usage record and cwd in a transcript.

    Reads the tail rather than the file: these grow past a megabyte, this runs
    on every prompt, and only the last few entries matter.
    """
    size = os.path.getsize(path)
    with open(path, "rb") as fh:
        if size > probe_bytes:
            fh.seek(size - probe_bytes)
            fh.readline()          # discard the partial line
        lines = fh.read().decode("utf-8", "replace").splitlines()

    context, cwd = 0, None
    for line in reversed(lines):
        try:
            d = json.loads(line)
        except Exception:
            continue
        if cwd is None and d.get("cwd"):
            cwd = d["cwd"]
        if context == 0:
            u = (d.get("message") or {}).get("usage") or d.get("usage") or {}
            if isinstance(u, dict):
                t = ((u.get("input_tokens") or 0)
                     + (u.get("cache_creation_input_tokens") or 0)
                     + (u.get("cache_read_input_tokens") or 0))
                if t:
                    context = t
        if context and cwd:
            break
    return context, cwd


def short(n):
    """Tokens in the seven characters a panel value gets."""
    if n >= 1_000_000:
        return "%.2fM" % (n / 1_000_000.0)
    if n >= 1_000:
        return "%dk" % round(n / 1000.0)
    return str(n)


def collect(limit, max_age_min):
    """One row per project, newest transcript, busiest first."""
    import time
    now = time.time()
    best = {}                       # project dir -> (context, cwd, mtime)
    for path in glob.glob(os.path.join(PROJECTS, "*", "*.jsonl")):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if (now - mtime) / 60.0 > max_age_min:
            continue
        key = os.path.dirname(path)
        if key in best and best[key][2] >= mtime:
            continue
        try:
            context, cwd = tail_usage(path)
        except Exception:
            continue
        if not context:
            continue
        best[key] = (context, cwd, mtime)

    rows = []
    for context, cwd, _ in sorted(best.values(), key=lambda v: -v[0]):
        label = os.path.basename(os.path.normpath(cwd)) if cwd else "?"
        pct = int(round(100.0 * context / limit))
        rows.append({"label": label[:13], "bar": min(pct, 100), "value": short(context)})
    return rows[:PANEL_ROWS]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default=os.environ.get("SMALLTV_HOST", ""))
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT,
                    help="context window in tokens (default %d)" % DEFAULT_LIMIT)
    ap.add_argument("--max-age-min", type=float, default=240.0,
                    help="ignore transcripts untouched for longer than this")
    ap.add_argument("--ttl-sec", type=int, default=3600,
                    help="how long the panel stays in the carousel unrefreshed")
    ap.add_argument("--title", default="CONTEXT")
    ap.add_argument("--id", default="ctx")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    rows = collect(a.limit, a.max_age_min)
    if not rows:
        # Nothing active: drop the panel so it leaves the carousel now rather
        # than sitting there stale until its ttl runs out.
        body = {"id": a.id, "drop": True}
    else:
        body = {"id": a.id, "title": a.title, "ttlSec": a.ttl_sec, "rows": rows}

    if a.dry_run or not a.device:
        print(json.dumps(body, indent=1))
        return 0

    req = urllib.request.Request(
        "http://%s/api/panel" % a.device,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        urllib.request.urlopen(req, timeout=TIMEOUT_S).read()
    except Exception:
        pass    # a desk toy must never be able to fail a hook
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)
