#!/usr/bin/env python3
"""Write data/config.local.json with a GitHub user and every public repo.

config.local.json is the board's second config: the same shape as
config.json, merged on top of it, and gitignored -- it is where your own
accounts go, so the committed config stays a demo for everyone else.

    python3 tools/my-github.py rtorcato        # then ./push-config social
"""
import json
import pathlib
import sys
import urllib.request

user = sys.argv[1] if len(sys.argv) > 1 else "rtorcato"
req = urllib.request.Request(f"https://api.github.com/users/{user}/repos?per_page=100&sort=updated&type=owner",
                             headers={"User-Agent": "Mozilla/5.0 (esp32-social)"})
with urllib.request.urlopen(req, timeout=20) as r:
    repos = json.load(r)
accounts = [{"service": "github", "id": user, "label": user}]
for rp in repos:
    if rp.get("fork") or rp.get("archived"):
        continue
    accounts.append({"service": "github", "id": rp["full_name"], "label": rp["name"][:22], "repo": True})
out = pathlib.Path(__file__).absolute().parent.parent / "data" / "config.local.json"
existing = {}
if out.exists():
    existing = json.loads(out.read_text())
kept = [a for a in existing.get("accounts", []) if a.get("service") != "github"]
existing["accounts"] = kept + accounts
out.write_text(json.dumps(existing, indent=2) + "\n")
print(f"{len(accounts) - 1} repos + the user -> {out.relative_to(out.parents[2])} (gitignored); now ./push-config social")
