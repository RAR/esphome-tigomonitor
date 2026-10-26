#!/bin/bash
# Builds a ready-to-flash SD card image: Raspberry Pi OS Lite (arm64) with
# tigomonitord installed and enabled.
#
#   sudo linux/image/build-image.sh path/to/tigomonitord_VERSION_arm64.deb
#
# Needs root (loop devices, chroot) and an arm64 host, so packages install
# natively — CI runs it on GitHub's ubuntu-24.04-arm runner. Output:
#   build/image/tigomonitor-VERSION-arm64.img.xz (+ .sha256)
#
# The image has no user account and no Wi-Fi of its own: Raspberry Pi Imager's
# "Edit settings" adds both when the card is flashed. That is the step a user
# cannot skip on a Pi Zero 2 W, which has no Ethernet.
set -euo pipefail

deb="$(realpath "${1:?usage: build-image.sh tigomonitord_VERSION_arm64.deb}")"
version="$(dpkg-deb -f "$deb" Version)"
base_url="${RASPIOS_URL:-https://downloads.raspberrypi.com/raspios_lite_arm64_latest}"
grow_mb=256
work="build/image"
mnt="$work/mnt"
loop=""

cleanup() {
  set +e
  for m in dev/pts dev proc sys boot/firmware ""; do
    mountpoint -q "$mnt/$m" && umount "$mnt/$m"
  done
  [ -n "$loop" ] && losetup -d "$loop"
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ "$(dpkg --print-architecture)" = arm64 ] || { echo "run on an arm64 host" >&2; exit 1; }
mkdir -p "$work" "$mnt"

# --- base image, verified against Raspberry Pi's published checksum ---
url="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$base_url")"
name="$(basename "$url")"
echo "base image: $url"
if [ ! -f "$work/$name" ]; then
  curl -fL --retry 3 -o "$work/$name.part" "$url"
  mv "$work/$name.part" "$work/$name"
fi
expected="$(curl -fsSL "$url.sha256" | awk '{print $1}')"
actual="$(sha256sum "$work/$name" | awk '{print $1}')"
[ "$expected" = "$actual" ] || { echo "checksum mismatch for $name" >&2; exit 1; }

img="$work/tigomonitor-$version-arm64.img"
xz -dc "$work/$name" > "$img"

# --- room for the package and its libraries ---
truncate -s "+${grow_mb}M" "$img"
parted -s "$img" resizepart 2 100%
loop="$(losetup -fP --show "$img")"
e2fsck -pf "${loop}p2" || [ $? -le 1 ]
resize2fs "${loop}p2"

mount "${loop}p2" "$mnt"
mount "${loop}p1" "$mnt/boot/firmware"
mount --bind /dev "$mnt/dev"
mount --bind /dev/pts "$mnt/dev/pts"
mount -t proc proc "$mnt/proc"
mount -t sysfs sys "$mnt/sys"

# --- install ---
cp "$deb" "$mnt/tmp/tigomonitord.deb"
# The image's resolv.conf may be a symlink into a /run that is empty here.
mv "$mnt/etc/resolv.conf" "$mnt/etc/resolv.conf.orig" 2>/dev/null || true
cp -L /etc/resolv.conf "$mnt/etc/resolv.conf"
chroot "$mnt" /bin/sh -e <<'EOF'
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends /tmp/tigomonitord.deb avahi-daemon
apt-get clean
rm -rf /var/lib/apt/lists/* /tmp/tigomonitord.deb
test -e /etc/systemd/system/multi-user.target.wants/tigomonitord.service
# A Pi made for this has no other serial devices to guard, so read the
# first adapter it finds; a plain install waits to be told which one.
install -d -o tigomonitor -g tigomonitor -m 755 /var/lib/tigomonitor
printf '{"serial_port": "auto"}\n' > /var/lib/tigomonitor/settings.json
chown tigomonitor:tigomonitor /var/lib/tigomonitor/settings.json
chmod 600 /var/lib/tigomonitor/settings.json
EOF
rm -f "$mnt/etc/resolv.conf"
mv "$mnt/etc/resolv.conf.orig" "$mnt/etc/resolv.conf" 2>/dev/null || true

# --- defaults Imager can override ---
# http://tigomonitor.local unless the user picks another hostname in Imager.
echo tigomonitor > "$mnt/etc/hostname"
sed -i 's/^127\.0\.1\.1.*/127.0.1.1\ttigomonitor/' "$mnt/etc/hosts"

# The Pi's own UART, for RS485 HATs: on, and not used as a login console.
# USB dongles do not need this; it is harmless for them.
cfg="$mnt/boot/firmware/config.txt"
grep -q '^enable_uart=1' "$cfg" || printf '\n[all]\n# tigomonitor: RS485 HATs read the bus on the UART\nenable_uart=1\n' >> "$cfg"
sed -i -E 's/ ?console=(serial0|ttyAMA0|ttyS0),[0-9]+//' "$mnt/boot/firmware/cmdline.txt"

cat > "$mnt/boot/firmware/TIGOMONITOR.txt" <<EOF
Tigo Monitor $version

Open http://tigomonitor.local (or the hostname you chose in Raspberry Pi
Imager) once the Pi has joined your Wi-Fi, then set your MQTT broker under
Tools. Docs: https://github.com/RAR/esphome-tigomonitor
EOF

sync
cleanup
trap - EXIT
loop=""

xz -T0 -6 -f "$img"
(cd "$work" && sha256sum "$(basename "$img").xz" > "$(basename "$img").xz.sha256")
echo "$img.xz"
