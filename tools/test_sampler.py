#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Strict native sample-bank, import/edit/render and concurrent reload checks."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("--cflag", action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/sampler"
    output.mkdir(parents=True, exist_ok=True)
    env = native_environment()
    compiler = compiler_path(args.cc, env)
    sources = [root / "tests/sampler_native.c", root / "tests/sampler_concurrent.c"]
    for owner, names in {
        "app_sampler": ("app_sampler.c", "sampler_bank.c", "sampler_formats.c", "sampler_record.c"),
        "app_tracker_engine": ("tracker_engine.c", "tracker_module.c", "tracker_mixer.c"),
        "app_audio": ("app_audio_bus.c",),
        "app_synth": ("app_synth.c",),
    }.items():
        sources.extend(root / "components" / owner / name for name in names)
    includes = [root / "components" / name / "include" for name in
                ("app_sampler", "app_tracker_engine", "app_audio", "app_synth")]
    executable = output / ("sampler_native.exe" if os.name == "nt" else "sampler_native")
    if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [str(compiler), "/nologo", "/utf-8", "/std:c11", "/experimental:c11atomics",
                   "/W4", "/WX", "/O2", "/D_CRT_SECURE_NO_WARNINGS",
                   *["/I" + str(path) for path in includes], *args.cflag,
                   *map(str, sources), "/Fe:" + str(executable)]
    else:
        command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                   *["-I" + str(path) for path in includes], *args.cflag,
                   *map(str, sources), "-lm", "-pthread", "-o", str(executable)]
    print(f"sampler: native compiler {compiler}", flush=True)
    subprocess.run(command, cwd=output, env=env, check=True, timeout=120)
    subprocess.run([str(executable), str(root / "main/assets/aryx.s3m")],
                   cwd=output, env=env, check=True, timeout=90)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
