#!/usr/bin/env python3
"""
CI & Git status watcher applet for markee.

Two modes (zero external Python dependencies, stdlib only):
  1. Local Git + optional build/test runner:
       python3 examples/git_watch.py --repo . --cmd "make test"
     Displays the active git branch and dirty status; whenever files or HEAD
     change, runs `--cmd`, shows a running indicator, and flashes PASS/FAIL
     banners with gentle chime/alert sounds.

  2. GitHub Actions watcher:
       python3 examples/git_watch.py --github owner/repo
     Polls the latest workflow run (uses $GITHUB_TOKEN if set) and alerts on
     completion or status changes.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

# The 3x5 font advances 4 px per character (3 px glyph + 1 px spacing).
SMALL_FONT_ADVANCE = 4


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"].rstrip("/")
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


class MarkeeClient:
    def __init__(self, base_url: str, token: Optional[str]):
        self.base_url = base_url.rstrip("/")
        self.token = token

    def post_json(self, path: str, payload: Any) -> None:
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        req = urllib.request.Request(
            f"{self.base_url}{path}",
            data=json.dumps(payload).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            resp.read()

    def display_size(self) -> Tuple[int, int]:
        """Return the live (width, height) of the matrix from GET /info."""
        headers = {"Authorization": f"Bearer {self.token}"} if self.token else {}
        req = urllib.request.Request(f"{self.base_url}/info", headers=headers)
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            info = json.loads(resp.read().decode("utf-8"))
        return int(info.get("width", 72)), int(info.get("height", 16))

    def draw(self, ops: List[Dict[str, Any]], transition: Optional[str] = None) -> None:
        q = f"?transition={transition}" if transition else ""
        self.post_json(f"/draw{q}", ops)

    def notify(self, text: str, border: str, color: str, ttl: float = 4.0, sound: Optional[str] = None) -> None:
        body: Dict[str, Any] = {"text": text, "border": border, "color": color, "ttl": ttl}
        if sound:
            body["sound"] = sound
        self.post_json("/notify", body)

    def alert_melody(self, melody: str, volume: int = 75) -> None:
        self.post_json("/alert", {"melody": melody, "volume": volume})


def git_info(repo: str) -> Tuple[str, int, str]:
    """Return (branch, dirty_count, fingerprint)."""
    def run_git(args: List[str]) -> str:
        res = subprocess.run(["git", "-C", repo] + args, capture_output=True, text=True, timeout=5.0, check=False)
        return res.stdout.strip() if res.returncode == 0 else ""

    branch = run_git(["rev-parse", "--abbrev-ref", "HEAD"]) or "detached"
    head = run_git(["rev-parse", "--short", "HEAD"])
    porcelain = run_git(["status", "--porcelain"])
    lines = [ln for ln in porcelain.splitlines() if ln.strip()]
    dirty = len(lines)
    diff_stat = run_git(["diff", "--stat"])
    fp = hashlib.sha1(f"{branch}|{head}|{porcelain}|{diff_stat}".encode("utf-8")).hexdigest()
    return branch, dirty, fp


def poll_github_actions(repo_slug: str, gh_token: Optional[str]) -> Tuple[str, str, str]:
    """Return (status_kind, label, run_id) where status_kind is 'pass'|'fail'|'running'|'unknown'."""
    url = f"https://api.github.com/repos/{repo_slug}/actions/runs?per_page=1"
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "markee-git-watch"}
    if gh_token:
        headers["Authorization"] = f"Bearer {gh_token}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=8.0) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    runs = data.get("workflow_runs") or []
    if not runs:
        return "unknown", "NO RUNS", ""
    run = runs[0]
    run_id = str(run.get("id", ""))
    status = str(run.get("status", ""))
    conclusion = str(run.get("conclusion") or "")
    branch = str(run.get("head_branch") or "ci")
    if status != "completed":
        return "running", f"CI {branch}", run_id
    if conclusion == "success":
        return "pass", f"OK {branch}", run_id
    return "fail", f"FAIL {branch}", run_id


def build_status_ops(state: str, top_text: str, bottom_text: str, size: Tuple[int, int]) -> List[Dict[str, Any]]:
    palette = {
        "pass": ("check", "#22ee88", "#04160c", "#1b5e3c"),
        "fail": ("cross", "#ff4433", "#1a0404", "#661818"),
        "running": ("bolt", "amber", "#161004", "#553b10"),
        "clean": ("check", "#38e8b0", "#041214", "#18464c"),
        "dirty": ("flame", "amber", "#140e04", "#4a3310"),
        "unknown": ("bell", "#8899aa", "#080a0e", "#26303d"),
    }
    icon, fg, bg, border = palette.get(state, palette["unknown"])
    w, h = size
    # Two text rows (5 px each, 2 px apart) centered vertically beside an 8x8 icon,
    # all inside a full-display rounded border.
    text_x = 13
    top = max(1, (h - 12) // 2)
    max_chars = max(1, (w - text_x - 2 + 1) // SMALL_FONT_ADVANCE)
    return [
        {"op": "clear", "color": bg},
        {"op": "rect", "x": 0, "y": 0, "w": w, "h": h, "color": border, "radius": 2},
        {"op": "icon", "name": icon, "x": 3, "y": max(0, (h - 8) // 2), "color": fg},
        {"op": "text", "x": text_x, "y": top, "text": top_text[:max_chars], "color": "warmwhite", "font": "3x5"},
        {"op": "text", "x": text_x, "y": top + 7, "text": bottom_text[:max_chars], "color": fg, "font": "3x5"},
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description="CI and local Git status watcher for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional markee Bearer token")
    parser.add_argument("--repo", default=".", help="Local git repository path (default: .)")
    parser.add_argument("--cmd", help="Optional shell command to run when repo changes (e.g. 'make test')")
    parser.add_argument("--github", help="Watch GitHub Actions latest run for 'owner/repo' instead of local git")
    parser.add_argument("--interval", type=float, default=5.0, help="Poll interval in seconds (default: 5.0)")
    parser.add_argument("--mute", action="store_true", help="Disable alert sounds on pass/fail")
    parser.add_argument("--once", action="store_true", help="Run one check cycle and exit")
    parser.add_argument("--dry-run", action="store_true", help="Print status without contacting markee")
    args = parser.parse_args()

    client = MarkeeClient(args.url, args.token)
    size = (72, 16)
    if not args.dry_run:
        try:
            size = client.display_size()
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print(f"Error: cannot read display size from {args.url}/info ({exc})", file=sys.stderr)
            return 1
    last_fp: Optional[str] = None
    last_ci_key: Optional[Tuple[str, str]] = None

    try:
        while True:
            if args.github:
                try:
                    kind, label, run_id = poll_github_actions(args.github, os.environ.get("GITHUB_TOKEN"))
                except Exception as exc:
                    print(f"GitHub poll failed: {exc}", file=sys.stderr)
                    if args.once:
                        return 1
                    time.sleep(max(2.0, args.interval))
                    continue

                ci_key = (kind, run_id)
                ops = build_status_ops(kind, args.github.split("/")[-1], label, size)
                if args.dry_run:
                    print(f"[github] {args.github}: {kind} ({label}, run={run_id})")
                elif ci_key != last_ci_key:
                    client.draw(ops, transition="fade" if last_ci_key else None)
                    if last_ci_key is not None and kind in ("pass", "fail"):
                        client.notify(
                            "CI PASSED" if kind == "pass" else "CI FAILED",
                            border="green" if kind == "pass" else "red",
                            color="green" if kind == "pass" else "red",
                            sound=None if args.mute else ("chime" if kind == "pass" else "bell"),
                        )
                    print(f"[github] {args.github}: {kind} ({label})")
                last_ci_key = ci_key
            else:
                branch, dirty, fp = git_info(args.repo)
                changed = fp != last_fp
                if changed:
                    last_fp = fp
                    if args.cmd:
                        if not args.dry_run:
                            client.draw(build_status_ops("running", branch, "RUNNING...", size))
                        t0 = time.monotonic()
                        proc = subprocess.run(args.cmd, shell=True, cwd=args.repo, check=False)
                        elapsed = time.monotonic() - t0
                        passed = proc.returncode == 0
                        state = "pass" if passed else "fail"
                        summary = f"PASS {elapsed:.1f}S" if passed else f"FAIL RC={proc.returncode}"
                        ops = build_status_ops(state, branch, summary, size)
                        if args.dry_run:
                            print(f"[git] {branch} dirty={dirty} -> {summary}")
                        else:
                            client.draw(ops, transition="fade")
                            client.notify(
                                "BUILD OK" if passed else "BUILD FAIL",
                                border="green" if passed else "red",
                                color="green" if passed else "red",
                                ttl=3.5,
                            )
                            if not args.mute:
                                client.alert_melody("C5:100,E5:100,G5:200" if passed else "G4:140,E4:140,C4:260")
                            print(f"[git] {branch}: {summary}")
                    else:
                        state = "dirty" if dirty > 0 else "clean"
                        sub = f"{dirty} MODIFIED" if dirty > 0 else "CLEAN"
                        ops = build_status_ops(state, branch, sub, size)
                        if args.dry_run:
                            print(f"[git] {branch}: {sub}")
                        else:
                            client.draw(ops, transition="fade" if not args.once else None)
                            print(f"[git] {branch}: {sub}")

            if args.once:
                return 0
            time.sleep(max(1.0, args.interval))
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
