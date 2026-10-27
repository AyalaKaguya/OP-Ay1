#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real UI owner against fake GSP scheduling, without a device."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-dir", type=Path, default=root)
    arguments = parser.parse_args()
    project = arguments.project_dir.resolve()
    output = root / ".codex-runs/ui-owner"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("ui_owner.exe" if os.name == "nt" else "ui_owner")
    source_paths = [
        "components/app_tracker/tracker_model.c",
        "components/app_tracker/tracker_synth.c",
        "components/app_tracker/tracker_settings.c",
        "components/app_tracker/tracker_transition.c",
        "components/app_tracker/tracker_graphics.c",
        "components/app_tracker/tracker_pages.c",
        "components/app_tracker/tracker_render.c",
        "components/app_tracker/tracker_font.c",
        "components/app_tracker/tracker_workspace.c",
        "components/app_tracker/tracker_workspace_render.c",
        "components/app_tracker/tracker_sampler_ui.c",
        "components/app_tracker_engine/tracker_engine.c",
        "components/app_tracker_engine/tracker_module.c",
        "components/app_tracker_engine/tracker_mixer.c",
        "components/app_audio/app_audio_bus.c",
        "components/app_synth/app_synth.c",
        "components/app_note_router/app_note_router.c",
        "components/app_usb_midi/usb_midi_native.c",
        "components/app_settings/app_settings.c",
        "components/app_sampler/app_sampler.c",
        "components/app_sampler/sampler_bank.c",
        "components/app_sampler/sampler_formats.c",
        "components/app_sampler/sampler_record.c",
        "components/app_project/app_project.c",
        "components/app_project/project_platform.c",
        "components/app_project/project_files.c",
        "components/app_project/project_session.c",
    ]
    sources = [root / "tests/ui_owner_lifecycle.c", *[project / source for source in source_paths]]
    includes = [
        project / "components/app_tracker",
        project / "components/app_tracker/include",
        project / "build-sim/op_ay1_backend_assets",
        project / "managed_components/espressif__esp-gsp/include",
        *[
            project / "components" / component / "include"
            for component in (
                "app_tracker_engine",
                "app_audio",
                "app_synth",
                "app_note_router",
                "app_usb_midi",
                "app_settings",
                "app_sampler",
                "app_project",
            )
        ],
    ]
    environment = native_environment()
    compiler = compiler_path(None, environment)
    if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [
            str(compiler),
            "/nologo",
            "/utf-8",
            "/std:c11",
            "/experimental:c11atomics",
            "/O2",
            "/W4",
            "/WX",
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
    subprocess.run(command, cwd=output, env=environment, check=True, timeout=90)
    subprocess.run(
        [str(executable), str(project / "main/assets/aryx.s3m")], cwd=output, env=environment, check=True, timeout=30
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
