#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the production HOME/project/sampler UI through native GSP pointers.

Uses an isolated real filesystem root and verifies committed project resources.
No RPC mutates application state; only feed_pointer and screenshots are used.
Build the native backend first. Microphone/hardware behavior is not simulated.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import wave
import zlib

from test_ui_sim import Rpc

HOME, PROJECTS, NEW, IMPORT, NAME, SAVE, SWITCH, ERROR = range(8)
STORAGE, SETTINGS, SAMPLER, LEGACY = 8, 9, 10, 11


class Session:
    def __init__(self, args):
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.storage = Path(tempfile.mkdtemp(prefix="project-data-", dir=self.output))
        self.state_path = self.output / "state.ndjson"
        self.settings_path = self.output / "settings.ndjson"
        self.stop = self.output / "stop-backend"
        for path in (self.state_path, self.settings_path, self.stop):
            path.unlink(missing_ok=True)
        self.logs = [(self.output / name).open("w", encoding="utf-8") for name in ("sim.log", "backend.log")]
        self.backend = None
        self.rpc = None
        ready = self.output / f"ready-{time.time_ns()}.json"
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.sim = subprocess.Popen(
            [
                str(args.sim.resolve()),
                "--bundle",
                str(args.build_dir.resolve() / "op_ay1_backend_assets/app.gspb"),
                "--frames",
                "0",
                "--headless",
                "--backend-required",
                "--backend-listen",
                "tcp://127.0.0.1:0",
                "--api-listen",
                "tcp://127.0.0.1:0",
                "--ready-file",
                str(ready),
            ],
            cwd=args.project_dir,
            stdout=self.logs[0],
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
        deadline = time.monotonic() + 30
        while not ready.exists():
            if self.sim.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Simulator failed to become ready; see sim.log")
            time.sleep(0.05)
        info = json.loads(ready.read_text(encoding="utf-8"))
        env = dict(
            os.environ,
            OP_AY1_SIM_STATE_LOG=str(self.state_path),
            OP_AY1_SIM_SETTINGS_LOG=str(self.settings_path),
            OP_AY1_SIM_AUDIO="0",
            OP_AY1_SIM_STOP_FILE=str(self.stop),
            OP_AY1_PROJECT_ROOT=str(self.storage),
        )
        self.backend = subprocess.Popen(
            [
                str(args.build_dir.resolve() / ("op_ay1_backend.exe" if os.name == "nt" else "op_ay1_backend")),
                "--endpoint",
                info["backend"],
            ],
            cwd=args.project_dir,
            env=env,
            stdout=self.logs[1],
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
        self.rpc = Rpc(info["api"])
        self.checks = []

    def read(self, predicate=lambda s: True, timeout=8, settings=False):
        path = self.settings_path if settings else self.state_path
        deadline, last = time.monotonic() + timeout, None
        while time.monotonic() < deadline:
            if self.backend.poll() is not None:
                raise RuntimeError(f"Backend exited {self.backend.returncode}; see backend.log")
            if path.exists():
                for line in reversed(path.read_text(encoding="utf-8").splitlines()):
                    try:
                        last = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    break
                if last is not None and predicate(last):
                    if not settings:
                        assert last["error"] == 0, last
                    return last
            time.sleep(0.025)
        raise AssertionError(f"Expected state not reached; last={last}")

    def settled(self, screen=None):
        return self.read(
            lambda s: not s["project_busy"] and not s["page_motion"] and (screen is None or s["workspace"] == screen)
        )

    def pointer(self, x, y, pressed, pause=0.06):
        self.rpc.call("feed_pointer", {"x": x, "y": y, "pressed": pressed})
        time.sleep(pause)

    def tap(self, x, y, settle=True):
        self.pointer(x, y, True)
        self.pointer(x, y, False)
        if settle:
            self.settled()

    def drag(self, x1, y1, x2, y2):
        self.pointer(x1, y1, True)
        for step in range(1, 7):
            self.pointer(round(x1 + (x2 - x1) * step / 6), round(y1 + (y2 - y1) * step / 6), True)
        self.pointer(x2, y2, False)
        self.settled()

    def screenshot(self, name):
        self.settled()
        self.rpc.call("screenshot", {"path": str(self.output / (name + ".png")), "format": "png"})

    def home(self, page=0):
        current = self.settled()
        if current["workspace"] != HOME:
            self.drag(240, 460, 240, 350)
        current = self.settled(HOME)
        for _ in range(3):
            if current["home_page"] == page:
                return current
            self.drag(400, 460, 80, 460)
            current = self.settled(HOME)
        raise AssertionError(f"HOME swipe failed to reach page {page}: {current}")

    def module(self, index):
        self.home(index // 4)
        self.tap(352 if index % 2 else 128, 334 if index % 4 >= 2 else 176)
        return self.settled()

    def browser(self):
        self.tap(185, 58)
        return self.settled(PROJECTS)

    def new(self, aryx=False):
        self.browser()
        self.tap(80, 110)
        self.settled(NEW)
        self.tap(240, 296 if aryx else 180)
        if self.settled()["workspace"] == SWITCH:
            self.tap(240, 334)
        return self.settled(HOME)

    def keyboard(self, name):
        self.settled(NAME)
        for _ in range(24):
            self.tap(392, 308, settle=False)
        rows = ["QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"]
        for char in name:
            if char == " ":
                self.tap(225, 356, settle=False)
                continue
            row = next(i for i, text in enumerate(rows) if char in text)
            self.tap(43 + rows[row].index(char) * 43, 215 + row * 47, settle=False)
        self.tap(351, 399)
        return self.read(lambda s: s["project_name"] == name and not s["project_busy"] and not s["project_dirty"])

    def save(self, name=None):
        self.tap(414, 58)
        current = self.settled()
        if name is not None:
            return self.keyboard(name)
        if current["workspace"] == SAVE:
            self.tap(352, 316)
        return self.read(lambda s: not s["project_busy"] and not s["project_dirty"])

    def legacy(self, page):
        current = self.settled()
        if current["workspace"] != LEGACY:
            current = self.module(2)
        for _ in range(6):
            if current["page"] == page:
                return current
            forward = current["page"] < page
            self.drag(400 if forward else 80, 460, 80 if forward else 400, 460)
            current = self.settled(LEGACY)
        raise AssertionError(f"Tracker swipe failed to reach page {page}: {current}")

    @staticmethod
    def legacy_y(y):
        return 78 + (y - 40) * 354 // 392

    def legacy_tap(self, x, y):
        self.tap(x, self.legacy_y(y))

    def legacy_drag(self, x1, y1, x2, y2):
        self.drag(x1, self.legacy_y(y1), x2, self.legacy_y(y2))

    def other_project(self):
        # No catalog ordering assumption: opening the current ID returns HOME
        # unchanged, while the other ID produces the unsaved switch dialog.
        before = self.read()
        for row in range(2):
            self.browser()
            self.tap(150, 162 + row * 50)
            self.tap(126, 400)
            current = self.settled()
            if current["workspace"] == SWITCH or current["project_generation"] != before["project_generation"]:
                return current
        raise AssertionError("No other saved project found")

    def archive(self, name):
        matches = []
        for project in (self.storage / "projects").iterdir():
            current = project / "CURRENT"
            if not current.is_file():
                continue
            version = project / current.read_text(encoding="ascii").strip()
            manifest = (version / "manifest.bin").read_bytes()
            assert len(manifest) == 192 and manifest[:8] == b"OPAYPRJ1"
            assert zlib.crc32(manifest[:188]) == struct.unpack_from("<I", manifest, 188)[0]
            stored_name = manifest[56:120].split(b"\0", 1)[0].decode("utf-8")
            if stored_name != name:
                continue
            module, bank, state = [
                (version / item).read_bytes() for item in ("module.s3m", "samples.bank", "state.bin")
            ]
            assert len(module) == struct.unpack_from("<Q", manifest, 28)[0]
            assert len(bank) == struct.unpack_from("<Q", manifest, 36)[0]
            for data, offset in ((module, 44), (bank, 48), (state, 52)):
                assert zlib.crc32(data) == struct.unpack_from("<I", manifest, offset)[0]
            matches.append(
                {"id": project.name, "version": version.name, "module": module, "bank": bank, "state": state}
            )
        assert matches, name
        return matches

    @staticmethod
    def bank_entries(bank):
        assert bank[:4] == b"AYSB" and struct.unpack_from("<I", bank, 4)[0] == 1
        count = struct.unpack_from("<I", bank, 8)[0]
        assert struct.unpack_from("<I", bank, 12)[0] == len(bank)
        cursor, entries = 16, []
        for _ in range(count):
            fields = struct.unpack_from("<14I", bank, cursor + 64)
            frames, assigned = fields[1], fields[11]
            cursor += 120
            entries.append({"frames": frames, "assigned_slot": assigned, "pcm": bank[cursor : cursor + frames * 2]})
            cursor += frames * 2
        assert cursor == len(bank)
        return entries

    def close(self):
        if self.backend and self.backend.poll() is None:
            self.stop.touch()
            try:
                self.backend.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.backend.terminate()
                self.backend.wait(timeout=5)
        if self.rpc:
            try:
                self.rpc.call("quit")
            except (RuntimeError, OSError):
                pass
            self.rpc.close()
        if self.sim.poll() is None:
            try:
                self.sim.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.sim.terminate()
                self.sim.wait(timeout=5)
        for log in self.logs:
            log.close()


def run(s: Session, args):
    initial = s.read(lambda v: v["workspace"] == HOME and not v["project_busy"])
    assert initial["project_opened"] and initial["project_dirty"] and initial["project_name"] == "ARYX DEMO"
    s.screenshot("home-initial-demo")
    s.module(1)
    s.settled(SETTINGS)
    s.screenshot("settings-global")
    s.tap(35, 12)
    s.tap(64, 458)
    assert s.settled()["workspace"] == SETTINGS, "Removed HOME buttons must be inert"
    s.tap(440, 90)
    s.drag(250, 380, 250, 270)
    assert s.settled()["workspace"] == SETTINGS, "Embedded settings dismissal must not return HOME"
    s.drag(240, 460, 240, 410)
    assert s.settled()["workspace"] == SETTINGS, "A short upward drag must not navigate"
    s.drag(240, 460, 300, 380)
    assert s.settled()["workspace"] == SETTINGS, "A diagonal drag must not navigate"
    s.tap(400, 20)
    s.settled(STORAGE)
    s.screenshot("storage")
    s.home()
    for page in range(3):
        s.home(page)
        s.screenshot(f"home-{page + 1}")
    s.tap(352, 176)  # empty HOME page 3 slot
    assert s.settled()["workspace"] == HOME
    s.drag(400, 460, 80, 460)
    assert s.settled()["home_page"] == 0, "HOME must wrap forward"
    s.drag(80, 460, 400, 460)
    assert s.settled()["home_page"] == 2, "HOME must wrap backward"
    s.checks.append("three ordered HOME pages, inert empty slots, real initial ARYX project and global settings access")
    s.checks.append(
        "bottom-edge up returns HOME; former HOME taps, short and diagonal drags are inert; HOME swipes wrap"
    )

    blank = s.new()
    assert blank["project_name"] == "UNTITLED" and blank["project_dirty"] and blank["sample_trim_end"] == 0
    s.save("BLANK A")
    blank_archive = s.archive("BLANK A")[0]
    s.checks.append("new blank, real touch name keyboard and first complete self-contained save")

    aryx = s.new(aryx=True)
    assert aryx["project_name"] == "ARYX IMPORT" and aryx["sample_count"] == 9 and aryx["project_dirty"]
    s.save("SONG A")
    song = s.archive("SONG A")[0]
    assert song["id"] != blank_archive["id"]
    assert song["module"] == (args.project_dir / "main/assets/aryx.s3m").read_bytes()
    original_generation = s.read()["project_generation"]
    expected = {0: PROJECTS, 1: SETTINGS, 2: LEGACY, 3: LEGACY, 4: LEGACY, 5: LEGACY, 6: SAMPLER, 7: LEGACY, 8: LEGACY}
    pages = {2: 2, 3: 3, 4: 4, 5: 5, 7: 1, 8: 0}
    for index in range(9):
        current = s.module(index)
        assert current["workspace"] == expected[index]
        if index in pages:
            assert current["page"] == pages[index]
        assert current["project_generation"] == original_generation and current["project_name"] == "SONG A"
        assert not current["project_dirty"]
    s.checks.append("ARYX import preserves raw module and nine slots; all nine HOME entries share one clean session")

    s.legacy(2)
    s.legacy_tap(340, 250)
    s.read(lambda v: v["playing"])
    s.module(5)
    s.read(lambda v: v["playing"] and not v["project_dirty"])
    s.home()
    s.read(lambda v: v["playing"])
    s.legacy(2)
    s.legacy_tap(120, 250)
    s.read(lambda v: not v["playing"] and not v["project_dirty"])
    s.checks.append("HOME/module navigation retains playback; transport and navigation remain clean")

    s.legacy(3)
    s.legacy_drag(278, 205, 120, 205)
    changed = s.read(lambda v: v["project_dirty"] and v["gain"][0] < 40)
    s.browser()
    s.tap(326, 110)  # verified current-launch RECENT list
    s.tap(130, 162)
    s.tap(126, 400)
    same = s.settled(HOME)
    assert same["project_name"] == "SONG A" and same["project_dirty"] and same["gain"] == changed["gain"]
    assert same["project_generation"] == changed["project_generation"]
    s.browser()
    s.tap(326, 110)  # back to all verified projects
    s.other_project()
    s.settled(SWITCH)
    s.screenshot("dirty-switch")
    s.tap(240, 394)  # cancel
    s.read(
        lambda v: v["project_dirty"]
        and v["project_name"] == "SONG A"
        and v["project_generation"] == changed["project_generation"]
    )
    s.other_project()
    s.tap(240, 274)  # save + switch
    switched = s.read(lambda v: v["workspace"] == HOME and not v["project_busy"] and v["project_name"] == "BLANK A")
    assert not switched["project_dirty"]
    assert s.archive("SONG A")[0]["version"] != song["version"]
    s.checks.append("dirty switch cancel retains session; save+switch commits the edited project before replacement")

    s.legacy(5)
    s.legacy_tap(438, 67)  # next synth preset marks blank dirty
    s.read(lambda v: v["project_dirty"])
    s.other_project()
    s.settled(SWITCH)
    s.tap(240, 334)  # discard + switch
    s.read(lambda v: v["workspace"] == HOME and v["project_name"] == "SONG A" and not v["project_dirty"])
    assert s.archive("BLANK A")[0]["version"] == blank_archive["version"]
    s.checks.append("discard+switch preserves the previously saved blank project")

    s.module(6)
    original = s.read(lambda v: v["sample_count"] == 9)
    source_entries = s.bank_entries(song["bank"])
    assert len(source_entries) == 9 and sum(entry["frames"] > 0 for entry in source_entries) == 8
    s.screenshot("sample-library")
    s.tap(235, 402)
    s.read(lambda v: v["sample_edit_active"])
    s.drag(25, 220, 100, 220)
    s.tap(402, 323)
    s.tap(262, 355)  # C2 speed +100 Hz
    s.tap(440, 355)  # gain +5 percent
    draft = s.read(lambda v: v["sample_edit_dirty"] and v["sample_trim_start"] > 0)
    assert not draft["project_dirty"]
    assert draft["sample_loop_enabled"] != original["sample_loop_enabled"]
    assert draft["sample_c2"] == original["sample_c2"] + 100 and draft["sample_gain"] > original["sample_gain"]
    s.screenshot("sample-edit-draft")
    s.tap(90, 402)
    cancelled = s.read(lambda v: not v["sample_edit_active"])
    assert cancelled["sample_trim_start"] == original["sample_trim_start"] and not cancelled["project_dirty"]
    assert cancelled["sample_c2"] == original["sample_c2"] and cancelled["sample_gain"] == original["sample_gain"]
    s.tap(235, 402)
    s.drag(25, 220, 100, 220)
    s.tap(402, 323)
    s.tap(262, 355)
    s.tap(440, 355)
    applied_trim = s.read()["sample_trim_start"]
    s.tap(386, 402)
    applied = s.read(lambda v: not v["sample_edit_active"] and v["project_dirty"])
    assert applied["sample_trim_start"] == applied_trim and applied_trim > 0
    assert applied["sample_c2"] == draft["sample_c2"] and applied["sample_gain"] == draft["sample_gain"]
    assert applied["sample_loop_enabled"] == draft["sample_loop_enabled"]
    s.tap(403, 148)  # one-round undo
    undone = s.read(lambda v: v["sample_trim_start"] == original["sample_trim_start"] and not v["project_busy"])
    assert undone["project_dirty"]
    assert undone["sample_c2"] == original["sample_c2"] and undone["sample_gain"] == original["sample_gain"]
    s.save()
    s.checks.append("sample preview is clean; Cancel restores; Apply commits trim/loop; Undo restores original")

    inbox = s.storage / "inbox"
    fixture = inbox / "fixture.wav"
    pcm = b"".join(struct.pack("<h", round(math.sin(i / 11) * 12000)) for i in range(480))
    with wave.open(str(fixture), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(48000)
        wav.writeframes(pcm)
    s.browser()
    s.tap(416, 110)  # real catalog/inbox refresh
    s.tap(217, 110)
    s.tap(348, 110)
    s.tap(130, 212)
    s.tap(240, 400)
    imported = s.read(lambda v: v["workspace"] == SAMPLER and v["sample_count"] == 10 and not v["project_busy"])
    assert imported["sample_index"] == 9 and imported["sample_trim_end"] == 480 and imported["project_dirty"]
    s.tap(387, 402)  # explicit ASSIGN view
    s.read(lambda v: v["sampler_tab"] == 3)
    s.tap(46, 230)  # choose tracker slot 1
    s.tap(352, 402)
    s.read(lambda v: v["sampler_tab"] == 0 and not v["project_busy"])
    s.save()
    persisted = s.archive("SONG A")[0]
    assert pcm in persisted["bank"], "Imported PCM must be inside the committed project bank"
    imported_entries = s.bank_entries(persisted["bank"])
    assert imported_entries[9]["assigned_slot"] == 1 and imported_entries[9]["pcm"] == pcm
    assert sum(entry["assigned_slot"] == 1 for entry in imported_entries) == 1
    assert persisted["module"] == song["module"], "Assignment overlay preserves original S3M/pattern/effect bytes"
    s.checks.append(
        "WAV inbox imports real PCM; explicit slot assignment persists without rewriting the original module"
    )

    fixture.write_bytes(b"RIFF corrupt input")
    before = s.read()
    s.browser()
    s.tap(416, 110)
    s.tap(217, 110)
    s.tap(348, 110)
    s.tap(130, 212)
    s.tap(240, 400)
    failed = s.settled(ERROR)
    assert (
        failed["project_name"] == before["project_name"]
        and failed["project_generation"] == before["project_generation"]
    )
    assert failed["sample_count"] == 10 and not failed["project_dirty"]
    assert s.archive("SONG A")[0]["bank"] == persisted["bank"]
    s.screenshot("invalid-wav-retains-project")
    s.checks.append("invalid WAV reports failure and keeps project, sample bank, generation and saved state")

    s.module(6)
    s.tap(386, 110)
    s.tap(240, 402)
    unsupported = s.settled(ERROR)
    assert unsupported["record_state"] == 5 and unsupported["record_frames"] == 0
    assert unsupported["sample_count"] == 10 and not unsupported["project_dirty"]
    s.screenshot("native-record-unsupported")
    s.checks.append("native REC reports unsupported without fabricated frames, sample or successful KEEP")

    s.module(0)
    s.tap(352, 400)  # Save As from project browser
    s.keyboard("SONG A")  # same name makes a new ID, never overwrites
    copies = s.archive("SONG A")
    assert len(copies) == 2 and copies[0]["id"] != copies[1]["id"]
    s.checks.append("Save As with an existing name creates a distinct verified project ID")
    s.screenshot("save-as-copy")
    observed = [
        json.loads(line) for line in s.state_path.read_text(encoding="utf-8").splitlines() if line.endswith("}")
    ]
    assert any(v["project_busy"] and v["workspace"] in (NAME, SAVE, IMPORT, SAMPLER) for v in observed)
    generations = [v["project_generation"] for v in observed]
    assert generations == sorted(generations)
    s.checks.append("async save/import busy states are observed and session generations never regress")
    return {
        "passed": True,
        "checks": s.checks,
        "storage": str(s.storage),
        "input": "GSP feed_pointer only; state logs and archives are read-only observations",
        "unverified": ["device microphone/capture", "physical touch/USB/NAND", "power-loss durability"],
    }


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, required=True)
    parser.add_argument("--project-dir", type=Path, default=root)
    parser.add_argument("--build-dir", type=Path, default=root / "build-sim")
    parser.add_argument("--output", type=Path, default=root / ".codex-runs/project-ui-sim")
    args = parser.parse_args()
    args.project_dir = args.project_dir.resolve()
    session = Session(args)
    try:
        result = run(session, args)
        (session.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Native project UI: " + "; ".join(session.checks) + " PASS")
        print(f"Evidence: {session.output}")
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
