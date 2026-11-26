#!/usr/bin/env python3
"""Build Game Store homebrew entries from the retrobrews collections on GitHub.

retrobrews (github.com/retrobrews) gathers homebrew games whose authors allow
free distribution, one repo per system, each with a gamelist.xml. The device
downloads every ROM straight from that repo (pinned to a commit), so we do not
re-host the games. Cover thumbnails are made from the repo's screenshots.

Usage: build_homebrew.py <out_json> [img_dir img_base_url]
(needs git and python3-pillow)
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

# retrobrews repo -> Onion Emu/ folder
REPOS = {
    "nes-games": "FC",
    "snes-games": "SFC",
    "gbc-games": "GBC",
    "gba-games": "GBA",
    "md-games": "MD",
    "sms-games": "MS",
    "atari2600-games": "ATARI",
    "colecovision-games": "COLECO",
    "c64-games": "COMMODORE",
    "zxspectrum-games": "ZXS",
}
# file extension -> system, where it differs from the repo's
EXT_SYSTEM = {".gb": "GB", ".gg": "GG"}

here = os.path.dirname(os.path.abspath(__file__))
out_json = sys.argv[1]
img_dir = sys.argv[2] if len(sys.argv) > 3 else ""
img_base = sys.argv[3] if len(sys.argv) > 3 else ""


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


def text(game, tag):
    v = game.findtext(tag)
    return (v or "").strip()


entries, seen = [], set()
with tempfile.TemporaryDirectory() as work:
    for repo, system in REPOS.items():
        dest = os.path.join(work, repo)
        url = f"https://github.com/retrobrews/{repo}.git"
        if subprocess.call(["git", "clone", "-q", "--depth", "1", url, dest]) != 0:
            print(f"skip {repo}: clone failed", file=sys.stderr)
            continue
        sha = subprocess.check_output(["git", "-C", dest, "rev-parse", "HEAD"], text=True).strip()
        try:
            root = ET.parse(os.path.join(dest, "gamelist.xml")).getroot()
        except (OSError, ET.ParseError) as e:
            print(f"skip {repo}: {e}", file=sys.stderr)
            continue
        count = 0
        for game in root.iter("game"):
            rel = text(game, "path").lstrip("./")
            path = os.path.join(dest, rel)
            if not rel or "/" in rel or not os.path.isfile(path):
                continue
            name = text(game, "name") or os.path.splitext(rel)[0]
            ext = os.path.splitext(rel)[1].lower()
            sys_ = EXT_SYSTEM.get(ext, system)
            gid = f"game-{slug(sys_)}-{slug(name)}"[:120]
            if gid in seen:
                continue
            seen.add(gid)
            data = open(path, "rb").read()
            date = text(game, "releasedate")[:4]
            genre = text(game, "genre")
            if genre.lower() == name.lower():  # many entries repeat the name here
                genre = ""
            desc = re.sub(r"\s+", " ", text(game, "desc"))
            if len(desc) > 400:
                desc = desc[:397].rsplit(" ", 1)[0] + "..."
            e = {
                "id": gid,
                "type": "game",
                "name": name,
                "author": text(game, "developer"),
                "description": desc,
                "version": date or sha[:7],
                "system": sys_,
                "url": f"https://raw.githubusercontent.com/retrobrews/{repo}/{sha}/{rel}",
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "file": rel,
                "extract": False,  # RetroArch reads zipped ROMs directly
                "source": f"https://github.com/retrobrews/{repo}",
            }
            if genre:
                e["genre"] = genre
            img = text(game, "image").lstrip("./")
            if img_dir and img and os.path.isfile(os.path.join(dest, img)):
                os.makedirs(img_dir, exist_ok=True)
                if subprocess.call([sys.executable, os.path.join(here, "thumb.py"),
                                    os.path.join(dest, img), os.path.join(img_dir, gid + ".png")]) == 0:
                    e["image"] = f"{img_base}/{gid}.png"
            entries.append(e)
            count += 1
        print(f"{repo}: {count} games ({system})", file=sys.stderr)

entries.sort(key=lambda e: (e["system"], e["name"].lower()))
with open(out_json, "w") as f:
    json.dump(entries, f, ensure_ascii=False, indent=1)
print(f"wrote {out_json}: {len(entries)} games", file=sys.stderr)
