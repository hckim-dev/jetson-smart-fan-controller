#!/usr/bin/env python3
"""임시 extlinux 구성으로 DT 제안의 fallback 선택과 변경 거부를 검증한다."""

import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "prepare_dt", ROOT / "scripts/prepare-dt.py"
)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class OverlayReached(Exception):
    pass


class PrepareTests(unittest.TestCase):
    def configuration(self, directory, fallback=True, matching=True):
        root = pathlib.Path(directory)
        active = root / "active.dtb"
        preserved = root / "preserved.dtb"
        active.touch()
        preserved.touch()
        retained = root / "retained.dtbo"
        other = root / "other.dtbo"
        retained.touch()
        other.touch()
        config = root / "extlinux.conf"
        text = f"DEFAULT smartfan-lcd\nLABEL smartfan-lcd\n  FDT {active}\n  OVERLAYS {retained}\n"
        if fallback:
            overlays = retained if matching else other
            text += f"LABEL smartfan-output\n  FDT {preserved}\n  OVERLAYS {overlays}\n"
        config.write_text(text)
        return config, preserved

    def test_retained_overlay_uses_preserved_base_not_active_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            config, preserved = self.configuration(directory)
            captured = []

            def stop_at_overlay(command, **kwargs):
                captured.append(command)
                raise OverlayReached

            with (
                mock.patch.object(prepare, "CONFIG", config),
                mock.patch.object(prepare, "BUILD", pathlib.Path(directory) / "build"),
                mock.patch.object(
                    prepare.subprocess, "run", side_effect=stop_at_overlay
                ),
                self.assertRaises(OverlayReached),
            ):
                prepare.main()
            self.assertEqual(len(captured), 1)
            command = captured[0]
            self.assertEqual(command[0], "fdtoverlay")
            self.assertEqual(command[command.index("-i") + 1], str(preserved))
            self.assertEqual(command[-1], str(preserved.parent / "retained.dtbo"))

    def test_missing_fallback_rejected_before_tools(self):
        self.assert_rejected(fallback=False, matching=True)

    def test_mismatched_retained_overlay_rejected_before_tools(self):
        self.assert_rejected(fallback=True, matching=False)

    def assert_rejected(self, fallback, matching):
        with tempfile.TemporaryDirectory() as directory:
            config, _ = self.configuration(directory, fallback, matching)
            with (
                mock.patch.object(prepare, "CONFIG", config),
                mock.patch.object(prepare.subprocess, "run") as run,
            ):
                with self.assertRaises(ValueError):
                    prepare.main()
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
