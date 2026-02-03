#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
VERSION=$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")
DIST_DIR="$ROOT_DIR/dist"
STAGE_DIR="$DIST_DIR/stage"

chmod +x "$ROOT_DIR/tools/version"

rm -rf "$DIST_DIR"
mkdir -p "$STAGE_DIR"

cd "$ROOT_DIR"

autoreconf -fi
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var
make -j"$(nproc)"

make DESTDIR="$STAGE_DIR" install

install -d "$STAGE_DIR/etc/bird"
install -m 0644 "$ROOT_DIR/packaging/config/bird.conf" "$STAGE_DIR/etc/bird/bird.conf"

install -d "$STAGE_DIR/lib/systemd/system"
install -m 0644 "$ROOT_DIR/packaging/systemd/bird.service" "$STAGE_DIR/lib/systemd/system/bird.service"

# Raw binary artifact
install -d "$DIST_DIR/bin"
install -m 0755 "$STAGE_DIR/usr/sbin/bird" "$DIST_DIR/bin/bird"
install -m 0755 "$STAGE_DIR/usr/sbin/birdc" "$DIST_DIR/bin/birdc"
install -m 0755 "$STAGE_DIR/usr/sbin/birdcl" "$DIST_DIR/bin/birdcl"

# Zip binaries
BIN_ZIP="$DIST_DIR/bird-${VERSION}-bin.zip"
BIN_NAMES=()
for bin in bird birdc birdcl birdd; do
  if [ -f "$DIST_DIR/bin/$bin" ]; then
    BIN_NAMES+=("$bin")
  fi
done
if [ "${#BIN_NAMES[@]}" -eq 0 ]; then
  echo "No binaries found to zip" >&2
  exit 1
fi
(cd "$DIST_DIR/bin" && zip -9 -j "$BIN_ZIP" "${BIN_NAMES[@]}")

# Build .deb
DEB_DIR="$DIST_DIR/deb"
mkdir -p "$DEB_DIR/DEBIAN"
cat > "$DEB_DIR/DEBIAN/control" <<CONTROL
Package: bird
Version: $VERSION
Section: net
Priority: optional
Architecture: amd64
Maintainer: bird-ci <ci@example.invalid>
Description: BIRD Internet Routing Daemon with SHM exporter
CONTROL

cp -a "$STAGE_DIR"/* "$DEB_DIR"/

dpkg-deb --build "$DEB_DIR" "$DIST_DIR/bird-${VERSION}-amd64.deb"

# Build .rpm
RPM_TOP="$DIST_DIR/rpmbuild"
RPM_SPEC="$RPM_TOP/SPECS/bird.spec"
RPM_FILELIST="$RPM_TOP/SPECS/filelist"
mkdir -p "$RPM_TOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

find "$STAGE_DIR" -mindepth 1 -printf "/%P\n" | sort -u > "$RPM_FILELIST"

cat > "$RPM_SPEC" <<SPEC
Name: bird
Version: $VERSION
Release: 1%{?dist}
Summary: BIRD Internet Routing Daemon with SHM exporter
License: GPLv2
BuildArch: x86_64

%description
BIRD Internet Routing Daemon with SHM exporter.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a "$STAGE_DIR"/* %{buildroot}/

%files -f $RPM_FILELIST

%post
systemctl daemon-reload >/dev/null 2>&1 || true

%preun
if [ \$1 -eq 0 ]; then
  systemctl daemon-reload >/dev/null 2>&1 || true
fi
SPEC

rpmbuild -bb "$RPM_SPEC" --define "_topdir $RPM_TOP" --define "_rpmdir $DIST_DIR" >/dev/null

mapfile -d '' RPM_FILES < <(find "$DIST_DIR" -maxdepth 2 -type f -name '*.rpm' -print0)
if [ "${#RPM_FILES[@]}" -eq 0 ]; then
  echo "No RPM artifacts produced" >&2
  exit 1
fi
for rpm_file in "${RPM_FILES[@]}"; do
  if [ "$(dirname "$rpm_file")" != "$DIST_DIR" ]; then
    cp -a "$rpm_file" "$DIST_DIR/"
  fi
done
