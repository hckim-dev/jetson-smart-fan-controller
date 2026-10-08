#!/usr/bin/env bash
# Install a reviewed boot proposal. No module load, GPIO or reboot.
set -euo pipefail

task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
task_cfg=/boot/extlinux/extlinux.conf
task_cfg_proposal="$task_root/build/extlinux.smartfan.conf"
task_dtb_proposal="$task_root/build/smartfan-board.dtb"
task_record="$task_root/build/boot-proposal.txt"
task_tmp_cfg=
task_tmp_dtb=
task_check_only=0

if [[ $# -gt 1 ]]; then
  echo 'Usage: install-dt.sh [--check]' >&2
  exit 1
fi
case "${1:-}" in
--check) task_check_only=1 ;;
'') ;;
*)
  echo 'Usage: install-dt.sh [--check]' >&2
  exit 1
  ;;
esac

cleanup() {
  if [[ -n "$task_tmp_cfg" ]]; then rm -f -- "$task_tmp_cfg"; fi
  if [[ -n "$task_tmp_dtb" ]]; then rm -f -- "$task_tmp_dtb"; fi
}
trap cleanup EXIT

if [[ $EUID -ne 0 && "$task_check_only" -eq 0 ]]; then
  echo 'Run: sudo bash scripts/install-dt.sh' >&2
  exit 1
fi
for task_path in "$task_cfg_proposal" "$task_dtb_proposal" "$task_record" "$task_cfg"; do
  if [[ ! -f "$task_path" || -L "$task_path" ]]; then
    printf 'Missing or unexpected symlink: %s\n' "$task_path" >&2
    exit 1
  fi
done
task_dtb=$(awk '/^Proposed FDT: / {print $3}' "$task_record")
task_label=$(awk '/^Proposed default: / {print $3}' "$task_record")
if [[ -z "$task_label" ]]; then task_label=smartfan; fi
case "$task_dtb:$task_label" in
/boot/dtb/smartfan-stage1.dtb:smartfan | /boot/dtb/smartfan-stage1-output.dtb:smartfan-output | /boot/dtb/smartfan-stage2.dtb:smartfan-speed | /boot/dtb/smartfan-stage2-encoder-alt.dtb:smartfan-encoder-alt | /boot/dtb/smartfan-ledbar.dtb:smartfan-ledbar | /boot/dtb/smartfan-lcd.dtb:smartfan-lcd) ;;
*)
  echo 'Unexpected managed DTB path or boot label.' >&2
  exit 1
  ;;
esac
if [[ -L "$task_dtb" ]]; then
  echo 'Refusing to replace a symlink at the proposed DTB path.' >&2
  exit 1
fi
if [[ "$task_label" == smartfan-speed || "$task_label" == smartfan-encoder-alt || "$task_label" == smartfan-ledbar || "$task_label" == smartfan-lcd ]]; then
  task_dtb_hash=$(awk '/^Prepared DTB SHA256: / {print $4}' "$task_record")
  task_proposal_hash=$(awk '/^Prepared config SHA256: / {print $4}' "$task_record")
  if [[ ! "$task_dtb_hash" =~ ^[a-f0-9]{64}$ ||
    ! "$task_proposal_hash" =~ ^[a-f0-9]{64}$ ||
    "$task_dtb_hash" != "$(sha256sum -- "$task_dtb_proposal" | awk '{print $1}')" ||
    "$task_proposal_hash" != "$(sha256sum -- "$task_cfg_proposal" | awk '{print $1}')" ]]; then
    echo 'Prepared DTB or config changed since review. Nothing installed.' >&2
    exit 1
  fi
fi
if cmp -s -- "$task_cfg_proposal" "$task_cfg"; then
  if [[ -f "$task_dtb" ]] && cmp -s -- "$task_dtb_proposal" "$task_dtb"; then
    echo 'DT_ALREADY_INSTALLED; reboot still required if not done.'
    exit 0
  fi
  echo 'Boot config already selects smartfan, but DTB differs/missing; stop for review.' >&2
  exit 1
fi

task_expected_hash=$(awk '/^Current extlinux SHA256: / {print $4}' "$task_record")
task_current_hash=$(sha256sum -- "$task_cfg" | awk '{print $1}')
if [[ ! "$task_expected_hash" =~ ^[a-f0-9]{64}$ || "$task_expected_hash" != "$task_current_hash" ]]; then
  echo 'Boot config changed since review. Nothing installed; prepare a new proposal.' >&2
  exit 1
fi
if [[ -e "$task_dtb" ]] && ! cmp -s -- "$task_dtb_proposal" "$task_dtb"; then
  printf 'Different DTB already exists at %s; refusing to overwrite.\n' "$task_dtb" >&2
  exit 1
