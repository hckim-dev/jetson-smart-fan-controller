#!/usr/bin/env python3
"""Prepare an offline LCD/LED Bar/PWM DT update; never write to /boot."""

import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CONFIG = pathlib.Path("/boot/extlinux/extlinux.conf")
LABEL = "smartfan-lcd"
TARGET = "/boot/dtb/smartfan-lcd.dtb"
PINMUX = "/bus@0/pinmux@2430000"
OUTPUT_STATE = PINMUX + "/smartfan-output"
PWM_STATE = PINMUX + "/smartfan-pwm"
PWM_NODE = "/bus@0/pwm@3280000"
I2C_NODE = "/bus@0/i2c@c250000"
GPIO_PINS = {"soc_gpio19_pg6", "soc_gpio32_pq5"}
ENCODER_PINS = {"soc_gpio41_ph7", "soc_gpio43_pi1"}
LED_PINS = {
    "spi3_sck_py0",
    "spi3_cs1_py4",
    "spi3_cs0_py3",
    "spi3_miso_py1",
    "soc_gpio21_ph0",
    "soc_gpio44_pi2",
    "spi3_mosi_py2",
    "soc_gpio42_pi0",
}


def get_property(dtb, node, prop, kind="s"):
    return subprocess.check_output(
        ["fdtget", "-t", kind, str(dtb), node, prop], text=True
    ).strip()


def properties(dtb, node):
    return subprocess.check_output(
        ["fdtget", "-p", str(dtb), node], text=True
    ).splitlines()


def snapshot_node(dtb, node):
    """Compare protected subtrees without assuming their property types."""
    result = {
        prop: get_property(dtb, node, prop, "bx")
        for prop in properties(dtb, node)
        if prop not in ("phandle", "linux,phandle")
    }
    children = subprocess.check_output(
        ["fdtget", "-l", str(dtb), node], text=True
    ).splitlines()
    for child in children:
        result[child + "/"] = snapshot_node(dtb, node + "/" + child)
    return result


def cells(dtb, node, prop):
    return [int(value, 16) for value in get_property(dtb, node, prop, "x").split()]


def assert_property(dtb, node, prop, expected, kind="s"):
    actual = get_property(dtb, node, prop, kind)
    if actual != expected:
        raise ValueError(f"Unexpected {node}:{prop}: {actual!r}, expected {expected!r}")


