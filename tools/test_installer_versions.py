#!/usr/bin/env python3
"""Tests of tools/installer_versions.py, run by tools/test-host.sh: python3 -B tools/test_installer_versions.py

A throwaway git repository gets three releases the way this one gets them: each release commit replaces the full
image in firmware/ and rewrites firmware/latest.json. The script must bring every release's image back out of the
history, next to a versions.json that lists them newest first with their real sizes and checksums.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SCRIPT = os.path.join(ROOT, "tools", "installer_versions.py")
fails = []


def check(ok, what):
    if not ok:
        fails.append(what)


def git(repo, *args):
    subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                    *args], check=True, capture_output=True)


def release(repo, version, date, image, recorded_sha=None):
    """One release commit: the previous full image goes, this one and its latest.json come."""
    fw = os.path.join(repo, "firmware")
    os.makedirs(fw, exist_ok=True)
    for name in os.listdir(fw):
        if name.endswith("-full.bin"):
            os.remove(os.path.join(fw, name))
    full = f"companion-dock-v{version}-full.bin"
    with open(os.path.join(fw, full), "wb") as f:
        f.write(image)
    with open(os.path.join(fw, "latest.json"), "w") as f:
        json.dump({"version": version, "date": date, "full": full, "full_size": len(image),
                   "full_sha256": recorded_sha or hashlib.sha256(image).hexdigest()}, f)
    git(repo, "add", "-A")
    git(repo, "commit", "-m", f"Add the {version} firmware files")


IMAGES = {"0.2.9": b"nine" * 100, "0.2.10": b"ten" * 300, "0.3.0": b"three-oh" * 50}

with tempfile.TemporaryDirectory() as tmp:
    repo, out = os.path.join(tmp, "repo"), os.path.join(tmp, "site", "firmware")
    os.makedirs(repo)
    git(repo, "init", "-q")
    release(repo, "0.2.9", "2026-01-01", IMAGES["0.2.9"])
    release(repo, "0.2.10", "2026-02-01", IMAGES["0.2.10"])
    # a commit that only touches latest.json again (a corrected date) must not list the release twice
    with open(os.path.join(repo, "firmware", "latest.json")) as f:
        fixed = json.load(f)
    fixed["date"] = "2026-02-02"
    with open(os.path.join(repo, "firmware", "latest.json"), "w") as f:
        json.dump(fixed, f)
    git(repo, "commit", "-am", "Correct the date")
    release(repo, "0.3.0", "2026-03-01", IMAGES["0.3.0"])

    run = subprocess.run([sys.executable, "-B", SCRIPT, out, "--repo", repo], capture_output=True, text=True)
    check(run.returncode == 0, f"the script failed: {run.stderr}")
    listed = []
    if run.returncode == 0:
        with open(os.path.join(out, "versions.json")) as f:
            listed = json.load(f)
    check([v.get("version") for v in listed] == ["0.3.0", "0.2.10", "0.2.9"],
          f"every release once, newest first by number (0.2.10 is after 0.2.9): {[v.get('version') for v in listed]}")
    for v in listed:
        want = IMAGES.get(v["version"], b"")
        path = os.path.join(out, v["full"])
        got = open(path, "rb").read() if os.path.exists(path) else None
        check(got == want, f"{v['version']}: its full image is brought back out of the history")
        check(v.get("full_size") == len(want), f"{v['version']}: the listed size is the image's")
        check(v.get("full_sha256") == hashlib.sha256(want).hexdigest(), f"{v['version']}: the listed checksum is the image's")
    check(next((v.get("date") for v in listed if v["version"] == "0.2.10"), None) == "2026-02-02",
          "a release's entry comes from the newest commit that describes it")

    # an image that doesn't match the checksum its own latest.json records is not offered
    bad_repo, bad_out = os.path.join(tmp, "bad"), os.path.join(tmp, "badsite")
    os.makedirs(bad_repo)
    git(bad_repo, "init", "-q")
    release(bad_repo, "0.1.0", "2026-01-01", b"damaged", recorded_sha="0" * 64)
    release(bad_repo, "0.1.1", "2026-01-02", b"good")
    run = subprocess.run([sys.executable, "-B", SCRIPT, bad_out, "--repo", bad_repo], capture_output=True, text=True)
    check(run.returncode == 0, f"the script failed on the second repository: {run.stderr}")
    if run.returncode == 0:
        with open(os.path.join(bad_out, "versions.json")) as f:
            check([v["version"] for v in json.load(f)] == ["0.1.1"], "a release whose image fails its recorded checksum is left out")
        check(not os.path.exists(os.path.join(bad_out, "companion-dock-v0.1.0-full.bin")), "and its image is not written")

if fails:
    for f in fails:
        print("FAIL", f)
    sys.exit(1)
print("test_installer_versions: every release comes back out of the history, newest first, with its real size and checksum")
