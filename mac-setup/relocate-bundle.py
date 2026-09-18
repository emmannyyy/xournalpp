#!/usr/bin/env python3
"""Rewrite bundled Mach-O dependencies so the app runs without Homebrew."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


SYSTEM_PREFIXES = ("/System/", "/usr/lib/")
FORBIDDEN_PREFIXES = ("/opt/homebrew/", "/Users/")


def output(*args: str | Path) -> str:
    return subprocess.check_output([str(arg) for arg in args], text=True)


def is_macho(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    try:
        return "Mach-O" in output("file", "-b", path)
    except subprocess.CalledProcessError:
        return False


def dependencies(path: Path) -> list[str]:
    lines = output("otool", "-L", path).splitlines()[1:]
    return [line.strip().split(" (", 1)[0] for line in lines if line.strip()]


def bundled_target(resources: Path, dependency: str) -> Path | None:
    dep = Path(dependency)
    if dependency.startswith("/opt/homebrew/Cellar/"):
        relative = dep.relative_to("/opt/homebrew/Cellar")
        parts = relative.parts
        if len(parts) >= 3:
            candidate = resources / "opt" / parts[0] / Path(*parts[2:])
            if candidate.exists():
                return candidate.resolve()
    if dependency.startswith("/opt/homebrew/"):
        candidate = resources / dep.relative_to("/opt/homebrew")
        if candidate.exists():
            return candidate.resolve()

    matches = [path for path in resources.rglob(dep.name) if path.is_file()]
    if not matches:
        return None
    return min(matches, key=lambda path: (len(path.parts), len(str(path)))).resolve()


def loader_reference(binary: Path, target: Path) -> str:
    relative = os.path.relpath(target, binary.parent)
    return f"@loader_path/{relative}"


def relocate(app: Path) -> None:
    resources = app / "Contents" / "Resources"
    binaries = [path for path in app.rglob("*") if is_macho(path)]
    if not binaries:
        raise RuntimeError(f"No Mach-O files found in {app}")

    for binary in binaries:
        binary.chmod(binary.stat().st_mode | 0o200)
        for dependency in dependencies(binary):
            if dependency.startswith(SYSTEM_PREFIXES) or dependency.startswith(("@", "/System/")):
                continue
            if not dependency.startswith("/"):
                continue
            target = bundled_target(resources, dependency)
            if target is None:
                raise RuntimeError(f"No bundled copy for {dependency} required by {binary}")
            subprocess.check_call(
                ["install_name_tool", "-change", dependency, loader_reference(binary, target), str(binary)]
            )

        install_names = output("otool", "-D", binary).splitlines()[1:]
        if install_names:
            subprocess.check_call(
                ["install_name_tool", "-id", f"@loader_path/{binary.name}", str(binary)]
            )

    leaks: list[str] = []
    for binary in binaries:
        for dependency in dependencies(binary):
            if dependency.startswith(FORBIDDEN_PREFIXES):
                leaks.append(f"{binary}: {dependency}")
    if leaks:
        raise RuntimeError("Non-relocatable dependencies remain:\n" + "\n".join(leaks))

    print(f"Relocated and verified {len(binaries)} Mach-O files")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"Usage: {sys.argv[0]} <application.app>")
    app = Path(sys.argv[1]).resolve()
    if not app.is_dir():
        raise SystemExit(f"Application bundle not found: {app}")
    relocate(app)


if __name__ == "__main__":
    main()