def check_stage2(dtb):
    assert_property(dtb, "/smartfan", "compatible", "edu,jetson-smartfan")
    assert_property(dtb, "/smartfan", "pinctrl-names", "default")
    assert_property(
        dtb,
        "/smartfan",
        "pinctrl-0",
        get_property(dtb, OUTPUT_STATE, "phandle", "x"),
        "x",
    )
    if "in1-gpios" in properties(dtb, "/smartfan"):
        raise ValueError("Legacy IN1 GPIO must not coexist with motor PWM")
    gpio_node = get_property(dtb, "/__symbols__", "gpio")
    gpio_phandle = cells(dtb, gpio_node, "phandle")[0]
    for prop, offset in (("enable-gpios", 54), ("in2-gpios", 125)):
        if cells(dtb, "/smartfan", prop) != [gpio_phandle, offset, 0]:
            raise ValueError(f"Unexpected motor GPIO descriptor: {prop}")
    expected_leds = [
        cell
        for offset in (144, 148, 147, 145, 56, 66, 146, 64)
        for cell in (gpio_phandle, offset, 0)
    ]
    if cells(dtb, "/smartfan", "led-gpios") != expected_leds:
        raise ValueError(
            "LED GPIO order/polarity must match J12 13,16,18,22,33,35,37,40"
        )
    leds = OUTPUT_STATE + "/led-outputs"
    if set(get_property(dtb, leds, "nvidia,pins").split()) != LED_PINS:
        raise ValueError("Unexpected LED pad mapping")
    for prop in (
        "nvidia,tristate",
        "nvidia,enable-input",
        "nvidia,gpio-mode",
        "nvidia,open-drain",
        "nvidia,pull",
    ):
        assert_property(dtb, leds, prop, "0", "x")
    assert_property(dtb, "/bus@0/spi@3230000", "status", "disabled")
    assert_property(dtb, I2C_NODE, "clock-frequency", "186a0", "x")
    pwm_phandle = cells(dtb, PWM_NODE, "phandle")[0]
    if cells(dtb, "/smartfan", "pwms") != [pwm_phandle, 0, 4000000]:
        raise ValueError("Motor PWM must use PWM1 channel 0 with a 4 ms period")
    assert_property(dtb, "/smartfan", "pwm-names", "motor")
    for prop, value in (
        ("lease-timeout-ms", 2000),
        ("max-on-ms", 30000),
        ("startup-boost-ms", 200),
    ):
        if cells(dtb, "/smartfan", prop) != [value]:
            raise ValueError(f"Unexpected motor timing: {prop}")
    outputs = OUTPUT_STATE + "/outputs"
    inputs = OUTPUT_STATE + "/encoder-inputs"
    if set(get_property(dtb, outputs, "nvidia,pins").split()) != GPIO_PINS:
        raise ValueError("GPIO output state must contain only ENA/IN2 pads")
    if set(get_property(dtb, inputs, "nvidia,pins").split()) != ENCODER_PINS:
        raise ValueError("Encoder input state must contain only PH7/PI1 pads")
    for prop in ("nvidia,tristate", "nvidia,enable-input", "nvidia,gpio-mode"):
        assert_property(dtb, outputs, prop, "0", "x")
    for prop, value in (
        ("nvidia,tristate", "1"),
        ("nvidia,enable-input", "1"),
        ("nvidia,gpio-mode", "0"),
        ("nvidia,pull", "0"),
    ):
        assert_property(dtb, inputs, prop, value, "x")
    assert_property(dtb, PWM_NODE, "status", "okay")
    assert_property(dtb, PWM_NODE, "#pwm-cells", "2", "x")
    assert_property(dtb, PWM_NODE, "pinctrl-names", "default")
    assert_property(
        dtb, PWM_NODE, "pinctrl-0", get_property(dtb, PWM_STATE, "phandle", "x"), "x"
    )
    pwm_pad = PWM_STATE + "/pwm-output"
    assert_property(dtb, pwm_pad, "nvidia,pins", "soc_gpio39_pn1")
    assert_property(dtb, pwm_pad, "nvidia,function", "gp")
    for prop, value in (
        ("nvidia,tristate", "0"),
        ("nvidia,enable-input", "0"),
        ("nvidia,gpio-mode", "1"),
    ):
        assert_property(dtb, pwm_pad, prop, value, "x")


