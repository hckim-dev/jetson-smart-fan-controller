#!/usr/bin/env python3
"""임시 extlinux 구성으로 DT 제안의 fallback 선택과 변경 거부를 검증한다."""

import contextlib
import importlib.util
import io
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

    def run_provider_case(self, directory, changed=False):
        config, preserved = self.configuration(directory)
        config.write_text(config.read_text() + f"LABEL JetsonIO\n  FDT {preserved}\n")
        active = preserved.parent / "active.dtb"
        build = preserved.parent / "build"
        merged = build / "smartfan-board.dtb"
        reads = []

        # 주변 pad 검증은 stub으로 분리하고 실제 provider 비교와 제안 출력까지 실행한다.
        def properties(tree, node):
            return ["vendor,guard-property"] if node == prepare.PINMUX else []

        def get_property(tree, node, prop, kind="s"):
            if prop == "vendor,guard-property":
                tree = pathlib.Path(tree)
                reads.append(tree)
                if tree == active:
                    return "aa"
                return "cc" if changed and tree == merged else "bb"
            return "1"

        def apply_overlay(command, **kwargs):
            self.assertEqual(command[0], "fdtoverlay")
            pathlib.Path(command[command.index("-o") + 1]).write_bytes(b"fixture DTB")

        with (
            mock.patch.object(prepare, "CONFIG", config),
            mock.patch.object(prepare, "BUILD", build),
            mock.patch.object(prepare, "properties", side_effect=properties),
            mock.patch.object(prepare, "get_property", side_effect=get_property),
            mock.patch.object(prepare, "snapshot_node", return_value={}),
            mock.patch.object(prepare, "check_controller_dt"),
            mock.patch.object(prepare.subprocess, "run", side_effect=apply_overlay),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            if changed:
                with self.assertRaisesRegex(
                    ValueError, "Pinmux provider properties changed"
                ):
                    prepare.main()
                self.assertFalse((build / "extlinux.smartfan.conf").exists())
            else:
                prepare.main()
                self.assertTrue((build / "extlinux.smartfan.conf").is_file())
                record = (build / "boot-proposal.txt").read_text()
                self.assertIn(f"Preparation base DTB: {preserved}", record)
        self.assertIn(preserved, reads)
        self.assertIn(merged, reads)
        self.assertNotIn(active, reads)

    def test_provider_baseline_uses_preserved_tree_through_proposal(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_provider_case(directory)

    def test_changed_provider_rejected_before_proposal(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_provider_case(directory, changed=True)

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
