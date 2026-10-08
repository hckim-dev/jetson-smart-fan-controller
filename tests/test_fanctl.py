#!/usr/bin/env python3
"""CLI integration checks. Always use dry-run; no kernel or hardware access."""

import pathlib
import queue
import signal
import subprocess
import threading
import time
import unittest

CLI = pathlib.Path(__file__).resolve().parents[1] / "build" / "fanctl"


class Session:
    def __init__(self, *options):
        self.process = subprocess.Popen(
            [str(CLI), "--dry-run", *options],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.lines = []
        self.events = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.append(line)
            self.events.put(line)

    def send(self, command):
        self.process.stdin.write(command)
        self.process.stdin.flush()

    def expect(self, substring, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.events.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                break
            if substring in line:
                return line
        raise AssertionError(f"Missing {substring!r}; output: {''.join(self.lines)}")

    def finish(self, command="quit\n"):
        if command:
            self.send(command)
        self.process.stdin.close()
        result = self.process.wait(timeout=3)
        self.reader.join(timeout=1)
        errors = self.process.stderr.read()
        return result, "".join(self.lines), errors

    def __enter__(self):
        self.expect("STATE OFF reason=initial")
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=3)
        self.reader.join(timeout=1)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


class FanctlTests(unittest.TestCase):
    def run_input(self, data, *options):
        return subprocess.run(
            [str(CLI), "--dry-run", *options],
            input=data,
            capture_output=True,
            timeout=3,
        )

    def test_on_off_quit(self):
        result = self.run_input(b"on\nstatus\noff\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"STATE ON", result.stdout)
        self.assertIn(b"STATE OFF reason=user", result.stdout)
        self.assertIn(b"CLOSED state=OFF reason=quit", result.stdout)

    def test_eof_stops(self):
        result = self.run_input(b"on\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"STATE ON", result.stdout)
        self.assertIn(b"CLOSED state=OFF reason=eof", result.stdout)

    def test_unterminated_on_is_not_executed(self):
        result = self.run_input(b"on")
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(b"STATE ON", result.stdout)
        self.assertIn(b"reason=eof", result.stdout)

    def test_rejects_oversized_and_nul_commands_without_prefix_on(self):
        for invalid in (b"on" + b" " * 128, b"on\x00off"):
            with self.subTest(invalid=invalid):
                result = self.run_input(invalid + b"\nstatus\non\noff\nquit\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"ERR command too long or contains NUL", result.stderr)
                self.assertEqual(result.stdout.count(b"STATE ON"), 1)
                self.assertIn(b"STATE OFF reason=user", result.stdout)

    def test_partial_line_does_not_block_heartbeat(self):
        with Session("--lease-ms", "500", "--max-on-ms", "3000") as session:
            session.send("on\n")
            session.expect("STATE ON")
            session.send("sta")
            time.sleep(0.9)  # Longer than the lease, shorter than max-on.
            session.send("tus\n")
            session.expect("STATE ON")
            code, output, errors = session.finish()
            self.assertEqual(code, 0, errors)
            self.assertNotIn("reason=lease", output)

    def test_repeat_on_cannot_extend_maximum_duration(self):
        with Session("--lease-ms", "500", "--max-on-ms", "1500") as session:
            session.send("on\n")
            session.expect("STATE ON")
            time.sleep(0.6)
            session.send("on\n")
            line = session.expect("STATE ON")
            remaining = int(line.split("on_remaining_ms=")[1])
            self.assertLess(remaining, 1100)
            session.expect("STATE OFF reason=max_on", timeout=1.3)
            session.send("heartbeat\n")
            session.expect("STATE OFF reason=max_on")
            code, _, errors = session.finish()
            self.assertEqual(code, 0, errors)

    def test_stopped_process_expires_on_resume_without_automatic_restart(self):
        # The dry-run process cannot run while SIGSTOPped. This only checks
        # expiry on resume, not the kernel driver's independent delayed work.
        with Session("--lease-ms", "500", "--max-on-ms", "3000") as session:
            session.send("on\n")
            session.expect("STATE ON")
            session.process.send_signal(signal.SIGSTOP)
            time.sleep(0.8)
            session.send("heartbeat\n")
            session.process.send_signal(signal.SIGCONT)
            session.expect("STATE OFF reason=lease")
            # Poll may dispatch the queued command before the periodic status
            # check; either order is valid. Ask again instead of counting
            # duplicate notifications from one state transition.
            session.send("status\n")
            session.expect("STATE OFF reason=lease")
            session.send("on\n")
            session.expect("STATE ON")  # Explicit restart is allowed.
            code, _, errors = session.finish()
            self.assertEqual(code, 0, errors)

    def test_signal_exit_during_partial_command(self):
        for signo in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signo=signo), Session() as session:
                session.send("on\n")
                session.expect("STATE ON")
                session.send("sta")
                session.process.send_signal(signo)
                code, output, errors = session.finish(command="")
                self.assertEqual(code, 0, errors)
                self.assertIn("CLOSED state=OFF reason=signal", output)

    def test_speed_selection_off_and_zero_restart(self):
        result = self.run_input(b"speed 2\nstatus\nspeed 0\non\nstatus\noff\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"SPEED level=2/5 duty=0%", result.stdout)
        self.assertIn(b"SPEED level=0/5 duty=0%", result.stdout)
        self.assertIn(b"SPEED level=2/5 duty=100%", result.stdout)  # startup boost
        self.assertIn(b"STATE ON", result.stdout)

    def test_speed_boost_and_maximum_deadline(self):
        with Session("--lease-ms", "500", "--max-on-ms", "1500") as session:
            session.send("speed 1\non\n")
            session.expect("STATE ON")
            session.expect("SPEED level=1/5 duty=100%")
            time.sleep(0.3)
            session.send("status\n")
            session.expect("SPEED level=1/5 duty=60%")
            time.sleep(0.3)
            session.send("speed 3\n")
            line = session.expect("STATE ON")
            self.assertLess(int(line.split("on_remaining_ms=")[1]), 1100)
            session.expect("SPEED level=3/5 duty=80%")
            session.expect("STATE OFF reason=max_on", timeout=1.3)
            session.send("speed 4\n")
            session.expect("STATE OFF reason=max_on")
            session.expect("SPEED level=4/5 duty=0%")
            code, _, errors = session.finish()
            self.assertEqual(code, 0, errors)

    def test_encoder_simulation_clamps_and_does_not_start(self):
        result = self.run_input(
            b"speed 1\nccw\nccw\ncw\ncw\ncw\ncw\ncw\ncw\nquit\n", "--encoder"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"SPEED level=0/5 duty=0%", result.stdout)
        self.assertIn(b"SPEED level=5/5 duty=0%", result.stdout)
        self.assertNotIn(b"STATE ON", result.stdout)
        result = self.run_input(
            b"speed 2\ncw\nquit\n", "--encoder", "--reverse-encoder"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"SPEED level=1/5 duty=0%", result.stdout)

    def test_invalid_speed_does_not_change_selection_or_start(self):
        result = self.run_input(b"speed 6\nspeed -1\nspeed 2extra\nstatus\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count(b"ERR speed"), 3)
        self.assertNotIn(b"STATE ON", result.stdout)
        self.assertIn(b"SPEED level=5/5 duty=0%", result.stdout)

    def test_led_test_stays_off_and_speed_restores_auto(self):
        result = self.run_input(b"led 1\nstatus\nled 8\nstatus\nspeed 2\nstatus\nled auto\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"STATE ON", result.stdout)
        self.assertGreaterEqual(result.stdout.count(b"mode=test count=1/8"), 2)
        self.assertGreaterEqual(result.stdout.count(b"mode=test count=8/8"), 2)
        self.assertIn(b"SPEED level=2/5 duty=0%", result.stdout)
        self.assertTrue(result.stdout.rstrip().endswith(b"CLOSED state=OFF reason=quit"))
        self.assertIn(b"mode=auto count=0/8", result.stdout)

    def test_led_auto_tracks_speed_and_off(self):
        result = self.run_input(b"speed 1\non\nspeed 2\nspeed 3\nspeed 4\nspeed 5\noff\nstatus\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        for count in (2, 4, 5, 7, 8):
            self.assertIn(f"mode=auto count={count}/8".encode(), result.stdout)
        after_off = result.stdout.split(b"STATE OFF reason=user", 1)[1]
        self.assertIn(b"mode=auto count=0/8", after_off)

    def test_led_rejects_invalid_counts_and_test_while_running(self):
        result = self.run_input(b"led 3\nled 9\nled -1\nled bad\nstatus\non\nled 1\nstatus\noff\nquit\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count(b"ERR led must"), 3)
        self.assertIn(b"ERR LED Bar: Device or resource busy", result.stderr)
        self.assertGreaterEqual(result.stdout.count(b"mode=test count=3/8"), 2)
        self.assertIn(b"mode=auto count=8/8", result.stdout)

    def test_invalid_options_are_rejected_before_device_open(self):
        arguments = (
            ["--lease-ms", "500"],
            ["--reverse-encoder"],
            ["--encoder-chip", "/dev/gpiochip0"],
            ["--dry-run", "--encoder", "--encoder-chip", "/dev/gpiochip0"],
            ["--dry-run", "--device", "/dev/smartfan"],
            ["--dry-run", "--lease-ms", "-1"],
            ["--dry-run", "--lease-ms", "500oops"],
            ["--dry-run", "--lease-ms", "1000", "--max-on-ms", "1000extra"],
            ["--dry-run", "--lease-ms", "2000", "--max-on-ms", "1000"],
            ["--lcd"],
            ["--lcd-address", "0x27"],
            ["--dry-run", "--lcd", "--lcd-address", "0x77"],
            ["--dry-run", "--lcd", "--lcd-address", "0x27extra"],
            ["--dry-run", "--lcd", "--lcd-bus", "x" * 128],
        )
        for options in arguments:
            with self.subTest(options=options):
                result = subprocess.run(
                    [str(CLI), *options], capture_output=True, timeout=3
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"DEVICE", result.stdout)
                self.assertNotIn(b"/dev/smartfan:", result.stderr)

    def test_lcd_tracks_led_test_speed_and_running_state(self):
        with Session("--lcd") as session:
            session.send("led 1\n")
            session.expect('LCD row1="FAN OFF MANUAL  " row2="SPEED:5/5 LED:1 "')
            session.send("speed 2\n")
            session.expect('LCD row1="FAN OFF MANUAL  " row2="SPEED:2/5 LED:0 "')
            session.send("on\n")
            session.expect('LCD row1="FAN ON MANUAL   " row2="SPEED:2/5 LED:4 "')
            session.send("off\n")
            session.expect('LCD row1="FAN OFF MANUAL  " row2="SPEED:2/5 LED:0 "')
            code, output, errors = session.finish()
            self.assertEqual(code, 0, errors)
            self.assertIn("CLOSED state=OFF reason=quit", output)

    def test_lcd_reports_max_on_stop_without_user_command(self):
        with Session("--lcd", "--lease-ms", "500", "--max-on-ms", "1000") as session:
            session.send("speed 2\non\n")
            session.expect('LCD row1="FAN ON MANUAL   " row2="SPEED:2/5 LED:4 "')
            session.expect('LCD row1="FAN OFF MANUAL  " row2="SPEED:2/5 LED:0 "')
            code, _, errors = session.finish()
            self.assertEqual(code, 0, errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