def main():
    raw = CONFIG.read_text()
    defaults = re.findall(r"^DEFAULT\s+(\S+)\s*$", raw, re.MULTILINE)
    if len(defaults) != 1:
        raise ValueError("Cannot identify a unique current default entry")
    blocks = list(
        re.finditer(
            r"^LABEL\s+(\S+)[^\n]*\n.*?(?=^LABEL\s+|\Z)",
            raw,
            re.MULTILINE | re.DOTALL,
        )
    )
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

    # If stage 2 is already the selected boot entry, rebuild from its preserved
    # stage-1 fallback rather than applying the overlay on top of itself. This
    # makes prepare/check safely repeatable after installation.
    preparation_base = base
    if defaults[0] in {
        LABEL,
        "smartfan-speed",
        "smartfan-encoder-alt",
        "smartfan-ledbar",
    }:
        fallbacks = [block for block in blocks if block.group(1) == "smartfan-output"]
        if len(fallbacks) != 1:
            raise ValueError(
                "Stage-2 is active but its smartfan-output fallback is missing"
            )
        fallback_entry = fallbacks[0].group(0)
        fallback_paths = re.findall(
            r"^\s*FDT\s+(\S+)\s*$", fallback_entry, re.MULTILINE
        )
        fallback_overlays = re.findall(
            r"^\s*OVERLAYS\s+(.+)$", fallback_entry, re.MULTILINE
        )
        if len(fallback_paths) != 1 or fallback_overlays != overlays:
            raise ValueError(
                "Stage-2 fallback FDT/overlays do not match the active entry"
            )
        preparation_base = pathlib.Path(fallback_paths[0])
        if not preparation_base.is_file():
            raise ValueError(
                f"Preserved stage-1 base DTB is missing: {preparation_base}"
            )

    BUILD.mkdir(exist_ok=True)
    existing_overlays = [name for names in overlays for name in names.split()]
    effective_base = preparation_base
    if existing_overlays:
        effective_base = BUILD / "smartfan-current-effective.dtb"
        subprocess.run(
            [
                "fdtoverlay",
                "-i",
                str(base),
                "-o",
                str(effective_base),
                *existing_overlays,
            ],
            check=True,
        )

    # The PWM driver reselects default/sleep on runtime resume/suspend. Refuse
    # an unexpected existing state instead of replacing another user's pins.
    pwm_props = properties(effective_base, PWM_NODE)
    if "pinctrl-names" in pwm_props:
        assert_property(effective_base, PWM_NODE, "pinctrl-names", "default")
        assert_property(
            effective_base,
            PWM_NODE,
            "pinctrl-0",
            get_property(effective_base, PWM_STATE, "phandle", "x"),
            "x",
        )
    if any(re.fullmatch(r"pinctrl-[1-9][0-9]*", prop) for prop in pwm_props):
        raise ValueError("PWM1 has another pinctrl state; review it before preparation")
    protected_paths = (
        PINMUX + "/exp-header-pinmux",
        "/bus@0/pwm@32a0000",
        "/bus@0/spi@3210000",
    )
    protected = {node: snapshot_node(effective_base, node) for node in protected_paths}
    i2c_before = snapshot_node(effective_base, I2C_NODE)
    i2c_before.pop("clock-frequency", None)
    assert_property(
        effective_base,
        PINMUX,
        "pinctrl-0",
        get_property(effective_base, protected_paths[0], "phandle", "x"),
        "x",
    )
    raw_provider_properties = {
        prop: get_property(base, PINMUX, prop, "bx")
        for prop in properties(preparation_base, PINMUX)
    }
    raw_builtin_pwm = snapshot_node(preparation_base, protected_paths[1])

    merged = BUILD / "smartfan-board.dtb"
    subprocess.run(
        [
            "fdtoverlay",
            "-i",
            str(preparation_base),
            "-o",
            str(merged),
            str(BUILD / "smartfan.dtbo"),
        ],
        check=True,
    )
    # DT overlays do not delete an existing base-tree property with
    # /delete-property/. Remove the stage-1 IN1 descriptor from this offline copy.
    if "in1-gpios" in properties(merged, "/smartfan"):
        subprocess.run(
            ["fdtput", "-d", str(merged), "/smartfan", "in1-gpios"], check=True
        )
    check_stage2(merged)
    if {
        prop: get_property(merged, PINMUX, prop, "bx")
        for prop in properties(merged, PINMUX)
    } != raw_provider_properties:
        raise ValueError("Pinmux provider properties changed")
    if snapshot_node(merged, protected_paths[1]) != raw_builtin_pwm:
        raise ValueError("Built-in fan PWM provider changed")
    # The bootloader applies retained overlays after loading the proposed FDT.
    # Validate that effective order too, while leaving the FDT/entry split intact.
    effective = merged
    if existing_overlays:
        effective = BUILD / "smartfan-effective-board.dtb"
        subprocess.run(
            ["fdtoverlay", "-i", str(merged), "-o", str(effective), *existing_overlays],
            check=True,
        )
    check_stage2(effective)
    # Applying an existing overlay can renumber its local phandles. Verify
    # the resolved state and its contents, rather than numeric IDs.
    assert_property(
        effective,
        PINMUX,
        "pinctrl-0",
        get_property(effective, protected_paths[0], "phandle", "x"),
        "x",
    )
    for node, expected in protected.items():
        if snapshot_node(effective, node) != expected:
            raise ValueError(f"Existing boot overlay changes protected state: {node}")
    i2c_after = snapshot_node(effective, I2C_NODE)
    i2c_after.pop("clock-frequency", None)
    if i2c_after != i2c_before:
        raise ValueError(
            "Header I2C properties/children changed beyond clock-frequency"
        )

    # Preserve the currently selected boot entry as a fallback. Repeated
    # preparation updates our own entry instead of creating duplicate labels.
    new_entry = re.sub(r"^LABEL\s+\S+", "LABEL " + LABEL, entry, count=1)
    new_entry = re.sub(
        r"^(\s*MENU LABEL)[^\n]*",
        r"\1 Smart fan LCD I2C 100kHz and LED Bar",
        new_entry,
        flags=re.MULTILINE,
    )
    new_entry = re.sub(
        r"^(\s*FDT\s+)\S+",
        lambda match: match.group(1) + TARGET,
        new_entry,
        flags=re.MULTILINE,
    )
    own_entries = [block for block in blocks if block.group(1) == LABEL]
    if len(own_entries) > 1:
        raise ValueError("Multiple smartfan-lcd entries require manual review")
    if own_entries:
        block = own_entries[0]
        prefix, suffix = raw[: block.start()], raw[block.end() :]
        if not suffix:
            proposal = prefix + new_entry.rstrip() + "\n"
        else:
            proposal = prefix + new_entry.rstrip() + "\n\n" + suffix.lstrip("\n")
    else:
        proposal = raw.rstrip() + "\n\n" + new_entry.rstrip() + "\n"
    proposal = re.sub(
        r"^DEFAULT\s+\S+",
        "DEFAULT " + LABEL,
        proposal,
        count=1,
        flags=re.MULTILINE,
    )
    if "LABEL JetsonIO\n" not in proposal:
        raise ValueError("Original JetsonIO fallback entry must remain present")
    for block in blocks:
        if block.group(1) != LABEL and block.group(0).rstrip() not in proposal:
            raise ValueError(f"Existing fallback entry changed: {block.group(1)}")
    (BUILD / "extlinux.before-smartfan.conf").write_text(raw)
    (BUILD / "extlinux.smartfan.conf").write_text(proposal)
    (BUILD / "boot-proposal.txt").write_text(
        "NOT INSTALLED\n"
        f"Current default: {defaults[0]}\n"
        f"Current base DTB: {base}\n"
        f"Preparation base DTB: {preparation_base}\n"
        f"Current extlinux SHA256: {hashlib.sha256(raw.encode()).hexdigest()}\n"
        f"Proposed default: {LABEL}\n"
        f"Proposed FDT: {TARGET}\n"
        "Original boot entries and overlays retained.\n"
        "GPIO interlock: PG6/PQ5; motor PWM1/PN1: 250 Hz; startup boost: 200 ms.\n"
        "Encoder PH7/PI1 inputs: tristate=1, enable-input=1, gpio-mode=0, pull=0.\n"
        "LED Bar J12 13/16/18/22/33/35/37/40: GPIO push-pull output, initial LOW.\n"
        "Header SPI1 disabled for LED pins; SPI0 retained.\n"
        "Header I2C c250000 clock set to 100000 Hz for PCF8574T; child devices retained.\n"
        "Legacy in1-gpios removed; provider default, UART and built-in fan retained.\n"
        f"Prepared DTB SHA256: {hashlib.sha256(merged.read_bytes()).hexdigest()}\n"
        f"Prepared config SHA256: {hashlib.sha256(proposal.encode()).hexdigest()}\n"
    )
    print(
        "DT_READY: offline LCD 100kHz/LED Bar/PWM DT/config prepared; /boot unchanged"
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Prepare failed: {error}", file=sys.stderr)
        sys.exit(1)
