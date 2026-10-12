#!/usr/bin/env python3
"""실제 작업 공간 대신 임시 프로젝트에서 산출물 정리를 검증한다."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BuildTests(unittest.TestCase):
    def test_clean_preserves_logs_notes_and_boot_proposals(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            shutil.copyfile(ROOT / "Makefile", project / "Makefile")
            kernel = project / "fake-kernel"
            kernel.mkdir()
            (kernel / "Makefile").write_text(".PHONY: clean\nclean:\n\t@:\n")
            build = project / "build"
            build.mkdir()
            kept = [
                "measurement.log",
                "user-notes.txt",
                "boot-proposal.txt",
                "extlinux.before-smartfan.conf",
                "custom-experiment.c",
            ]
            removed = [
                "fanctl",
                "encoder-monitor",
                "test_sensor",
                "test_process",
                "test_event_log",
                "test_lcd_concurrency",
                "threads/test_event_log",
                "threads/test_lcd_concurrency",
                "test-bmp180-process",
                "smartfan.dtbo",
            ]
            for name in kept + removed:
                (build / name).parent.mkdir(parents=True, exist_ok=True)
                (build / name).write_text("sentinel\n")
            result = subprocess.run(
                ["make", "clean", f"KDIR={kernel}"],
                cwd=project,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for name in kept:
                self.assertEqual((build / name).read_text(), "sentinel\n")
            for name in removed:
                self.assertFalse((build / name).exists(), name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
