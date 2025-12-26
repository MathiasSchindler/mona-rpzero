#!/usr/bin/env bash
set -euo pipefail

# Creates a Raspberry Pi-style SD image with:
#  - p1: FAT32 (boot)
#  - p2: ext4  (root)
#
# Note: QEMU's `-M raspi3b` does NOT run the Raspberry Pi GPU firmware.
# QEMU typically loads your kernel directly via `-kernel` and uses `-dtb`.
# This image is still useful as a root filesystem (`/dev/mmcblk0p2`) and as
# a place to keep boot artifacts together (kernel/dtb/initramfs/config/cmdline).

usage() {
  cat <<'EOF'
Usage:
  tools/make-sd-image.sh \
    --image sd.img \
    --size 1024 \
    --bootdir boot_files/ \
    --rootdir root_files/

Optional:
  --rootless  Create/populate partitions without sudo/loop mounts (default).
  --mount     Use loop devices + mount + rsync (requires sudo).

Arguments:
  --image   Path to output raw image file.
  --size    Size in MiB (default: 1024).
  --bootdir Directory copied into the FAT boot partition.
           Typical contents for kernel-first:
             kernel8.img
             bcm2710-rpi-zero-2-w.dtb   (or another Pi3/BCM2710 DTB)
             initramfs.cpio.gz          (optional)
             cmdline.txt                (optional; for real Pi firmware)
             config.txt                 (optional; for real Pi firmware)
  --rootdir Directory copied into the ext4 root partition.
           For Linux+initramfs this can be empty; for Linux+rootfs put an
           extracted root filesystem tree here.

This script requires:
  - parted, mkfs.vfat, mkfs.ext4

Rootless mode additionally requires:
  - mtools (mcopy)
  - dd

Mount mode additionally requires:
  - root privileges (loop devices / mounting)
  - losetup, mount, umount
  - rsync

Example:
  mkdir -p out/boot out/root
  cp kernel8.img bcm2710-rpi-zero-2-w.dtb out/boot/
  sudo tools/make-sd-image.sh --image sd.img --size 2048 --bootdir out/boot --rootdir out/root
EOF
}

IMAGE=""
SIZE_MIB=1024
BOOTDIR=""
ROOTDIR=""
MODE="rootless"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image) IMAGE="$2"; shift 2;;
    --size) SIZE_MIB="$2"; shift 2;;
    --bootdir) BOOTDIR="$2"; shift 2;;
    --rootdir) ROOTDIR="$2"; shift 2;;
    --rootless) MODE="rootless"; shift 1;;
    --mount) MODE="mount"; shift 1;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2;;
  esac
done

if [[ -z "$IMAGE" || -z "$BOOTDIR" || -z "$ROOTDIR" ]]; then
  echo "Missing required arguments." >&2
  usage
  exit 2
fi

if [[ ! -d "$BOOTDIR" ]]; then
  echo "bootdir not found: $BOOTDIR" >&2
  exit 2
fi
if [[ ! -d "$ROOTDIR" ]]; then
  echo "rootdir not found: $ROOTDIR" >&2
  exit 2
fi

for bin in parted mkfs.vfat mkfs.ext4; do
  command -v "$bin" >/dev/null 2>&1 || { echo "Missing dependency: $bin" >&2; exit 2; }
done

if [[ "$MODE" == "rootless" ]]; then
  for bin in mcopy dd awk; do
    command -v "$bin" >/dev/null 2>&1 || { echo "Missing dependency (rootless): $bin" >&2; exit 2; }
  done
else
  for bin in rsync losetup mount umount; do
    command -v "$bin" >/dev/null 2>&1 || { echo "Missing dependency (mount mode): $bin" >&2; exit 2; }
  done
fi

