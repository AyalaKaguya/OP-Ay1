#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the portable source bus and device initialization ownership checks."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/audio"
    output.mkdir(parents=True, exist_ok=True)
    env = native_environment()
    compiler = compiler_path(None, env)
    includes = [
        root / "components" / name / "include"
        for name in ("app_audio", "app_synth", "app_tracker_engine", "app_sampler")
    ]
    bus_sources = [
        root / "tests/audio_bus_test.c",
        root / "components/app_audio/app_audio_bus.c",
        root / "components/app_synth/app_synth.c",
        *[
            root / "components/app_tracker_engine" / name
            for name in ("tracker_engine.c", "tracker_module.c", "tracker_mixer.c")
        ],
        *[
            root / "components/app_sampler" / name
            for name in ("app_sampler.c", "sampler_bank.c", "sampler_formats.c", "sampler_record.c")
        ],
    ]
    init_sources = [root / "tests/audio_init_test.c", root / "components/app_audio/app_audio.c"]
    for name, sources in [("bus", bus_sources), ("init", init_sources)]:
        exe = output / (name + (".exe" if os.name == "nt" else ""))
        test_includes = includes if name == "bus" else [root / "components/app_audio/tests/host_stubs", *includes]
        if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [
                str(compiler),
                "/nologo",
                "/std:c11",
                "/experimental:c11atomics",
                "/W4",
                "/WX",
                "/O2",
                "/D_CRT_SECURE_NO_WARNINGS",
                *["/I" + str(p) for p in test_includes],
                *map(str, sources),
                "/Fe:" + str(exe),
            ]
        else:
            command = [
                str(compiler),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O2",
                *["-I" + str(p) for p in test_includes],
                *map(str, sources),
                "-lm",
                "-o",
                str(exe),
            ]
        subprocess.run(command, cwd=output, env=env, check=True, timeout=120)
        subprocess.run([str(exe)], cwd=output, env=env, check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
