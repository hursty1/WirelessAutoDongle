#!/usr/bin/env python3
"""Copy the working tree to Linux storage, restoring Git symlinks and modes."""

import os
from pathlib import Path
import subprocess
import sys


SOURCE = Path("/src")
DESTINATION = Path("/app")


def restore_git_metadata(repository):
    """Windows Git can represent symlinks as files containing the link target."""
    checkout = SOURCE / repository
    entries = subprocess.check_output([
        "git", "-c", f"safe.directory={checkout}", "-C", str(checkout),
        "ls-files", "--stage", "-z",
    ])
    links = 0
    for entry in entries.split(b"\0"):
        if not entry:
            continue
        metadata, filename = entry.split(b"\t", 1)
        mode, _, stage = metadata.split()
        if stage != b"0":
            raise RuntimeError(f"Resolve Git conflicts before building: {checkout}")
        relative = Path(os.fsdecode(filename))
        source = checkout / relative
        destination = DESTINATION / repository / relative
        if mode == b"160000" or not os.path.lexists(destination):
            continue
        if mode == b"120000":
            target = (os.readlink(source) if source.is_symlink()
                      else source.read_text().rstrip("\r\n"))
            if destination.is_symlink() and os.readlink(destination) == target:
                continue
            destination.unlink()
            destination.symlink_to(target)
            links += 1
        elif not destination.is_symlink():
            destination.chmod(0o755 if mode == b"100755" else 0o644)
    return links


def main():
    # Exclusions also protect the existing nested volumes from --delete.
    print("Synchronizing sources into the Linux volume...", flush=True)
    subprocess.run([
        "rsync", "-a", "--delete", "--no-owner", "--no-group",
        "--exclude=/.git", "--exclude=/buildroot/.git",
        "--exclude=/buildroot/dl", "--exclude=/buildroot/output",
        "--exclude=/images", str(SOURCE) + "/", str(DESTINATION) + "/",
    ], check=True)
    links = restore_git_metadata(Path("."))
    links += restore_git_metadata(Path("buildroot"))
    print(f"Synchronized Linux source tree; restored {links} symlinks.", flush=True)
    os.chdir(DESTINATION / "buildroot")
    os.execvp(sys.argv[1], sys.argv[1:])


if __name__ == "__main__":
    main()
