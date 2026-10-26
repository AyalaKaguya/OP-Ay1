#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check UI motion timing, aliasing, clipped RGB565 copies and rapid retargets."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/ui-transitions"
    output.mkdir(parents=True, exist_ok=True)
    environment = native_environment()
    compiler = compiler_path(None, environment)
    executable = output / ("ui_transitions.exe" if os.name == "nt" else "ui_transitions")
    sources = [root / "tests/ui_transitions.c", root / "components/app_tracker/tracker_transition.c"]
    include = root / "components/app_tracker"
    if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [str(compiler), "/nologo", "/utf-8", "/std:c11", "/O2", "/W4", "/WX",
                   "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(include), *map(str, sources), "/Fe:" + str(executable)]
    else:
        command = [str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-I" + str(include), *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=output, env=environment, check=True, timeout=90)
    subprocess.run([str(executable)], cwd=output, env=environment, check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