TMPDIR=""
LOOPDEV=""
cleanup() {
  set +e
  if [[ -n "$TMPDIR" ]]; then
    if mountpoint -q "$TMPDIR/boot"; then umount "$TMPDIR/boot"; fi
    if mountpoint -q "$TMPDIR/root"; then umount "$TMPDIR/root"; fi
    rmdir "$TMPDIR/boot" "$TMPDIR/root" 2>/dev/null || true
    rmdir "$TMPDIR" 2>/dev/null || true
  fi
  if [[ -n "$LOOPDEV" ]]; then
    losetup -d "$LOOPDEV" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Create blank image
rm -f "$IMAGE"
truncate -s "$((SIZE_MIB))M" "$IMAGE"

# Partition: p1 FAT32 boot (256 MiB), p2 ext4 root (rest)
parted -s "$IMAGE" mklabel msdos
parted -s "$IMAGE" mkpart primary fat32 1MiB 257MiB
parted -s "$IMAGE" set 1 boot on
parted -s "$IMAGE" mkpart primary ext4 257MiB 100%

if [[ "$MODE" == "rootless" ]]; then
  # Get offsets/sizes from partition table.
  # Output format: num:start:end:size:fs:name:flags
  P1_LINE=$(parted -ms "$IMAGE" unit B print | awk -F: '$1==1 {print; exit}')
  P2_LINE=$(parted -ms "$IMAGE" unit B print | awk -F: '$1==2 {print; exit}')
  if [[ -z "$P1_LINE" || -z "$P2_LINE" ]]; then
    echo "Failed to parse partitions from image: $IMAGE" >&2
    exit 2
  fi

  p1_start_b=$(echo "$P1_LINE" | awk -F: '{gsub(/B/,"",$2); print $2}')
  p1_size_b=$(echo "$P1_LINE" | awk -F: '{gsub(/B/,"",$4); print $4}')
  p2_start_b=$(echo "$P2_LINE" | awk -F: '{gsub(/B/,"",$2); print $2}')
  p2_size_b=$(echo "$P2_LINE" | awk -F: '{gsub(/B/,"",$4); print $4}')

  if (( p1_start_b % 512 != 0 )) || (( p2_start_b % 512 != 0 )); then
    echo "Partition offsets are not 512-byte aligned (unexpected)." >&2
    exit 2
  fi

  p1_start_sector=$((p1_start_b / 512))
  p2_start_sector=$((p2_start_b / 512))

  # Build a FAT32 filesystem image for boot, populate it, then dd it into place.
  BOOT_TMP=$(mktemp -p "$(dirname "$IMAGE")" boot.XXXXXX.img)
  truncate -s "$p1_size_b" "$BOOT_TMP"
  mkfs.vfat -F 32 -n BOOT "$BOOT_TMP" >/dev/null

  shopt -s dotglob nullglob
  boot_files=("$BOOTDIR"/*)
  shopt -u dotglob nullglob
  if (( ${#boot_files[@]} > 0 )); then
    shopt -s dotglob nullglob
    mcopy -i "$BOOT_TMP" -s "$BOOTDIR"/* ::/
    shopt -u dotglob nullglob
  fi

  dd if="$BOOT_TMP" of="$IMAGE" bs=512 seek="$p1_start_sector" conv=notrunc status=none
  rm -f "$BOOT_TMP"

  # Build an ext4 filesystem image for root, then dd it into place.
  ROOT_TMP=$(mktemp -p "$(dirname "$IMAGE")" rootfs.XXXXXX.img)
  truncate -s "$p2_size_b" "$ROOT_TMP"
  mkfs.ext4 -F -L rootfs -d "$ROOTDIR" "$ROOT_TMP" >/dev/null

  dd if="$ROOT_TMP" of="$IMAGE" bs=512 seek="$p2_start_sector" conv=notrunc status=none
  rm -f "$ROOT_TMP"

  sync
else
  # Legacy path: requires root privileges.
  # Attach loop device with partition nodes
  LOOPDEV=$(losetup --find --show --partscan "$IMAGE")
  BOOT_PART="${LOOPDEV}p1"
  ROOT_PART="${LOOPDEV}p2"

  # Format
  mkfs.vfat -F 32 -n BOOT "$BOOT_PART" >/dev/null
  mkfs.ext4 -F -L rootfs "$ROOT_PART" >/dev/null

  # Mount + copy
  TMPDIR=$(mktemp -d)
  mkdir -p "$TMPDIR/boot" "$TMPDIR/root"
  mount "$BOOT_PART" "$TMPDIR/boot"
  mount "$ROOT_PART" "$TMPDIR/root"

  # FAT/vfat doesn't support Unix ownership/permissions; when running as root,
  # rsync -a would try to chown and fail (code 23). So we explicitly disable
  # owner/group/perms preservation for the boot partition.
  rsync -a --delete --info=NAME --no-owner --no-group --no-perms "$BOOTDIR"/ "$TMPDIR/boot"/
  rsync -a --delete --info=NAME "$ROOTDIR"/ "$TMPDIR/root"/

  sync
fi

# If invoked via sudo, hand the resulting image back to the original user so
# unprivileged tools (like qemu) can open it.
if [[ -n "${SUDO_UID-}" && -n "${SUDO_GID-}" ]]; then
  chown "$SUDO_UID:$SUDO_GID" "$IMAGE" 2>/dev/null || true
  chmod u+rw "$IMAGE" 2>/dev/null || true
fi

echo "OK: created $IMAGE ($SIZE_MIB MiB)"
echo " - boot: FAT32, label BOOT"
echo " - root: ext4,  label rootfs"