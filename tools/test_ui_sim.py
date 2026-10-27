#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real native UI through the simulator's pointer API.

Build components/app_tracker/host first. Only input, read-only state logs and
captures are used; the test never changes application state through RPC.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import wave


class Rpc:
    def __init__(self, endpoint: str):
        host, port = endpoint.removeprefix("tcp://").rsplit(":", 1)
        self.socket = socket.create_connection((host, int(port)), timeout=10)
        self.stream = self.socket.makefile("rwb", buffering=0)
        self.sequence = 0

    def call(self, method: str, params: dict | None = None):
        self.sequence += 1
        wire = json.dumps({"jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params or {}}).encode()
        self.stream.write(f"Content-Length: {len(wire)}\r\n\r\n".encode() + wire)
        while True:
            headers = {}
            while (line := self.stream.readline()) not in (b"\r\n", b"\n"):
                if not line:
                    raise RuntimeError("Simulator API disconnected")
                key, value = line.decode().split(":", 1)
                headers[key.lower()] = value.strip()
            length = int(headers["content-length"])
            data = bytearray()
            while len(data) < length:
                piece = self.stream.read(length - len(data))
                if not piece:
                    raise RuntimeError("Truncated simulator reply")
                data.extend(piece)
            reply = json.loads(data)
            if reply.get("id") != self.sequence:
                continue
            if "error" in reply:
                raise RuntimeError(f"{method}: {reply['error']}")
            return reply.get("result")

    def close(self):
        self.stream.close()
        self.socket.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, required=True)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--build-dir", type=Path, default=root / "build-sim")
    parser.add_argument("--project-dir", type=Path, default=root)
    parser.add_argument("--output", type=Path, default=root / ".codex-runs/ui-sim")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build = args.build_dir.resolve()
    ready = output / f"ready-{time.time_ns()}.json"
    state_log = output / "state.ndjson"
    settings_log = output / "settings.ndjson"
    capture = output / "performance.wav"
    stop_file = output / "stop-backend"
    state_log.unlink(missing_ok=True)
    settings_log.unlink(missing_ok=True)
    capture.unlink(missing_ok=True)
    stop_file.unlink(missing_ok=True)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    env = dict(os.environ)
    env["OP_AY1_SIM_STATE_LOG"] = str(state_log)
    env["OP_AY1_SIM_SETTINGS_LOG"] = str(settings_log)
    env["OP_AY1_SIM_WAV"] = str(capture)
    env["OP_AY1_SIM_AUDIO"] = "0"
    env["OP_AY1_SIM_STOP_FILE"] = str(stop_file)
    # Legacy module regressions now enter through the real HOME/project flow.
    # A fresh owned root keeps this run independent of saved user projects.
    env["OP_AY1_PROJECT_ROOT"] = tempfile.mkdtemp(prefix="project-data-", dir=output)
    results = []
    backend = None
    rpc = None
    with (output / "sim.log").open("w", encoding="utf-8") as sim_log, (output / "backend.log").open(
        "w", encoding="utf-8"
    ) as backend_log:
        sim = subprocess.Popen(
            [
                str(args.sim.resolve()),
                "--bundle",
                str(build / "op_ay1_backend_assets/app.gspb"),
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
            cwd=args.project_dir.resolve(),
            stdout=sim_log,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
        try:
            deadline = time.monotonic() + 30
            while not ready.exists():
                if sim.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"Simulator did not become ready; see {output / 'sim.log'}")
                time.sleep(0.05)
            info = json.loads(ready.read_text(encoding="utf-8"))
            backend = subprocess.Popen(
                [
                    str(build / ("op_ay1_backend.exe" if os.name == "nt" else "op_ay1_backend")),
                    "--endpoint",
                    info["backend"],
                ],
                cwd=args.project_dir.resolve(),
                env=env,
                stdout=backend_log,
                stderr=subprocess.STDOUT,
                creationflags=flags,
            )
            rpc = Rpc(info["api"])

            def state(predicate=lambda s: True, timeout=8):
                end = time.monotonic() + timeout
                last = None
                while time.monotonic() < end:
                    if backend.poll() is not None:
                        raise RuntimeError(f"Backend exited: {backend.returncode}; see backend.log")
                    if state_log.exists():
                        for line in reversed(state_log.read_text(encoding="utf-8").splitlines()):
                            try:
                                last = json.loads(line)
                            except json.JSONDecodeError:
                                continue
                            break
                        if last is not None and predicate(last):
                            assert last["error"] == 0, last
                            return last
                    time.sleep(0.03)
                raise AssertionError(f"Expected UI state not reached; last={last}")

            def tap(x, y):
                y = legacy_y(y)
                rpc.call("feed_pointer", {"x": x, "y": y, "pressed": True})
                time.sleep(0.06)
                rpc.call("feed_pointer", {"x": x, "y": y, "pressed": False})
                time.sleep(0.25)

            def settings(predicate=lambda s: True, timeout=5):
                deadline = time.monotonic() + timeout
                last = None
                while time.monotonic() < deadline:
                    if settings_log.exists():
                        for line in reversed(settings_log.read_text(encoding="utf-8").splitlines()):
                            try:
                                last = json.loads(line)
                            except json.JSONDecodeError:
                                continue
                            break
                        if last is not None and predicate(last):
                            return last
                    time.sleep(0.03)
                raise AssertionError(f"Expected settings state not reached; last={last}")

            def pointer(x, y, pressed):
                y = legacy_y(y)
                rpc.call("feed_pointer", {"x": x, "y": y, "pressed": pressed})
                time.sleep(0.12)

            def drag(x1, y1, x2, y2):
                pointer(x1, y1, True)
                for step in range(1, 7):
                    pointer(round(x1 + (x2 - x1) * step / 6), round(y1 + (y2 - y1) * step / 6), True)
                pointer(x2, y2, False)

            def page(index):
                tap(200 + index * 16, 460)
                return state(lambda s: s["page"] == index and not s["page_motion"])

            def screenshot(name):
                time.sleep(0.2)
                rpc.call("screenshot", {"path": str(output / (name + ".png")), "format": "png"})

            def legacy_y(y):
                # Existing regression coordinates describe the original page
                # band. The product now fits it below the global project strip.
                current = state()
                opened = settings()["open"]
                return 78 + (y - 40) * 354 // 392 if current["workspace"] == 11 and not opened and 40 <= y < 432 else y

            state(lambda s: s["workspace"] == 0 and not s["project_busy"])
            tap(100, 176)  # HOME / OPEN PROJECT
            tap(80, 110)  # NEW
            tap(240, 296)  # built-in ARYX import
            if state()["workspace"] == 6:
                tap(240, 334)  # discard the initial unsaved demo session
            state(
                lambda s: s["workspace"] == 0 and s["project_opened"] and not s["project_busy"] and not s["page_motion"]
            )
            tap(120, 330)  # HOME / PLAYBACK
            state(lambda s: s["workspace"] == 11 and not s["page_motion"])
            page(1)
            for i in range(6):
                page(i)
                screenshot(f"page-{i}")
            results.append("six page navigation and native RGB565 captures")

            page(2)
            tap(340, 250)
            state(lambda s: s["playing"])
            screenshot("playback-playing")
            tap(120, 250)
            state(lambda s: not s["playing"] and s["launched"] == -1)
            results.append("playback start and restart")

            page(3)
            drag(278, 205, 86, 205)
            state(lambda s: s["gain"][0] == 0 and s["page"] == 3)
            drag(86, 205, 278, 205)
            state(lambda s: 98 <= s["gain"][0] <= 100)
            tap(414, 196)
            state(lambda s: s["solo"][0] == 1 and s["mute"][1] == 1)
            screenshot("mixer-solo")
            tap(414, 196)
            state(lambda s: not any(s["solo"]) and not any(s["mute"]))
            drag(456, 390, 456, 190)
            state(lambda s: s["scroll"] >= 132 and s["page"] == 3)
            screenshot("mixer-scrolled")
            results.append("gain drag capture, solo cascade and vertical mixer scroll")

            page(4)
            tap(60, 140)
            state(lambda s: s["playing"] and s["launched"] == 0)
            tap(60, 140)
            state(lambda s: s["stop"])
            screenshot("pads-stop")
            tap(60, 140)
            state(lambda s: not s["stop"])
            tap(170, 140)
            state(lambda s: s["pending"] == 1)
            screenshot("pads-queued")
            results.append("pad launch from pause, stop queue/cancel and next queue")
            page(2)
            tap(120, 250)
            state(lambda s: not s["playing"])

            page(5)
            pointer(56, 419, True)
            state(lambda s: s["note"] == 60 and s["voices"] > 0 and s["peak"] > 0.001)
            screenshot("synth-held")
            pointer(109, 419, True)
            state(lambda s: s["note"] == 62 and s["page"] == 5)
            pointer(109, 419, False)
            state(lambda s: s["note"] == -1 and s["voices"] == 0, timeout=4)
            results.append("real press, glissando, release and audible PCM while tracker paused")

            for tab in range(4):
                tap(79 + tab * 107, 123)
                state(lambda s: s["tab"] == tab)
                screenshot(f"synth-tab-{tab}")
            tap(79, 123)
            pointer(56, 419, True)
            state(lambda s: s["note"] == 60)
            pointer(56, 300, True)
            state(lambda s: s["note"] == -1)
            pointer(56, 300, False)
            state(lambda s: s["voices"] == 0)
            results.append("leaving keyboard bounds releases note")
            pointer(56, 419, True)
            state(lambda s: s["voices"] > 0)
            pointer(56, 419, False)
            tap(398, 294)
            state(lambda s: s["voices"] == 0 and s["note"] == -1)
            page(1)
            results.append("panic and return navigation")

            # The drawer uses production pointer arbitration on every page.
            for index in range(6):
                page(index)
                drag(220, 12, 225, 90)
                settings(lambda s: s["open"] == 1)
                tap(248, 460)  # Dismiss outside; the page nav must not receive it.
                settings(lambda s: s["open"] == 0)
                state(lambda s: s["page"] == index)
            results.append("global top swipe on all six pages and no dismissal passthrough")
            page(5)
            pointer(56, 419, True)
            state(lambda s: s["voices"] > 0)
            pointer(56, 20, True)
            pointer(56, 20, False)
            settings(lambda s: s["open"] == 0)
            state(lambda s: s["note"] == -1 and s["voices"] == 0)
            results.append("page drag crossing top edge retains its release owner")
            page(2)
            tap(340, 250)
            before_menu = state(lambda s: s["playing"])
            drag(220, 12, 225, 90)
            settings(lambda s: s["open"] == 1)
            drag(100, 127, 447, 127)
            settings(lambda s: s["brightness"] == 100)
            drag(447, 127, 32, 127)
            settings(lambda s: s["brightness"] == 5)
            tap(316, 127)
            settings(lambda s: 69 <= s["brightness"] <= 71)
            for angle in range(4):
                tap(79 + angle * 107, 265)
                settings(lambda s, angle=angle: s["rotation"] == angle * 90 and s["locked"] == 1)
            tap(370, 212)
            settings(lambda s: s["locked"] == 0)
            screenshot("settings-auto")
            tap(79, 265)
            settings(lambda s: s["rotation"] == 0 and s["locked"] == 1)
            current = state(lambda s: s["playing"] and s["frames"] > before_menu["frames"])
            assert current["page"] == 2
            results.append("brightness endpoints and four angle/lock requests while audio continues")
            tap(130, 372)
            settings(lambda s: s["confirm"] == 1)
            screenshot("settings-midi-confirm")
            tap(130, 270)
            settings(lambda s: s["confirm"] == 0 and s["usb_state"] == 0)
            tap(130, 372)
            tap(330, 270)
            settings(lambda s: s["confirm"] == 0 and s["usb_mode"] == 0 and s["usb_error"] == 0x106)
            screenshot("settings-native-usb-unavailable")
            tap(350, 372)
            settings(lambda s: s["usb_state"] == 0 and s["usb_error"] == 0)
            screenshot("settings-vendor")
            drag(250, 380, 250, 270)
            settings(lambda s: s["open"] == 0)
            state(lambda s: not s["menu_motion"])
            tap(120, 250)
            state(lambda s: not s["playing"])
            results.append("USB confirmation cancel, truthful native unsupported error and Vendor retry")

            page(1)
            rpc.call("feed_pointer", {"x": 280, "y": 460, "pressed": True})
            rpc.call("feed_pointer", {"x": 280, "y": 460, "pressed": False})
            state(lambda s: s["page"] == 5 and s["page_motion"] == 1)
            rpc.call("screenshot", {"path": str(output / "page-slide.png"), "format": "png"})
            # A second navigation target is accepted while the old slide runs.
            rpc.call("feed_pointer", {"x": 232, "y": 460, "pressed": True})
            rpc.call("feed_pointer", {"x": 232, "y": 460, "pressed": False})
            state(lambda s: s["page"] == 2 and s["page_motion"] == 1)
            state(lambda s: s["page"] == 2 and s["page_motion"] == 0)
            results.append("page slide intermediate frames and rapid navigation retarget")

            drag(220, 12, 225, 90)
            settings(lambda s: s["open"] == 1)
            tap(240, 169)
            settings(lambda s: s["timeouts"] == 1 and s["dim_s"] == 0 and s["off_s"] == 0)
            screenshot("display-timeouts-default-off")
            tap(240, 155)
            settings(lambda s: s["dim_s"] == 15 and s["off_s"] == 0)
            tap(240, 290)
            settings(lambda s: s["dim_s"] == 15 and s["off_s"] == 30)
            screenshot("display-timeouts-configured")
            tap(80, 20)
            tap(440, 20)
            settings(lambda s: s["open"] == 0)
            tap(340, 250)
            playing = state(lambda s: s["playing"])
            page(5)
            settings(
                lambda s: s["dimmed"] == 1 and s["screen_off"] == 0 and s["brightness"] == 70 and s["effective"] == 10,
                timeout=19,
            )
            screenshot("idle-dimmed")
            assert state()["frames"] > playing["frames"]
            # A dimmed screen restores brightness and accepts that same action.
            pointer(56, 419, True)
            state(lambda s: s["note"] == 60 and s["voices"] > 0)
            pointer(56, 419, False)
            settings(lambda s: s["dimmed"] == 0 and s["effective"] == 70)
            state(lambda s: s["note"] == -1 and s["voices"] == 0)
            settings(lambda s: s["screen_off"] == 1 and s["effective"] == 0, timeout=34)
            screenshot("idle-screen-off")
            audio_while_off = state(lambda s: s["playing"])
            assert audio_while_off["frames"] > playing["frames"]
            # The complete first gesture wakes without playing the synth.
            pointer(56, 419, True)
            pointer(109, 419, True)
            pointer(109, 419, False)
            settings(lambda s: s["screen_off"] == 0 and s["effective"] == 70)
            state(lambda s: s["note"] == -1 and s["voices"] == 0)
            screenshot("idle-awake")
            pointer(56, 419, True)
            state(lambda s: s["note"] == 60 and s["voices"] > 0)
            pointer(56, 419, False)
            state(lambda s: s["note"] == -1 and s["voices"] == 0)
            drag(220, 12, 225, 90)
            tap(240, 169)
            tap(97, 155)
            tap(97, 290)
            settings(lambda s: s["dim_s"] == 0 and s["off_s"] == 0)
            tap(80, 20)
            tap(440, 20)
            page(2)
            tap(120, 250)
            state(lambda s: not s["playing"])
            results.append("idle dim/off with continuing audio, restored preference and whole wake gesture swallowed")

            stop_file.touch()
            backend.wait(timeout=10)
            assert backend.returncode == 0, "Native backend did not close cleanly"
            rpc.call("quit")
            sim.wait(timeout=10)
            assert sim.returncode == 0, "Simulator did not close cleanly"
            with wave.open(str(capture), "rb") as wav:
                assert wav.getframerate() == 48000 and wav.getnchannels() == 2
                frames = wav.getnframes()
                pcm = wav.readframes(frames)
                assert frames > 48000 and any(pcm), "Captured output is empty"
            (output / "result.json").write_text(
                json.dumps(
                    {
                        "passed": True,
                        "checks": results,
                        "wav_frames": frames,
                        "sim_exit": sim.returncode,
                        "backend_exit": backend.returncode,
                        "focus_validation": "pointer release and out-of-bounds tested; browser blur/device delivery pending",
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            print("Native simulator checks passed: " + "; ".join(results))
            print(f"Evidence: {output}")
        finally:
            if rpc:
                rpc.close()
            for process in (backend, sim):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
