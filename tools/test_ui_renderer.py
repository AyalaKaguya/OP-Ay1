#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and run the portable C renderer checks without ESP-IDF or a device.

Run from any directory: python tools/test_ui_renderer.py
Uses native cl on Windows (discovers Visual Studio through vswhere), or cc on
POSIX. Explicit --cc selects a native compiler; repeat --cflag for extra flags.
Generated files stay under .codex-runs/ui-renderer by default. The fixture and
initial visual hashes are checked by tests/ui_renderer.c, never regenerated.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "tests/ui_renderer.c",
    "components/app_tracker/tracker_graphics.c",
    "components/app_tracker/tracker_pages.c",
    "components/app_tracker/tracker_render.c",
    "components/app_tracker/tracker_font.c",
    "components/tracker_engine/tracker_engine.c",
    "components/tracker_engine/tracker_module.c",
    "components/tracker_engine/tracker_mixer.c",
)


def native_environment() -> dict[str, str]:
    """Discover a VS native environment without changing the calling shell."""
    env = {key.upper() if os.name == "nt" else key: value for key, value in os.environ.items()}
    if os.name != "nt" or shutil.which("cl", path=env.get("PATH")):
        return env
    vswhere = Path(env.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    if not vswhere.is_file():
        return env
    result = subprocess.run(
        [
            str(vswhere), "-latest", "-prerelease", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath",
        ],
        check=True, capture_output=True, text=True, timeout=30,
    )
    installations = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not installations:
        return env
    vcvars = Path(installations[0]) / "VC/Auxiliary/Build/vcvarsall.bat"
    if not vcvars.is_file():
        return env
    # vcvars is a trusted VS-owned batch entry. Keep this one batch operation
    # within cmd; file operations and compiler invocations below use argv lists.
    command = f'cmd.exe /d /s /c "\"{vcvars}\" amd64 >nul && set"'
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key.upper()] = value
    return env


def compiler_path(requested: str | None, env: dict[str, str]) -> Path:
    if requested:
        found = shutil.which(requested, path=env.get("PATH"))
        path = Path(found or requested).expanduser().resolve()
        if path.is_file():
            return path
        raise RuntimeError(f"Native compiler not found: {requested}")
    candidates = ("cl", "cc", "gcc") if os.name == "nt" else ("cc", "clang", "gcc")
    for name in candidates:
        found = shutil.which(name, path=env.get("PATH"))
        if found:
            return Path(found).resolve()
    raise RuntimeError("No native C compiler found; activate a host toolchain or pass --cc PATH")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Native compiler executable, not the ESP cross compiler")
    parser.add_argument("--cflag", action="append", default=[], help="Extra compiler argument; repeat as needed")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".codex-runs/ui-renderer")
    parser.add_argument("--fixture", type=Path, default=ROOT / "main/assets/aryx.s3m")
    args = parser.parse_args()
    try:
        env = native_environment()
        compiler = compiler_path(args.cc, env)
        build = args.build_dir.expanduser().resolve()
        fixture = args.fixture.expanduser().resolve(strict=True)
        build.mkdir(parents=True, exist_ok=True)
        executable = build / ("ui_renderer.exe" if os.name == "nt" else "ui_renderer")
        includes = (ROOT / "components/app_tracker", ROOT / "components/tracker_engine/include")
        sources = [str(ROOT / source) for source in SOURCES]
        if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [
                str(compiler), "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                *("/I" + str(path) for path in includes), *args.cflag, *sources,
                "/Fe:" + str(executable),
            ]
        else:
            command = [
                str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                *("-I" + str(path) for path in includes), *args.cflag, *sources,
                "-lm", "-o", str(executable),
            ]
        print(f"ui_renderer: native compiler {compiler}", flush=True)
        subprocess.run(command, cwd=build, env=env, check=True, timeout=120)
        subprocess.run([str(executable), str(fixture)], cwd=build, env=env, check=True, timeout=30)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"ui_renderer: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
