#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build/run actual project-worker persistence checks in an isolated host directory."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_ui_renderer import compiler_path, native_environment

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "components/app_project/app_project.c",
    "components/app_project/project_platform.c",
    "components/app_project/project_files.c",
    "components/app_project/project_session.c",
    "components/app_tracker_engine/tracker_engine.c",
    "components/app_tracker_engine/tracker_module.c",
    "components/app_tracker_engine/tracker_mixer.c",
    "components/app_synth/app_synth.c",
    "components/app_sampler/app_sampler.c",
    "components/app_sampler/sampler_bank.c",
    "components/app_sampler/sampler_formats.c",
    "components/app_sampler/sampler_record.c",
    "components/app_audio/app_audio_bus.c",
    "components/app_note_router/app_note_router.c",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".codex-runs/project-check")
    parser.add_argument("--fixture", type=Path, default=ROOT / "main/assets/aryx.s3m")
    parser.add_argument("--keep-data", action="store_true", help="Keep the isolated filesystem for inspection")
    args = parser.parse_args()
    try:
        env = native_environment()
        compiler = compiler_path(args.cc, env)
        build = args.build_dir.expanduser().resolve()
        build.mkdir(parents=True, exist_ok=True)
        executable = build / ("project_persistence.exe" if os.name == "nt" else "project_persistence")
        components = ("app_project", "app_tracker_engine", "app_synth", "app_sampler", "app_audio", "app_note_router")
        includes = [ROOT / "tests", *(ROOT / "components" / c / "include" for c in components)]
        sources = [
            str(ROOT / "tests/project_persistence.c"),
            str(ROOT / "tests/project_io_faults.c"),
            *(str(ROOT / path) for path in SOURCES),
        ]
        if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [
                str(compiler),
                "/nologo",
                "/std:c11",
                "/W4",
                "/WX",
                "/experimental:c11atomics",
                "/D_CRT_SECURE_NO_WARNINGS",
                "/DAPP_PROJECT_IO_INTERPOSE=1",
                *("/I" + str(p) for p in includes),
                *sources,
                "/Fe:" + str(executable),
            ]
        else:
            command = [
                str(compiler),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                "-DAPP_PROJECT_IO_INTERPOSE=1",
                *("-I" + str(p) for p in includes),
                *sources,
                "-pthread",
                "-lm",
                "-o",
                str(executable),
            ]
        subprocess.run(command, cwd=build, env=env, check=True, timeout=120)
        temporary = tempfile.mkdtemp(prefix="project-data-", dir=build)
        print(f"project_persistence: isolated filesystem {temporary}", flush=True)
        subprocess.run(
            [str(executable), str(args.fixture.resolve(strict=True)), temporary],
            cwd=build,
            env=env,
            check=True,
            timeout=120,
        )
        if not args.keep_data:
            import shutil

            target = Path(temporary).resolve(strict=True)
            if target.parent != build or not target.name.startswith("project-data-"):
                raise RuntimeError(f"Refusing cleanup outside the test's generated filesystem: {target}")
            shutil.rmtree(target)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"project_persistence: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
