#!/bin/sh
# Builds tigomonitord and packages it as a .deb for the machine's own
# architecture (arm64 on a Pi or GitHub's arm runner, amd64 for testing).
# Run from the repo root inside the dev container (linux/Dockerfile.dev).
#
#   linux/packaging/build-deb.sh VERSION      e.g. 2.1.0 or 2.1.0~dev1
#
# Output: build/deb/tigomonitord_VERSION_ARCH.deb
set -eu
version="${1:?usage: build-deb.sh VERSION}"
arch="$(dpkg --print-architecture)"
root="build/deb/root"

rm -rf build/linux build/deb
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DTIGOD_VERSION="$version"
cmake --build build/linux -j"$(nproc)"
DESTDIR="$PWD/$root" cmake --install build/linux --strip

install -Dm644 linux/packaging/tigomonitord.service "$root/usr/lib/systemd/system/tigomonitord.service"
install -Dm644 linux/packaging/tigomonitor-http.service "$root/etc/avahi/services/tigomonitor-http.service"
install -Dm644 LICENSE "$root/usr/share/doc/tigomonitord/copyright"

# Runtime library dependencies, the way debhelper would work them out.
mkdir -p build/deb/shlibs/debian
printf 'Source: tigomonitord\n\nPackage: tigomonitord\nArchitecture: any\n' > build/deb/shlibs/debian/control
deps="$(cd build/deb/shlibs && dpkg-shlibdeps -O "$OLDPWD/$root/usr/bin/tigomonitord" | sed 's/^shlibs:Depends=//')"

mkdir -p "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<EOF
Package: tigomonitord
Version: $version
Architecture: $arch
Maintainer: Andrew Rankin <andrew@eiknet.com>
Depends: $deps, ca-certificates
Recommends: avahi-daemon
Section: misc
Priority: optional
Homepage: https://github.com/RAR/esphome-tigomonitor
Description: Tigo solar optimizer monitor for Home Assistant
 Listens to a Tigo TAP's RS485 bus and publishes every optimizer's power,
 voltage, current and temperature to Home Assistant over MQTT discovery,
 with the same web dashboard as the ESP32 build of esphome-tigomonitor.
EOF
printf '/etc/avahi/services/tigomonitor-http.service\n' > "$root/DEBIAN/conffiles"

cat > "$root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -d /run/systemd/system ]; then
  systemctl daemon-reload
  systemctl enable tigomonitord.service >/dev/null
  systemctl restart tigomonitord.service
elif [ "$1" = configure ]; then
  # No running systemd: an image build in a chroot. Enable for first boot.
  if ! systemctl enable tigomonitord.service >/dev/null 2>&1; then
    mkdir -p /etc/systemd/system/multi-user.target.wants
    ln -sf /usr/lib/systemd/system/tigomonitord.service /etc/systemd/system/multi-user.target.wants/tigomonitord.service
  fi
fi
EOF
cat > "$root/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ] && [ "$1" = remove ]; then
  systemctl disable --now tigomonitord.service >/dev/null || true
fi
EOF
cat > "$root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then systemctl daemon-reload || true; fi
# Settings and history stay unless purged.
if [ "$1" = purge ]; then rm -rf /var/lib/tigomonitor /var/lib/private/tigomonitor; fi
EOF
chmod 755 "$root/DEBIAN/postinst" "$root/DEBIAN/prerm" "$root/DEBIAN/postrm"

out="build/deb/tigomonitord_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$root" "$out"
echo "$out"
