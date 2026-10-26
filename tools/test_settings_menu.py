#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the production portable menu controller and renderer."""

from pathlib import Path
import os
import subprocess
from test_ui_renderer import native_environment, compiler_path


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/settings-menu"
    output.mkdir(parents=True, exist_ok=True)
    env = native_environment()
    compiler = compiler_path(None, env)
    sources = [
        "tests/settings_menu.c",
        "components/app_tracker/tracker_settings.c",
        "components/app_tracker/tracker_graphics.c",
        "components/app_tracker/tracker_font.c",
        "components/app_settings/app_settings.c",
    ]
    includes = [
        "components/app_tracker",
        "components/app_settings/include",
        "components/app_usb_midi/include",
        "components/app_note_router/include",
        "components/app_synth/include",
        "components/app_tracker_engine/include",
    ]
    exe = output / ("menu.exe" if os.name == "nt" else "menu")
    if compiler.name.lower() in ("cl", "cl.exe"):
        cmd = [
            str(compiler),
            "/nologo",
            "/std:c11",
            "/W4",
            "/WX",
            "/O2",
            "/D_CRT_SECURE_NO_WARNINGS",
            *["/I" + str(root / p) for p in includes],
            *[str(root / p) for p in sources],
            "/Fe:" + str(exe),
        ]
    else:
        cmd = [
            str(compiler),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *["-I" + str(root / p) for p in includes],
            *[str(root / p) for p in sources],
            "-lm",
            "-o",
            str(exe),
        ]
    subprocess.run(cmd, cwd=output, env=env, check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
