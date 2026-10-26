#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile USB-MIDI routing/parser checks without a USB device or ESP-IDF."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

from test_ui_renderer import compiler_path, native_environment


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    output = root / ".codex-runs/midi"
    output.mkdir(parents=True, exist_ok=True)
    environment = native_environment()
    compiler = compiler_path(None, environment)
    sources = [
        root / "tests/midi_routing.c",
        root / "components/app_note_router/app_note_router.c",
        root / "components/app_usb_midi/midi_descriptor.c",
        root / "components/app_usb_midi/midi_packets.c",
        root / "components/app_usb_midi/usb_midi_native.c",
        root / "components/app_synth/app_synth.c",
    ]
    includes = [root / "components" / name / "include" for name in
                ("app_note_router", "app_usb_midi", "app_synth")]
    executable = output / ("midi_routing.exe" if os.name == "nt" else "midi_routing")
    if compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [str(compiler), "/nologo", "/utf-8", "/std:c11", "/experimental:c11atomics",
                   "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                   *["/I" + str(p) for p in includes], *map(str, sources), "/Fe:" + str(executable)]
    else:
        command = [str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   *["-I" + str(p) for p in includes], *map(str, sources), "-lm", "-o", str(executable)]
    subprocess.run(command, cwd=output, env=environment, check=True, timeout=90)
    subprocess.run([str(executable)], cwd=output, env=environment, check=True, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
