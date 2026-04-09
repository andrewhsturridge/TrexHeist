#!/usr/bin/env python3
"""
TREX camera -> Feather GPIO bridge for Raspberry Pi 5.

What it does:
- Runs rpicam-hello with the built-in motion_detect post-processing stage.
- Watches the motion detector log lines ("Motion detected" / "Motion stopped").
- Drives one Raspberry Pi GPIO as an active-LOW motion output for the Feather.

Electrical assumptions for the simple v1 wiring:
- Pi GPIO17 is wired directly to Feather GPIO5.
- Pi GND is tied to Feather GND.
- Feather pin 5 is configured as INPUT_PULLUP on the Feather side.
- The old PIR output is no longer connected to Feather pin 5.

Output behaviour:
- Idle / no motion  => Pi drives the line HIGH.
- Motion detected   => Pi drives the line LOW.
- Release to HIGH is lightly debounced, and a short LOW hold can be enforced
  so the Feather/ESP32 side reliably sees each motion event.

This intentionally mimics the old active-LOW PIR input so the Feather code can
reuse its existing logic.
"""

from __future__ import annotations

import argparse
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

from gpiozero import OutputDevice

MOTION_DETECTED_RE = re.compile(r"\bMotion detected\b")
MOTION_STOPPED_RE = re.compile(r"\bMotion stopped\b")


class MotionOutput:
    """Active-LOW output wrapper.

    With active_high=False:
      - on()  => physical LOW  (active)
      - off() => physical HIGH (idle)
    """

    def __init__(self, gpio_pin: int) -> None:
        self._dev = OutputDevice(gpio_pin, active_high=False, initial_value=False)
        self._active = False
        self.set_idle("startup")

    @property
    def active(self) -> bool:
        return self._active

    def set_active(self, reason: str) -> None:
        if not self._active:
            self._dev.on()  # physical LOW
            self._active = True
            print(f"[bridge] ACTIVE (LOW)  reason={reason}", flush=True)

    def set_idle(self, reason: str) -> None:
        if self._active or reason == "startup":
            self._dev.off()  # physical HIGH
            self._active = False
            print(f"[bridge] IDLE   (HIGH) reason={reason}", flush=True)

    def close(self) -> None:
        try:
            self.set_idle("shutdown")
        finally:
            self._dev.close()


