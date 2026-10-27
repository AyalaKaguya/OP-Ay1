#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production workspace parent navigation without GSP or a device."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

from test_ui_interaction import SOURCES
from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    out = root / ".codex-runs/workspace-back"
    out.mkdir(parents=True, exist_ok=True)
    source_paths = [
        *SOURCES,
        "components/app_tracker/tracker_workspace.c",
        "components/app_tracker/tracker_sampler_ui.c",
        "components/app_tracker/tracker_settings.c",
        "components/app_settings/app_settings.c",
        "components/app_project/app_project.c",
        "components/app_project/project_platform.c",
        "components/app_project/project_files.c",
        "components/app_project/project_session.c",
    ]
    sources = [root / "tests/workspace_back.c", *[root / name for name in source_paths]]
    includes = [root / "components/app_tracker"]
    includes += [
        root / "components" / name / "include"
        for name in (
            "app_tracker_engine",
            "app_audio",
            "app_synth",
            "app_sampler",
            "app_note_router",
            "app_project",
            "app_settings",
            "app_usb_midi",
        )
    ]
    env = native_environment()
    compiler = compiler_path(None, env)
    executable = out / ("workspace_back.exe" if os.name == "nt" else "workspace_back")
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
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            *["-I" + str(path) for path in includes],
            *map(str, sources),
            "-lm",
            "-pthread",
            "-o",
            str(executable),
        ]
    subprocess.run(command, cwd=out, env=env, check=True, timeout=120)
    subprocess.run([str(executable), str(root / "main/assets/aryx.s3m")], cwd=out, env=env, check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
