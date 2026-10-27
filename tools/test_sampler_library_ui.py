#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production sample-library scrolling and real PCM thumbnails."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/sampler-library-ui"
    output.mkdir(parents=True, exist_ok=True)
    env = native_environment()
    compiler = compiler_path(None, env)
    sources = [root / "tests/sampler_library_ui.c"]
    for component, files in {
        "app_tracker": ("tracker_sampler_ui.c", "tracker_graphics.c", "tracker_font.c"),
        "app_sampler": ("app_sampler.c", "sampler_bank.c", "sampler_formats.c", "sampler_record.c"),
        "app_tracker_engine": ("tracker_engine.c", "tracker_module.c", "tracker_mixer.c"),
    }.items():
        sources.extend(root / "components" / component / name for name in files)
    includes = [root / "components/app_tracker"]
    includes.extend(
        root / "components" / name / "include"
        for name in (
            "app_sampler",
            "app_project",
            "app_tracker_engine",
            "app_audio",
            "app_synth",
            "app_note_router",
            "app_settings",
            "app_usb_midi",
        )
    )
    executable = output / ("sampler_library_ui.exe" if os.name == "nt" else "sampler_library_ui")
    if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [
            str(compiler),
            "/nologo",
            "/utf-8",
            "/std:c11",
            "/experimental:c11atomics",
            "/W4",
            "/WX",
            "/O2",
            "/D_CRT_SECURE_NO_WARNINGS",
            *["/I" + str(path) for path in includes],
            *map(str, sources),
            "/Fe:" + str(executable),
        ]
    else:
        command = [
            str(compiler),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O2",
            *["-I" + str(path) for path in includes],
            *map(str, sources),
            "-lm",
            "-pthread",
            "-o",
            str(executable),
        ]
    subprocess.run(command, cwd=output, env=env, check=True, timeout=90)
    subprocess.run([str(executable), str(root / "main/assets/aryx.s3m")], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
