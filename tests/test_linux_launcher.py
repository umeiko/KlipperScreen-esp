"""Offline process-lifecycle tests; no display, GPU, sudo or printer required."""
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
FAKE_CHILD = r'''#!/usr/bin/env python3
import os, pathlib, signal, socket, sys, time
root = pathlib.Path(os.environ["TEST_ROOT"])
kind = "weston" if pathlib.Path(sys.argv[0]).name == "weston" else "app"
mode = os.environ.get("TEST_" + kind.upper(), "alive")
(root / (kind + ".pid")).write_text(str(os.getpid()))
def terminate(sig, frame):
    (root / (kind + ".term")).write_text(str(sig))
    if mode != "ignore-term":
        sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
if mode == "early-exit":
    sys.exit(42)
if kind == "weston" and mode != "no-socket":
    sock = socket.socket(socket.AF_UNIX)
    sock.bind(os.environ["XDG_RUNTIME_DIR"] + "/" + os.environ["WAYLAND_DISPLAY"])
    sock.listen(1)
if kind == "app" and mode == "exit":
    sys.exit(0)
if kind == "weston" and mode == "crash":
    while not (root / "app.pid").exists():
        time.sleep(0.01)
    sys.exit(42)
while True:
    time.sleep(0.05)
'''


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux process/socket semantics required")
class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kr-launcher-")
        self.root = Path(self.temp.name)
        self.install = self.root / "install"
        (self.install / "bin").mkdir(parents=True)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.commands = self.root / "commands"
        self.commands.mkdir()
        self.launcher = self.install / "KlipperScreen-esp-start.sh"
        shutil.copyfile(REPO / "scripts/linux/KlipperScreen-esp-start.sh", self.launcher)
        for path in (self.commands / "weston", self.install / "bin/KlipperScreen-esp"):
            path.write_text(FAKE_CHILD)
            path.chmod(0o755)
        self.env = dict(os.environ, HOME=str(self.root), TEST_ROOT=str(self.root),
                        XDG_RUNTIME_DIR=str(self.runtime), BACKEND="W",
                        PATH=str(self.commands) + os.pathsep + os.environ["PATH"])
        self.process = None

    def start(self, weston="alive", app="alive"):
        self.env.update(TEST_WESTON=weston, TEST_APP=app)
        self.process = subprocess.Popen(["bash", str(self.launcher)], env=self.env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, start_new_session=True)

    def finish(self, expected):
        output, _ = self.process.communicate(timeout=15)
        self.assertEqual(self.process.returncode, expected, output)
        # Give the init/subreaper time to reap any child killed at final cleanup.
        deadline = time.monotonic() + 2
        while self.children_alive() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(self.children_alive(), output)
        return output

    def children_alive(self):
        alive = []
        for marker in self.root.glob("*.pid"):
            pid = int(marker.read_text())
            try:
                state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
                if state != "Z":
                    alive.append(pid)
            except FileNotFoundError:
                pass
        return alive

    def wait_app(self):
        deadline = time.monotonic() + 5
        while not (self.root / "app.pid").exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue((self.root / "app.pid").exists())

    def tearDown(self):
        if self.process and self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.communicate(timeout=3)
        for pid in self.children_alive():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.temp.cleanup()

    def test_weston_crash_stops_live_app(self):
        self.start(weston="crash")
        self.assertIn("child exited (status=42)", self.finish(42))
        self.assertTrue((self.root / "app.term").exists())

    def test_app_exit_stops_weston(self):
        self.start(app="exit")
        self.finish(1)
        self.assertTrue((self.root / "weston.term").exists())

    def test_early_weston_exit_does_not_start_app(self):
        self.start(weston="early-exit")
        self.assertIn("before becoming ready", self.finish(1))
        self.assertFalse((self.root / "app.pid").exists())

    def test_socket_timeout_does_not_start_app(self):
        self.start(weston="no-socket")
        self.assertIn("readiness timed out", self.finish(1))
        self.assertFalse((self.root / "app.pid").exists())

    def test_service_stop_cleans_both_without_touching_other_socket(self):
        with socket.socket(socket.AF_UNIX) as other:
            other.bind(str(self.runtime / "wayland-0"))
            self.start()
            self.wait_app()
            self.process.send_signal(signal.SIGTERM)
            self.finish(143)
            self.assertTrue((self.runtime / "wayland-0").is_socket())
            self.assertTrue((self.root / "weston.term").exists())
            self.assertTrue((self.root / "app.term").exists())

    def test_stubborn_child_is_killed_after_grace_period(self):
        self.start(weston="ignore-term", app="exit")
        self.finish(1)
        self.assertTrue((self.root / "weston.term").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