fi
task_compatible=$(fdtget -t s "$task_dtb_proposal" /smartfan compatible)
if [[ "$task_compatible" != edu,jetson-smartfan ]]; then
  echo 'Prepared DTB does not contain the expected device.' >&2
  exit 1
fi
if ! grep -qx "DEFAULT $task_label" "$task_cfg_proposal" ||
  ! grep -qx "LABEL $task_label" "$task_cfg_proposal" ||
  ! grep -qx 'LABEL JetsonIO' "$task_cfg_proposal"; then
  echo 'Boot proposal lacks the reviewed default or original fallback entry.' >&2
  exit 1
fi
if [[ "$task_label" == smartfan-speed || "$task_label" == smartfan-encoder-alt || "$task_label" == smartfan-ledbar || "$task_label" == smartfan-lcd ]]; then
  task_pwm=$(fdtget -t s "$task_dtb_proposal" /smartfan pwm-names)
  if [[ "$task_pwm" != motor ]] ||
    fdtget -p "$task_dtb_proposal" /smartfan | grep -qx 'in1-gpios'; then
    echo 'Stage-2 DTB lacks motor PWM or retains legacy IN1 GPIO.' >&2
    exit 1
  fi
  if ! grep -qx 'LABEL smartfan-output' "$task_cfg_proposal" ||
    ! grep -qx 'LABEL smartfan' "$task_cfg_proposal"; then
    echo 'Stage-1 boot fallbacks must remain available.' >&2
    exit 1
  fi
  if [[ "$task_label" == smartfan-encoder-alt ]]; then
    if ! grep -qx 'LABEL smartfan-speed' "$task_cfg_proposal" ||
      [[ "$(fdtget -t s "$task_dtb_proposal" /bus@0/pinmux@2430000/smartfan-output/encoder-inputs nvidia,pins)" != 'soc_gpio41_ph7 soc_gpio43_pi1' ]]; then
      echo 'Alternate encoder DTB or original stage-2 fallback is missing.' >&2
      exit 1
    fi
  fi
  if [[ "$task_label" == smartfan-ledbar || "$task_label" == smartfan-lcd ]]; then
    if ! grep -qx 'LABEL smartfan-encoder-alt' "$task_cfg_proposal" ||
      [[ "$(fdtget -t s "$task_dtb_proposal" /bus@0/spi@3230000 status)" != disabled ]] ||
      [[ "$(fdtget -t x "$task_dtb_proposal" /bus@0/pinmux@2430000/smartfan-output/led-outputs nvidia,tristate)" != 0 ]]; then
      echo 'LED DTB or original encoder boot fallback is missing.' >&2
      exit 1
    fi
  fi
  if [[ "$task_label" == smartfan-lcd ]]; then
    if ! grep -qx 'LABEL smartfan-ledbar' "$task_cfg_proposal" ||
      [[ "$(fdtget -t x "$task_dtb_proposal" /bus@0/i2c@c250000 clock-frequency)" != 186a0 ]]; then
      echo 'LCD I2C must be 100kHz and LED Bar fallback must remain.' >&2
      exit 1
    fi
  fi
fi

if [[ "$task_check_only" -eq 1 ]]; then
  echo 'DT_READY: proposal and current boot config verified; nothing installed.'
  exit 0
fi

task_stamp=$(date -u +%Y%m%dT%H%M%SZ)
task_backup="${task_cfg}.before-smartfan-${task_stamp}"
if [[ -e "$task_backup" ]]; then
  echo 'Backup filename exists; retry later.' >&2
  exit 1
fi
cp -a -- "$task_cfg" "$task_backup"
task_tmp_dtb=$(mktemp /boot/dtb/.smartfan-dtb.XXXXXX)
task_tmp_cfg=$(mktemp /boot/extlinux/.smartfan-config.XXXXXX)
install -m 0644 -- "$task_dtb_proposal" "$task_tmp_dtb"
install -m 0644 -- "$task_cfg_proposal" "$task_tmp_cfg"
cmp -s -- "$task_dtb_proposal" "$task_tmp_dtb"
cmp -s -- "$task_cfg_proposal" "$task_tmp_cfg"

# Publish the new DTB before the config that selects it.
mv -f -- "$task_tmp_dtb" "$task_dtb"
task_tmp_dtb=
mv -f -- "$task_tmp_cfg" "$task_cfg"
task_tmp_cfg=
sync
printf 'BACKUP=%s\n' "$task_backup"
echo 'DT_INSTALLED; original boot entries retained; no running GPIO changed by installer.'
echo 'No module loaded. Next: sudo reboot'