class CameraBridge:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.output = MotionOutput(args.gpio_pin)
        self.stop_requested = False
        self.child: subprocess.Popen[str] | None = None
        self.motion_state = False
        self.started_at = 0.0
        self.motion_hold_until = 0.0
        self.motion_stopped_at = 0.0

    def build_command(self) -> list[str]:
        cmd = [
            self.args.rpicam_bin,
            "-n",
            "--timeout", "0",
            "--framerate", str(self.args.framerate),
            "--lores-width", str(self.args.lores_width),
            "--lores-height", str(self.args.lores_height),
            "--autofocus-mode", "manual",
            "--lens-position", str(self.args.lens_position),
            "--post-process-file", str(self.args.motion_json),
        ]
        if self.args.camera_index is not None:
            cmd.extend(["--camera", str(self.args.camera_index)])
        return cmd

    def startup_ignore_active(self) -> bool:
        elapsed_ms = (time.monotonic() - self.started_at) * 1000.0
        return elapsed_ms < self.args.startup_ignore_ms

    def handle_motion_detected(self) -> None:
        now = time.monotonic()
        self.motion_state = True
        self.motion_stopped_at = 0.0
        hold_sec = max(0.0, self.args.motion_hold_ms / 1000.0)
        self.motion_hold_until = max(self.motion_hold_until, now + hold_sec)
        if self.startup_ignore_active():
            print("[bridge] Motion detected during startup-ignore window; output held HIGH.", flush=True)
            return
        self.output.set_active("motion_detected")

    def handle_motion_stopped(self) -> None:
        self.motion_state = False
        self.motion_stopped_at = time.monotonic()
        if self.output.active:
            print("[bridge] Motion stopped; waiting for debounce/hold before releasing HIGH.", flush=True)

    def ensure_output_matches_state(self) -> None:
        now = time.monotonic()
        if self.motion_state:
            if not self.output.active and not self.startup_ignore_active():
                self.output.set_active("startup_ignore_elapsed")
            return

        if not self.output.active:
            return

        if now < self.motion_hold_until:
            return

        debounce_sec = max(0.0, self.args.motion_stop_debounce_ms / 1000.0)
        if self.motion_stopped_at > 0.0 and (now - self.motion_stopped_at) < debounce_sec:
            return

        self.output.set_idle("motion_stopped")

    def terminate_child(self) -> None:
        if self.child is None:
            return
        if self.child.poll() is None:
            try:
                self.child.terminate()
                self.child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=5)
            except Exception as exc:
                print(f"[bridge] child terminate warning: {exc}", file=sys.stderr, flush=True)
        self.child = None

    def run_once(self) -> int:
        cmd = self.build_command()
        print("[bridge] launching:", " ".join(cmd), flush=True)
        self.motion_state = False
        self.motion_hold_until = 0.0
        self.motion_stopped_at = 0.0
        self.output.set_idle("camera_start")
        self.started_at = time.monotonic()

        self.child = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        assert self.child.stdout is not None
        stdout = self.child.stdout

        while not self.stop_requested:
            self.ensure_output_matches_state()
            ready, _, _ = select.select([stdout], [], [], 0.25)
            if ready:
                line = stdout.readline()
                if line == "":
                    if self.child.poll() is not None:
                        break
                    continue
                line = line.rstrip("\n")
                print(line, flush=True)

                if MOTION_DETECTED_RE.search(line):
                    self.handle_motion_detected()
                elif MOTION_STOPPED_RE.search(line):
                    self.handle_motion_stopped()
            else:
                if self.child.poll() is not None:
                    break

        rc = self.child.wait() if self.child is not None else 0
        self.output.set_idle(f"camera_exit_rc_{rc}")
        self.child = None
        return rc

    def run_forever(self) -> int:
        while not self.stop_requested:
            rc = self.run_once()
            if self.stop_requested:
                break
            print(f"[bridge] rpicam exited with rc={rc}; restarting in {self.args.restart_delay_sec:.1f}s", flush=True)
            time.sleep(self.args.restart_delay_sec)
        self.output.set_idle("bridge_exit")
        return 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="TREX camera motion -> Feather GPIO bridge")
    parser.add_argument("--gpio-pin", type=int, default=17, help="BCM GPIO pin on the Pi (default: 17)")
    parser.add_argument("--rpicam-bin", default="rpicam-hello", help="Path to rpicam-hello (default: rpicam-hello)")
    parser.add_argument("--motion-json", type=Path, default=Path("/opt/trex-camera/motion_detect.json"), help="motion_detect JSON path")
    parser.add_argument("--lens-position", default="0.0", help="Manual lens position (default: 0.0 / infinity)")
    parser.add_argument("--framerate", type=int, default=15, help="Camera framerate (default: 15)")
    parser.add_argument("--lores-width", type=int, default=960, help="Low-res width for motion_detect (default: 960)")
    parser.add_argument("--lores-height", type=int, default=540, help="Low-res height for motion_detect (default: 540)")
    parser.add_argument("--startup-ignore-ms", type=int, default=3000,
                        help="Ignore motion output for this many ms after launching rpicam (default: 3000)")
    parser.add_argument("--motion-hold-ms", type=int, default=250,
                        help="Keep GPIO LOW for at least this many ms after motion is detected (default: 250)")
    parser.add_argument("--motion-stop-debounce-ms", type=int, default=150,
                        help="Require motion to stay stopped for this many ms before releasing HIGH (default: 150)")
    parser.add_argument("--restart-delay-sec", type=float, default=2.0,
                        help="Delay before restarting rpicam if it exits (default: 2.0)")
    parser.add_argument("--camera-index", type=int, default=None,
                        help="Optional camera index if multiple cameras are attached")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    if not args.motion_json.exists():
        print(f"[bridge] motion JSON not found: {args.motion_json}", file=sys.stderr, flush=True)
        return 2

    bridge = CameraBridge(args)

    def _handle_signal(signum, _frame):
        print(f"[bridge] signal {signum} received; shutting down.", flush=True)
        bridge.stop_requested = True
        bridge.terminate_child()
        bridge.output.set_idle("signal_shutdown")

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    try:
        return bridge.run_forever()
    finally:
        try:
            bridge.terminate_child()
        finally:
            bridge.output.close()


if __name__ == "__main__":
    raise SystemExit(main())
