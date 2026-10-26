#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the portable gravity/orientation classifier checks without ESP-IDF or hardware."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_ui_renderer import compiler_path, native_environment


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Native C compiler executable")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".codex-runs/settings-orientation")
    args = parser.parse_args()
    try:
        env = native_environment()
        compiler = compiler_path(args.cc, env)
        build = args.build_dir.expanduser().resolve()
        build.mkdir(parents=True, exist_ok=True)
        output = build / ("settings_orientation.exe" if os.name == "nt" else "settings_orientation")
        include = ROOT / "components/app_settings/include"
        sources = [str(ROOT / "tests/settings_orientation.c"), str(ROOT / "components/app_settings/app_settings.c")]
        if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [str(compiler), "/nologo", "/std:c11", "/W4", "/WX", "/I" + str(include),
                       *sources, "/Fe:" + str(output)]
        else:
            command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(include),
                       *sources, "-lm", "-o", str(output)]
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        subprocess.run([str(output)], cwd=build, env=env, check=True, timeout=10)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"settings_orientation: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
