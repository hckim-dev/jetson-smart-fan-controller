#!/usr/bin/env python3
"""Prepare an offline, reviewed output-pad DT update; never write to /boot."""
import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CONFIG = pathlib.Path("/boot/extlinux/extlinux.conf")
LABEL = "smartfan-output"
TARGET = "/boot/dtb/smartfan-stage1-output.dtb"
PINS = {"soc_gpio19_pg6", "soc_gpio39_pn1", "soc_gpio32_pq5"}


def get_property(dtb, node, prop, kind="s"):
    return subprocess.check_output(
        ["fdtget", "-t", kind, str(dtb), node, prop], text=True
    ).strip()


def main():
    raw = CONFIG.read_text()
    defaults = re.findall(r"^DEFAULT\s+(\S+)\s*$", raw, re.MULTILINE)
    if len(defaults) != 1:
        raise ValueError("Cannot identify a unique current default entry")
    blocks = list(re.finditer(
        r"^LABEL\s+(\S+)[^\n]*\n.*?(?=^LABEL\s+|\Z)",
        raw, re.MULTILINE | re.DOTALL,
    ))
    selected = [block for block in blocks if block.group(1) == defaults[0]]
    if len(selected) != 1:
        raise ValueError("Current default entry is ambiguous")
    entry = selected[0].group(0)
    paths = re.findall(r"^\s*FDT\s+(\S+)\s*$", entry, re.MULTILINE)
    if len(paths) != 1:
        raise ValueError("Selected entry must have exactly one FDT")
    base = pathlib.Path(paths[0])
    if not base.is_file():
        raise ValueError(f"Selected DTB is missing: {base}")
    overlays = re.findall(r"^\s*OVERLAYS\s+(.+)$", entry, re.MULTILINE)
    for names in overlays:
        for name in names.split():
            if not pathlib.Path(name).is_file():
                raise ValueError(f"Existing boot overlay is missing: {name}")

    BUILD.mkdir(exist_ok=True)
    merged = BUILD / "smartfan-board.dtb"
    subprocess.run([
        "fdtoverlay", "-i", str(base), "-o", str(merged),
        str(BUILD / "smartfan.dtbo"),
    ], check=True)
    state = "/bus@0/pinmux@2430000/smartfan-output"
    if get_property(merged, "/smartfan", "compatible") != "edu,jetson-smartfan":
        raise ValueError("Merged DTB lacks the smartfan device")
    if get_property(merged, "/smartfan", "pinctrl-names") != "default":
        raise ValueError("Missing default pinctrl state")
    if get_property(merged, "/smartfan", "pinctrl-0", "x") != get_property(
        merged, state, "phandle", "x"
    ):
        raise ValueError("pinctrl reference does not resolve to the output state")
    if set(get_property(merged, state + "/outputs", "nvidia,pins").split()) != PINS:
        raise ValueError("Output state must contain exactly the three motor pads")
    for prop in ("nvidia,tristate", "nvidia,enable-input", "nvidia,gpio-mode"):
        if get_property(merged, state + "/outputs", prop, "x") != "0":
            raise ValueError(f"Unexpected output pad setting: {prop}")

    # Preserve the currently selected boot entry as a fallback. Repeated
    # preparation updates our own entry instead of creating duplicate labels.
    new_entry = re.sub(r"^LABEL\s+\S+", "LABEL " + LABEL, entry, count=1)
    new_entry = re.sub(
        r"^(\s*MENU LABEL)[^\n]*", r"\1 Smart fan stage 1 GPIO output pads",
        new_entry, flags=re.MULTILINE,
    )
    new_entry = re.sub(
        r"^(\s*FDT\s+)\S+", lambda match: match.group(1) + TARGET,
        new_entry, flags=re.MULTILINE,
    )
    own_entries = [block for block in blocks if block.group(1) == LABEL]
    if len(own_entries) > 1:
        raise ValueError("Multiple smartfan-output entries require manual review")
    if own_entries:
        block = own_entries[0]
        proposal = raw[:block.start()] + new_entry.rstrip() + "\n\n" + raw[block.end():]
    else:
        proposal = raw.rstrip() + "\n\n" + new_entry.rstrip() + "\n"
    proposal = re.sub(
        r"^DEFAULT\s+\S+", "DEFAULT " + LABEL, proposal,
        count=1, flags=re.MULTILINE,
    )
    if "LABEL JetsonIO\n" not in proposal:
        raise ValueError("Original JetsonIO fallback entry must remain present")
    (BUILD / "extlinux.before-smartfan.conf").write_text(raw)
    (BUILD / "extlinux.smartfan.conf").write_text(proposal)
    (BUILD / "boot-proposal.txt").write_text(
        "NOT INSTALLED\n"
        f"Current default: {defaults[0]}\n"
        f"Current base DTB: {base}\n"
        f"Current extlinux SHA256: {hashlib.sha256(raw.encode()).hexdigest()}\n"
        f"Proposed default: {LABEL}\n"
        f"Proposed FDT: {TARGET}\n"
        "Original boot entries and overlays retained.\n"
        "Three motor pads: tristate=0, enable-input=0, gpio-mode=0.\n"
    )
    print("DT_READY: offline GPIO output-pad DT/config prepared; /boot unchanged")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Prepare failed: {error}", file=sys.stderr)
        sys.exit(1)
