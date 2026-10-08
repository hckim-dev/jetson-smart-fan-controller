#!/usr/bin/env bash
# Install the reviewed stage-1 boot proposal. No module load, GPIO or reboot.
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
  /boot/dtb/smartfan-stage1.dtb:smartfan|/boot/dtb/smartfan-stage1-output.dtb:smartfan-output) ;;
  *) echo 'Unexpected managed DTB path or boot label.' >&2; exit 1 ;;
esac
if [[ -L "$task_dtb" ]]; then
  echo 'Refusing to replace a symlink at the proposed DTB path.' >&2
  exit 1
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
  echo 'Different smartfan-stage1.dtb already exists; refusing to overwrite.' >&2
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
task_tmp_dtb=$(mktemp /boot/dtb/.smartfan-stage1.XXXXXX)
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
