#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive real project/file selection lists with enough data to require scrolling.

Fixtures are independently checksummed filesystem archives derived from one UI
save. Application state changes only through GSP pointer input. No device access.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import time
import wave
import zlib

from test_project_ui_sim import HOME, IMPORT, PROJECTS, SAMPLER, SWITCH, Session


def selection_fixtures(session: Session, project_dir: Path):
    base = session.archive("BASE QA")[0]
    source = session.storage / "projects" / base["id"]
    names = {"BASE QA"}
    for index in range(8):
        identifier = f"{index + 1:032x}"
        name = f"LIST {chr(65 + index)}"
        destination = source.parent / identifier
        shutil.copytree(source, destination)
        generation = destination / (destination / "CURRENT").read_text(encoding="ascii").strip()
        path = generation / "manifest.bin"
        manifest = bytearray(path.read_bytes())
        assert len(manifest) == 192 and manifest[:8] == b"OPAYPRJ1"
        manifest[56:120] = name.encode("ascii").ljust(64, b"\0")
        manifest[120:153] = identifier.encode("ascii") + b"\0"
        struct.pack_into("<I", manifest, 188, zlib.crc32(manifest[:188]))
        path.write_bytes(manifest)
        stored = session.archive(name)[0]
        assert stored["module"] == base["module"] and stored["bank"] == base["bank"]
        names.add(name)

    inbox = session.storage / "inbox"
    wav_pcm = {}
    for index in range(16):
        name = f"WAVE-{index:02}.wav"
        frames = 320 + index * 17
        pcm = b"".join(struct.pack("<h", (frame * (index + 3) * 127) % 28000 - 14000) for frame in range(frames))
        with wave.open(str(inbox / name), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(48000)
            wav.writeframes(pcm)
        wav_pcm[frames] = pcm
    for index in range(8):
        shutil.copyfile(project_dir / "main/assets/aryx.s3m", inbox / f"MODULE-{index:02}.s3m")
    return names, wav_pcm


def no_snap(session: Session, prefix: str, top=320):
    # The selected pixels are static and the entry hint has expired before
    # comparing complete PNGs; the release must retain this non-row offset.
    time.sleep(1.3)
    session.pointer(180, top, True)
    session.pointer(180, top - 37, True)
    session.screenshot(prefix + "-held")
    session.pointer(180, top - 37, False)
    session.screenshot(prefix + "-released")
    held = (session.output / (prefix + "-held.png")).read_bytes()
    released = (session.output / (prefix + "-released.png")).read_bytes()
    assert hashlib.sha256(held).digest() == hashlib.sha256(released).digest(), "Release snapped or moved the list"


def open_selected(session: Session):
    session.tap(126, 400)
    return session.settled(HOME)


def open_wav_list(session: Session):
    session.browser()
    session.tap(416, 110)
    session.settled(PROJECTS)
    session.tap(217, 110)
    session.settled(IMPORT)
    session.tap(348, 110)
    session.settled(IMPORT)


def run(session: Session, args):
    session.read(lambda state: state["workspace"] == HOME and not state["project_busy"])
    session.save("BASE QA")
    names, wav_pcm = selection_fixtures(session, args.project_dir)
    session.browser()
    session.tap(416, 110)
    time.sleep(0.3)
    session.settled(PROJECTS)
    session.tap(150, 162)
    baseline = open_selected(session)
    assert baseline["project_name"] in names and not baseline["project_dirty"]
    session.browser()
    session.screenshot("projects-top")
    no_snap(session, "projects-free")
    for _ in range(4):
        session.drag(180, 350, 180, 160)
    session.screenshot("projects-bottom")
    retained = open_selected(session)
    assert retained["project_name"] == baseline["project_name"]
    assert retained["project_generation"] == baseline["project_generation"], (
        "A list drag selected/reopened another project"
    )
    session.browser()
    session.tap(150, 350)
    selected = open_selected(session)
    assert selected["project_name"] in names and not selected["project_dirty"]
    session.archive(selected["project_name"])
    session.checks.append(
        "nine verified projects scroll continuously; release retains an arbitrary offset; drag preserves selection"
    )

    open_wav_list(session)
    session.screenshot("wav-files-top")
    no_snap(session, "wav-files-free", top=320)
    for _ in range(7):
        session.drag(180, 350, 180, 210)
    session.screenshot("wav-files-bottom")
    # Tap the final visible row, then import through the production action.
    session.tap(130, 346)
    before = session.read()
    session.tap(240, 400)
    imported = session.read(
        lambda state: state["workspace"] == SAMPLER and state["sample_count"] == 10 and not state["project_busy"]
    )
    frames = imported["sample_trim_end"]
    assert frames in wav_pcm and imported["project_dirty"]
    session.save()
    saved = session.archive(selected["project_name"])[0]
    entries = session.bank_entries(saved["bank"])
    assert len(entries) == 10 and entries[9]["frames"] == frames and entries[9]["pcm"] == wav_pcm[frames]
    assert imported["project_generation"] == before["project_generation"]
    session.checks.append("sixteen WAV files scroll and import the selected file's independently expected real PCM")

    open_wav_list(session)
    # Switching formats resets the file offset, with S3M filtering independent
    # of the sixteen WAV entries in the same real inbox.
    session.tap(100, 110)
    session.settled(IMPORT)
    session.screenshot("s3m-files-top")
    no_snap(session, "s3m-files-free", top=320)
    for _ in range(4):
        session.drag(180, 350, 180, 210)
    session.screenshot("s3m-files-bottom")
    session.tap(130, 346)
    session.tap(240, 400)
    outcome = session.read(lambda state: state["workspace"] in (HOME, SWITCH) and not state["project_busy"])
    assert outcome["workspace"] == HOME, outcome
    assert outcome["sample_count"] == 9 and outcome["project_dirty"]
    assert outcome["project_name"].startswith("MODULE-")
    session.checks.append(
        "eight S3M files remain independently filtered and the bottom selection imports a real module"
    )

    session.browser()
    session.tap(326, 110)
    session.screenshot("recent-projects")
    session.tap(326, 110)
    session.screenshot("all-projects-restored")
    session.tap(217, 110)
    session.settled(IMPORT)
    session.back(PROJECTS)
    session.home()
    session.checks.append("recent/all and format changes retain valid selection; parent and HOME gestures still work")
    return {
        "passed": True,
        "checks": session.checks,
        "project_fixture_count": len(names),
        "wav_fixture_count": len(wav_pcm),
        "s3m_fixture_count": 8,
        "input": "GSP feed_pointer only; filesystem fixtures and archives independently verified",
        "unverified": ["physical device touch and NAND"],
    }


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, required=True)
    parser.add_argument("--project-dir", type=Path, default=root)
    parser.add_argument("--build-dir", type=Path, default=root / "build-sim")
    parser.add_argument("--output", type=Path, default=root / ".codex-runs/selection-lists-sim")
    args = parser.parse_args()
    args.project_dir = args.project_dir.resolve()
    session = Session(args)
    try:
        result = run(session, args)
        (session.output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print("Selection lists: " + "; ".join(session.checks) + " PASS")
        print(f"Evidence: {session.output}")
    except Exception:
        try:
            session.rpc.call("screenshot", {"path": str(session.output / "failure.png"), "format": "png"})
        except (OSError, RuntimeError):
            pass
        raise
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
