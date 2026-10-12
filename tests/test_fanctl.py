#!/usr/bin/env python3
"""dry-run으로 CLI를 통합 검증하며 커널 장치나 하드웨어에 접근하지 않는다."""

import os
import pathlib
import pty
import queue
import signal
import subprocess
import tempfile
import threading
import time
import tty
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
    def test_session_stats_count_fault_episodes_recovery_and_encoder(self):
        result = subprocess.run(
            [str(CLI), "--dry-run", "--bmp180", "--encoder"],
            input="temp error\ntemp error\ntemp 27\ncw\nccw\nup\nstats\nquit\n",
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        stats = next(
            line for line in result.stdout.splitlines() if line.startswith("STATS ")
        )
        for value in (
            "commands=7",
            "sensor_frames=0",
            "injected_samples=3",
            "sensor_faults=1",
            "sensor_recoveries=1",
            "encoder_cw=1",
            "encoder_ccw=1",
        ):
            self.assertIn(value, stats)
        self.assertIn(
            "LOG_STATS enabled=0 written=0 dropped=0 queued=0 error=0", result.stdout
        )
        self.assertNotIn("STATE ON", result.stdout)

    def test_monitor_is_opt_in_and_off_stops_periodic_output(self):
        with Session() as session:
            session.send("stats\n")
            session.expect("LOG_STATS")
            self.assertFalse(any(line.startswith("MONITOR ") for line in session.lines))
            session.send("monitor 500\n")
            session.expect("MONITOR mode=MANUAL")
            session.expect("MONITOR mode=MANUAL", timeout=2)
            session.send("monitor off\n")
            session.expect("MONITOR disabled")
            start = len(session.lines)
            time.sleep(0.65)
            self.assertFalse(
                any(line.startswith("MONITOR mode=") for line in session.lines[start:])
            )
            result, output, errors = session.finish()
            self.assertEqual(result, 0, errors)

    def test_invalid_monitor_interval_is_rejected(self):
        for value in ("0", "499", "10001", "-1", "abc", "500x"):
            result = subprocess.run(
                [str(CLI), "--dry-run"],
                input=f"monitor {value}\nquit\n",
                capture_output=True,
                text=True,
                timeout=3,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("ERR monitor interval", result.stderr)
            self.assertNotIn("MONITOR mode=", result.stdout)

    def test_monitor_does_not_extend_max_on_or_block_heartbeat(self):
        with Session("--lease-ms", "500", "--max-on-ms", "1200") as session:
            session.send("monitor 500\non\n")
            session.expect("STATE ON")
            session.expect("STATE OFF reason=max_on", timeout=3)
            result, output, errors = session.finish()
            self.assertEqual(result, 0, errors)
            self.assertNotIn("STATE OFF reason=lease", output)
            self.assertIn("MONITOR mode=MANUAL", output)

    def test_stats_exposes_log_and_session_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.log"
            with Session("--log", str(path)) as session:
                session.send("stats\n")
                session.expect("LOG_STATS enabled=1")
                result, output, errors = session.finish()
            self.assertEqual(result, 0, errors)
            self.assertIn(
                "SESSION_STATS commands=2 frames=0 faults=0 recoveries=0",
                path.read_text(),
            )

    def test_async_log_preserves_cli_and_drains_on_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.log"
            path.write_text("existing\n")
            with Session(
                "--bmp180", "--lcd", "--encoder", "--log", str(path)
            ) as session:
                session.send("speed 2\non\ncw\nstatus\noff\n")
                session.expect("CMD off")
                result, output, errors = session.finish()
            self.assertEqual(result, 0, errors)
            self.assertIn("LOG_CLOSED", output)
            self.assertIn("dropped=0 error=0", output)
            text = path.read_text()
            self.assertTrue(text.startswith("existing\n"))
            for event in ("START", "CMD on", "CMD cw", "CMD off", "STOP reason=quit"):
                self.assertIn(event, text)

    def test_log_start_failure_rejects_special_files(self):
        result = subprocess.run(
            [str(CLI), "--dry-run", "--log", "/dev/null"],
            input="on\nquit\n",
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("start event log", result.stderr)
        self.assertNotIn("READY", result.stdout)
        self.assertNotIn("CMD on", result.stdout)

    def test_log_drains_on_signal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.log"
            with Session("--log", str(path)) as session:
                session.send("on\n")
                session.expect("STATE ON")
                session.process.send_signal(signal.SIGTERM)
                result, output, errors = session.finish(command=None)
            self.assertEqual(result, 0, errors)
            self.assertIn("CLOSED state=OFF reason=signal", output)
            self.assertIn("STOP reason=signal", path.read_text())

    def test_invalid_stdin_rejected_before_resource_open(self):
        for redirect in ("exec 0<&-", "exec 0>/dev/null"):
            with self.subTest(redirect=redirect):
                result = subprocess.run(
                    [
                        "bash",
                        "-c",
                        redirect + '; exec "$1" --dry-run',
                        "bash",
                        str(CLI),
                    ],
                    capture_output=True,
                    timeout=3,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"stdin:", result.stderr)
                self.assertNotIn(b"READY", result.stdout)
                self.assertNotIn(b"DRY-RUN", result.stdout)

    def test_raw_pty_enter_cr_and_crlf_commands(self):
        for ending in (b"\r", b"\r\n"):
            with self.subTest(ending=ending):
                master, slave = pty.openpty()
                process = None
                try:
                    tty.setraw(slave)  # Enter가 LF로 자동 변환되지 않는 조건이다.
                    process = subprocess.Popen(
                        [str(CLI), "--dry-run", "--bmp180", "--lcd"],
                        stdin=slave,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                    )
                    data = ending.join(
                        [
                            b"mode manual",
                            b"speed 5",
                            b"on",
                            b"status",
                            b"off",
                            b"quit",
                            b"",
                        ]
                    )
                    os.write(master, data)
                    output, errors = process.communicate(timeout=3)
                    self.assertEqual(process.returncode, 0, errors)
                    self.assertIn(b"INPUT tty=1 canonical=0 icrnl=0", output)
                    self.assertEqual(output.count(b"CMD on\n"), 1)
                    self.assertIn(b"STATE ON", output)
                    self.assertIn(b"SPEED level=5/5 duty=100%", output)
                    self.assertIn(b"CLOSED state=OFF reason=quit", output)
                finally:
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.communicate(timeout=3)
                    os.close(slave)
                    os.close(master)

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
        self.assertIn(b"CMD on\n", result.stdout)
        self.assertIn(b"CMD status\n", result.stdout)

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
            time.sleep(0.9)  # lease보다 길고 max-on보다 짧은 구간을 기다린다.
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
        # SIGSTOP 중에는 dry-run도 실행되지 않는다. resume 시 만료만 검증하며
        # 커널 delayed work의 독립적인 보호 정지를 검증하는 시험은 아니다.
        with Session("--lease-ms", "500", "--max-on-ms", "3000") as session:
            session.send("on\n")
            session.expect("STATE ON")
            session.process.send_signal(signal.SIGSTOP)
            time.sleep(0.8)
            session.send("heartbeat\n")
            session.process.send_signal(signal.SIGCONT)
            session.expect("STATE OFF reason=lease")
            # 주기 검사보다 명령이 먼저 처리될 수도 있으므로 알림 중복 횟수를 가정하지 않는다.

            session.send("status\n")
            session.expect("STATE OFF reason=lease")
            session.send("on\n")
            session.expect("STATE ON")  # 명시적 재기동은 허용한다.
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
        self.assertIn(b"SPEED level=2/5 duty=100%", result.stdout)  # 기동 boost
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

    def test_encoder_zero_stops_and_rotation_does_not_restart(self):
        result = self.run_input(
            b"speed 1\non\nccw\ncw\nstatus\nquit\n", "--encoder", "--lcd"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"STATE ON"), 1)
        self.assertIn(b"SPEED level=0/5 duty=0%", result.stdout)
        self.assertIn(b"SPEED level=1/5 duty=0%", result.stdout)
        self.assertIn(b"STATE OFF reason=user", result.stdout)

    def test_auto_rejects_encoder_simulation_without_changing_target(self):
        result = self.run_input(
            b"temp 27\nmode auto\non\ncw\nccw\nstatus\noff\nquit\n",
            "--encoder",
            "--bmp180",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count(b"ERR manual speed/LED controls"), 2)
        self.assertIn(b"MODE AUTO armed=1 target=2", result.stdout)
        self.assertIn(b"STATE ON", result.stdout)
        self.assertNotIn(b"SPEED level=1/5", result.stdout)
        self.assertNotIn(b"SPEED level=3/5", result.stdout)

    def test_led_test_stays_off_and_speed_restores_auto(self):
        result = self.run_input(
            b"led 1\nstatus\nled 8\nstatus\nspeed 2\nstatus\nled auto\nquit\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"STATE ON", result.stdout)
        self.assertGreaterEqual(result.stdout.count(b"mode=test count=1/8"), 2)
        self.assertGreaterEqual(result.stdout.count(b"mode=test count=8/8"), 2)
        self.assertIn(b"SPEED level=2/5 duty=0%", result.stdout)
        self.assertTrue(
            result.stdout.rstrip().endswith(b"CLOSED state=OFF reason=quit")
        )
        self.assertIn(b"mode=auto count=0/8", result.stdout)

    def test_led_auto_tracks_speed_and_off(self):
        result = self.run_input(
            b"speed 1\non\nspeed 2\nspeed 3\nspeed 4\nspeed 5\noff\nstatus\nquit\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for count in (2, 4, 5, 7, 8):
            self.assertIn(f"mode=auto count={count}/8".encode(), result.stdout)
        after_off = result.stdout.split(b"STATE OFF reason=user", 1)[1]
        self.assertIn(b"mode=auto count=0/8", after_off)

    def test_led_rejects_invalid_counts_and_test_while_running(self):
        result = self.run_input(
            b"led 3\nled 9\nled -1\nled bad\nstatus\non\nled 1\nstatus\noff\nquit\n"
        )
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
            ["--sensor-bus", "/dev/i2c-7"],
            ["--dry-run", "--bmp180", "--sensor-bus", "x" * 128],
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

    def test_auto_requires_explicit_on_and_off_disarms(self):
        result = self.run_input(
            b"temp 27\nmode auto\nstatus\non\noff\ntemp 35\nstatus\nquit\n", "--bmp180"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        before_on = result.stdout.split(b"AUTO RUN:", 1)[0]
        self.assertNotIn(b"STATE ON", before_on)
        self.assertEqual(result.stdout.count(b"STATE ON"), 1)
        self.assertIn(b"MODE AUTO armed=0 target=5", result.stdout)
        self.assertIn(b"SPEED level=5/5 duty=0%", result.stdout)

    def test_auto_sensor_fault_stops_and_recovery_does_not_restart(self):
        result = self.run_input(
            b"temp 27\nmode auto\non\ntemp error\ntemp 32\nstatus\nquit\n", "--bmp180"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"AUTO STOP reason=sensor_fault_or_stale", result.stdout)
        self.assertEqual(result.stdout.count(b"STATE ON"), 1)
        after_fault = result.stdout.split(b"AUTO STOP", 1)[1]
        self.assertNotIn(b"AUTO RUN", after_fault)
        self.assertIn(b"MODE AUTO armed=0 target=5", after_fault)

    def test_auto_hysteresis_and_cold_warm_restart(self):
        result = self.run_input(
            b"temp 27\nmode auto\non\ntemp 25.5\ntemp 24.9\ntemp 22\ntemp 27\noff\nquit\n",
            "--bmp180",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"MODE AUTO armed=1 target=2", result.stdout)
        self.assertIn(b"MODE AUTO armed=1 target=1", result.stdout)
        self.assertIn(b"MODE AUTO armed=1 target=0", result.stdout)
        self.assertEqual(result.stdout.count(b"AUTO RUN:"), 2)

    def test_auto_rejects_manual_override_and_bad_temperatures(self):
        result = self.run_input(
            b"temp nan\ntemp inf\ntemp 86\ntemp -41\ntemp 27\nmode auto\nspeed 5\nup\nled 8\nstatus\nquit\n",
            "--bmp180",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count(b"ERR temp must"), 4)
        self.assertEqual(result.stderr.count(b"ERR manual speed/LED"), 3)
        self.assertNotIn(b"STATE ON", result.stdout)
        result = self.run_input(b"mode auto\non\nquit\n", "--bmp180")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"requires a fresh valid sensor", result.stderr)
        self.assertNotIn(b"STATE ON", result.stdout)

    def test_auto_stale_sensor_stops_with_heartbeats_running(self):
        with Session(
            "--bmp180", "--lease-ms", "500", "--max-on-ms", "10000"
        ) as session:
            session.send("temp 27\nmode auto\non\n")
            session.expect("STATE ON")
            session.expect("AUTO STOP reason=sensor_fault_or_stale", timeout=5)
            session.send("temp 32\nstatus\n")
            session.expect("MODE AUTO armed=0 target=5")
            code, output, errors = session.finish()
            self.assertEqual(code, 0, errors)
            self.assertEqual(output.count("AUTO RUN:"), 1)

    def test_auto_max_on_stops_without_hot_sample_restart(self):
        with Session("--bmp180", "--lease-ms", "500", "--max-on-ms", "1000") as session:
            session.send("temp 27\nmode auto\non\n")
            session.expect("STATE ON")
            session.expect("AUTO STOP reason=driver_limit")
            session.send("temp 35\nstatus\n")
            session.expect("MODE AUTO armed=0 target=5")
            session.send("on\n")
            session.expect("STATE ON")
            code, output, errors = session.finish()
            self.assertEqual(code, 0, errors)
            self.assertEqual(output.count("AUTO RUN:"), 2)

    def test_auto_lcd_temperature_and_fault_display(self):
        with Session("--bmp180", "--lcd") as session:
            session.send("temp 27\nmode auto\non\n")
            session.expect('LCD row1="FAN ON AUTO     " row2="T:27.0C S:2/5   "')
            session.send("temp error\n")
            session.expect('LCD row1="FAN OFF AUTO    " row2="T:ERR S:2/5     "')
            code, _, errors = session.finish()
            self.assertEqual(code, 0, errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
