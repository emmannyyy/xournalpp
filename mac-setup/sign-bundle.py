#!/usr/bin/env python3
"""Sign every Mach-O payload before sealing the StudySzn Marker app."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def is_macho(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    result = subprocess.run(
        ["file", "-b", str(path)], check=False, capture_output=True, text=True
    )
    return result.returncode == 0 and "Mach-O" in result.stdout


def sign(app: Path, identity: str) -> None:
    common = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        common[1:1] = ["--options", "runtime", "--timestamp"]

    binaries = sorted(
        (path for path in app.rglob("*") if is_macho(path)),
        key=lambda path: len(path.parts),
        reverse=True,
    )
    for binary in binaries:
        subprocess.check_call(["strip", "-S", "-x", str(binary)])
        subprocess.check_call([*common, str(binary)])

    subprocess.check_call([*common, str(app)])
    subprocess.check_call(
        ["codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)]
    )
    print(f"Signed {len(binaries)} Mach-O files and sealed {app.name}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"Usage: {sys.argv[0]} <application.app> <identity-or-dash>")
    app = Path(sys.argv[1]).resolve()
    if not app.is_dir():
        raise SystemExit(f"Application bundle not found: {app}")
    sign(app, sys.argv[2])


if __name__ == "__main__":
    main()
