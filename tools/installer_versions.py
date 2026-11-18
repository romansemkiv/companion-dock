#!/usr/bin/env python3
"""Every released full image, for the browser installer's version list (a fresh install can pick an older one).

    python3 tools/installer_versions.py OUTDIR [--repo PATH]

firmware/ holds one release at a time: each release commit replaces the full image and rewrites latest.json. The
older images are still in the history, so this walks the commits that changed firmware/latest.json, newest first,
and writes into OUTDIR (the site's firmware/ folder):

- each release's full image, under its own name (companion-dock-vX.Y.Z-full.bin);
- versions.json: [{version, date, full, full_size, full_sha256}], newest release first. Size and checksum are
  the written image's. A release is listed once, as its newest commit describes it, and left out when its image
  is gone from that commit or doesn't match the checksum its latest.json records.

It needs the whole history: in the Pages workflow the checkout has fetch-depth 0.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True)


def number(version):
    """0.2.10 sorts after 0.2.9."""
    return [int(p) if p.isdigit() else 0 for p in version.split(".")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    args = ap.parse_args()

    log = git(args.repo, "log", "--format=%H", "--", "firmware/latest.json")
    if log.returncode != 0:
        sys.exit(f"installer_versions: git log failed: {log.stderr.decode().strip()}")
    os.makedirs(args.outdir, exist_ok=True)
    versions = {}
    for commit in log.stdout.decode().split():
        try:
            latest = json.loads(git(args.repo, "show", f"{commit}:firmware/latest.json").stdout)
            version, full = latest["version"], os.path.basename(latest["full"])
        except (ValueError, KeyError, TypeError):
            continue
        if version in versions:
            continue
        image = git(args.repo, "show", f"{commit}:firmware/{full}")
        if image.returncode != 0:
            print(f"installer_versions: {version}: no {full} in {commit[:7]}, left out", file=sys.stderr)
            continue
        sha = hashlib.sha256(image.stdout).hexdigest()
        if latest.get("full_sha256") not in (None, sha):
            print(f"installer_versions: {version}: {full} doesn't match its recorded checksum, left out", file=sys.stderr)
            continue
        with open(os.path.join(args.outdir, full), "wb") as f:
            f.write(image.stdout)
        versions[version] = {"version": version, "date": latest.get("date", ""), "full": full,
                             "full_size": len(image.stdout), "full_sha256": sha}
    listed = sorted(versions.values(), key=lambda v: number(v["version"]), reverse=True)
    with open(os.path.join(args.outdir, "versions.json"), "w") as f:
        json.dump(listed, f, indent=2)
        f.write("\n")
    print(f"installer_versions: {len(listed)} releases: {', '.join(v['version'] for v in listed)}")


if __name__ == "__main__":
    main()
