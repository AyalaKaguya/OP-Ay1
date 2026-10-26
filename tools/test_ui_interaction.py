#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile/run pointer-driven tracker/synth controls with the real audio sources.

No device, ESP-IDF, GSP, audio output device or third-party package is required.
--project-dir selects a different source checkout while tests remain local.
Examples: python tools/test_ui_interaction.py
          python tools/test_ui_interaction.py --project-dir ../OP-Ay1 --case mixer
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_ui_renderer import compiler_path, native_environment


ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "components/app_tracker/tracker_model.c",
    "components/app_tracker/tracker_synth.c",
    "components/app_tracker/tracker_graphics.c",
    "components/app_tracker/tracker_pages.c",
    "components/app_tracker/tracker_render.c",
    "components/app_tracker/tracker_font.c",
    "components/tracker_engine/tracker_engine.c",
    "components/tracker_engine/tracker_module.c",
    "components/tracker_engine/tracker_mixer.c",
    "components/app_audio/app_audio_bus.c",
    "components/app_synth/app_synth.c",
)
CASES = ("all", "navigation", "playback", "mixer", "pads", "synth-keys", "synth-controls")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-dir", type=Path, default=ROOT, help="Source checkout to test, read-only")
    parser.add_argument("--cc", help="Native C compiler executable")
    parser.add_argument("--cflag", action="append", default=[], help="Extra compiler argument; repeat as needed")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".codex-runs/ui-interaction")
    parser.add_argument("--fixture", type=Path, help="Defaults to the selected project's main/assets/aryx.s3m")
    parser.add_argument("--case", choices=CASES, default="all")
    args = parser.parse_args()
    try:
        project = args.project_dir.expanduser().resolve(strict=True)
        fixture = (args.fixture or project / "main/assets/aryx.s3m").expanduser().resolve(strict=True)
        env = native_environment()
        compiler = compiler_path(args.cc, env)
        build = args.build_dir.expanduser().resolve()
        build.mkdir(parents=True, exist_ok=True)
        executable = build / ("ui_interaction.exe" if os.name == "nt" else "ui_interaction")
        includes = (
            project / "components/app_tracker", project / "components/tracker_engine/include",
            project / "components/app_audio/include", project / "components/app_synth/include",
        )
        sources = [str(ROOT / "tests/ui_interaction.c"), *(str(project / source) for source in SOURCES)]
        if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [
                str(compiler), "/nologo", "/std:c11", "/W4", "/WX",
                "/experimental:c11atomics", "/D_CRT_SECURE_NO_WARNINGS",
                *("/I" + str(path) for path in includes), *args.cflag, *sources,
                "/Fe:" + str(executable),
            ]
        else:
            command = [
                str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                *("-I" + str(path) for path in includes), *args.cflag, *sources,
                "-lm", "-o", str(executable),
            ]
        print(f"ui_interaction: source {project}; native compiler {compiler}", flush=True)
        subprocess.run(command, cwd=build, env=env, check=True, timeout=120)
        subprocess.run([str(executable), str(fixture), args.case], cwd=build, env=env, check=True, timeout=60)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"ui_interaction: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
