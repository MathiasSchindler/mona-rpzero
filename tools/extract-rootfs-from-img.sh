#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/extract-rootfs-from-img.sh --img raspios.img --outdir build/root

Extracts partition 2 (rootfs) from a Raspberry Pi OS .img into a directory.
This is useful for kernel-first QEMU boot: you boot your own kernel+DTB, but
reuse a known-good userland so `/sbin/init` exists.

Requires: sudo, parted, mount, umount, rsync
EOF
}

IMG=""
OUTDIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --img) IMG="$2"; shift 2;;
    --outdir) OUTDIR="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2;;
  esac
done

if [[ -z "$IMG" || -z "$OUTDIR" ]]; then
  echo "Missing required arguments." >&2
  usage
  exit 2
fi

if [[ ! -f "$IMG" ]]; then
  echo "Image not found: $IMG" >&2
  exit 2
fi

for bin in sudo parted mount umount rsync; do
  command -v "$bin" >/dev/null 2>&1 || { echo "Missing dependency: $bin" >&2; exit 2; }
done

# Find start offset (bytes) of partition 2
# `parted -ms unit B print` output example:
# 2:4194304B:...:...:ext4::;
P2_START_B=$(parted -ms "$IMG" unit B print | awk -F: '$1==2 {gsub(/B/,"",$2); print $2; exit}')

if [[ -z "$P2_START_B" ]]; then
  echo "Could not determine partition 2 offset for: $IMG" >&2
  exit 2
fi

TMPDIR=$(mktemp -d)
cleanup() {
  set +e
  if mountpoint -q "$TMPDIR"; then
    sudo umount "$TMPDIR" || true
  fi
  rmdir "$TMPDIR" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$OUTDIR"

# Mount root partition read-only and copy into outdir.
sudo mount -o loop,ro,offset="$P2_START_B" "$IMG" "$TMPDIR"

# Rootfs is ext4; preserve attributes. Since destination is your workspace,
# keep ownership as your user to avoid root-owned trees.
# We run rsync without -o/-g to avoid creating root-owned files.
rsync -a --delete --no-owner --no-group --info=NAME "$TMPDIR"/ "$OUTDIR"/

echo "OK: extracted rootfs (p2 offset ${P2_START_B} bytes) into $OUTDIR"