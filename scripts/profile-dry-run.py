#!/usr/bin/env python3
"""GPIO/PWM/I²C 접근 없이 재현 가능한 CPU·메모리 표본을 수집한다."""

import json
import pathlib
import resource
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    process = subprocess.Popen(
        [str(ROOT / "build/fanctl"), "--dry-run", "--bmp180", "--lcd"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        process.stdin.write("temp 27\nmode auto\non\n")
        process.stdin.flush()
        for temperature in [27.1, 27.0, 26.9, 27.0, 27.1]:
            time.sleep(1)
            process.stdin.write(f"temp {temperature}\n")
            process.stdin.flush()
        status = pathlib.Path(f"/proc/{process.pid}/status").read_text()
        peak = next(
            int(line.split()[1])
            for line in status.splitlines()
            if line.startswith("VmHWM:")
        )
        process.stdin.write("off\nquit\n")
        process.stdin.flush()
        output, errors = process.communicate(timeout=5)
        if (
            process.returncode
            or "STATE ON" not in output
            or "reason=quit" not in output
        ):
            raise RuntimeError(f"dry-run failed: {output}\n{errors}")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    print(
        json.dumps(
            {
                "scenario": "5s dry-run AUTO + LCD preview, 5 temperature commands; no hardware",
                "wall_s": round(time.monotonic() - started, 4),
                "cpu_user_s": round(after.ru_utime - before.ru_utime, 6),
                "cpu_system_s": round(after.ru_stime - before.ru_stime, 6),
                "rss_high_water_before_shutdown_kib": peak,
                "limitations": "Single sample; includes launch/cleanup CPU. No real I2C/GPIO latency or worst-case timing.",
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
