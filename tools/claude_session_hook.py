#!/usr/bin/env python3
"""Report this Claude Code session's state to a SmallTV running smalltv-mod.

Installed as three Claude Code hooks, it is what makes the device's Sessions
tab show anything:

    UserPromptSubmit -> working    (red, blinking: thinking or answering)
    Stop             -> waiting    (green: finished, wants your reply)
    SessionEnd       -> end        (drop the row)

Usage (the hook event is the one argument):

    claude_session_hook.py working --device 192.168.1.208

Design notes, because a hook runs on every prompt and every stop:

  * It never fails the tool call it is attached to. Any error - no device, bad
    JSON on stdin, DNS refusing to answer - exits 0 silently. A desk toy must
    not be able to break your editor.
  * It never blocks for long: one short timeout, no retry. A session's state is
    superseded by the next event anyway, so a dropped update costs nothing.
  * The label is the project directory's name, which is what actually tells two
    sessions apart when you have several open.
"""
import json
import os
import sys
import urllib.error
import urllib.request

TIMEOUT_S = 1.5
DEFAULT_DEVICE = os.environ.get("SMALLTV_HOST", "")


LOG_PATH = os.path.expanduser("~/.claude/smalltv-hook.log")
LOG_MAX = 64 * 1024


def _log(state: str, session_id: str, label: str, event: str = "?") -> None:
    """Record what actually fired.

    Which events a given Claude Code build raises for an interrupted turn is not
    something to guess at twice: this makes it observable. Capped and best
    effort, and never allowed to affect the hook's exit status.
    """
    try:
        if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > LOG_MAX:
            os.replace(LOG_PATH, LOG_PATH + ".1")
        with open(LOG_PATH, "a") as fh:
            fh.write("%s %-16s -> %-8s %s %s\n" % (
                __import__("datetime").datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                event, state, session_id, label))
    except Exception:
        pass


def main() -> int:
    state = sys.argv[1] if len(sys.argv) > 1 else "working"

    device = DEFAULT_DEVICE
    if "--device" in sys.argv:
        i = sys.argv.index("--device")
        if i + 1 < len(sys.argv):
            device = sys.argv[i + 1]
    if not device:
        return 0

    # The hook payload arrives on stdin. Everything here is optional: if the
    # shape ever changes, we fall back rather than raise.
    try:
        payload = json.load(sys.stdin)
    except Exception:
        payload = {}

    session_id = str(payload.get("session_id") or "")[:8]
    if not session_id:
        return 0

    cwd = payload.get("cwd") or os.getcwd()
    label = os.path.basename(os.path.normpath(cwd))[:23]

    # The payload names the event that fired, which is the one thing worth
    # knowing when three different events all map to "waiting".
    _log(state, session_id, label, str(payload.get("hook_event_name") or "?"))

    body = json.dumps({"id": session_id, "label": label, "state": state}).encode()
    url = "http://%s/api/session" % device
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}
    )
    try:
        urllib.request.urlopen(req, timeout=TIMEOUT_S).read()
    except Exception:
        pass  # deliberately silent; see the module docstring
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)
