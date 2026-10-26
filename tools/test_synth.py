#!/usr/bin/env python3
"""Build/run portable synth tests; all outputs stay under build-synth/."""
from __future__ import annotations

import argparse
import os
import math
import struct
import wave
from pathlib import Path
import shutil
import subprocess
import sys


def msvc_environment(output: Path) -> tuple[str, dict[str, str]] | None:
    base = Path(os.environ.get("ProgramFiles", "C:/Program Files"))
    candidates = sorted(base.glob("Microsoft Visual Studio/*/*/Common7/Tools/VsDevCmd.bat"))
    for script in reversed(candidates):
        command_file = output / "msvc-env.cmd"
        command_file.write_text(f'@call "{script}" -no_logo -host_arch=x64 -arch=x64 >nul\n@set\n', encoding="utf-8")
        result = subprocess.run(
            ["cmd.exe", "/d", "/c", str(command_file)],
            capture_output=True, text=True, check=False,
        )
        if result.returncode:
            continue
        environment = dict(os.environ)
        for line in result.stdout.splitlines():
            if "=" in line and not line.startswith("="):
                key, value = line.split("=", 1)
                for existing in list(environment):
                    if existing.upper() == key.upper():
                        del environment[existing]
                environment[key] = value
        compiler = shutil.which("cl.exe", path=next((value for key, value in environment.items() if key.upper() == "PATH"), None))
        if compiler:
            return compiler, environment
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="host C compiler path")
    parser.add_argument("--no-wav", action="store_true")
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / "build-synth"
    output.mkdir(exist_ok=True)
    executable = output / ("synth-tests.exe" if os.name == "nt" else "synth-tests")
    sources = [root / "components/app_synth/app_synth.c", root / "tests/synth_engine.c"]
    include = root / "components/app_synth/include"
    environment = dict(os.environ)
    compiler = arguments.cc
    if not compiler and os.name == "nt":
        msvc = msvc_environment(output)
        if msvc:
            compiler, environment = msvc
    compiler = compiler or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        parser.error("No native C compiler found; use --cc or install native build tools.")
    if Path(compiler).name.lower() in {"cl", "cl.exe"}:
        command = [compiler, "/nologo", "/std:c11", "/experimental:c11atomics", "/W4", "/WX",
                   "/O2", "/D_CRT_SECURE_NO_WARNINGS", f"/I{include}",
                   *(str(source) for source in sources), f"/Fo{output}{os.sep}", f"/Fe{executable}"]
    else:
        command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{include}",
                   *(str(source) for source in sources), "-lm", "-pthread", "-o", str(executable)]
    subprocess.run(command, cwd=root, env=environment, check=True, timeout=90)
    test_command = [str(executable)]
    if not arguments.no_wav:
        test_command.append(str(output / "synth-presets.wav"))
    subprocess.run(test_command, cwd=root, env=environment, check=True, timeout=90)
    if not arguments.no_wav:
        with wave.open(str(output / "synth-presets.wav"), "rb") as audio:
            assert (audio.getnchannels(), audio.getframerate(), audio.getnframes()) == (2, 48000, 384000)
            samples = struct.unpack("<768000h", audio.readframes(audio.getnframes()))
        for index, name in enumerate(("INIT", "BASS", "LEAD", "PAD")):
            segment = samples[index * 192000:(index + 1) * 192000]
            rms = math.sqrt(sum(value * value for value in segment) / len(segment)) / 32768.0
            assert rms > 0.005, f"{name} preset WAV is unexpectedly silent"
            print(f"Verified WAV {name}: RMS {rms:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
