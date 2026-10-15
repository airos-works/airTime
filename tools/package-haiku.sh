#!/usr/bin/env bash
# Builds the Haiku package (.hpkg) on Haiku. Run from the repository root.
#   tools/package-haiku.sh            uses build-haiku/airTime (runs make)
# The architecture comes from resources/airTime.PackageInfo; the arm64
# package is made by tools/build-arm64.sh instead, on the build host.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
NAME=$(awk '$1 == "name" { print $2; exit }' resources/airTime.PackageInfo)
VERSION=$(awk '$1 == "version" { print $2; exit }' resources/airTime.PackageInfo)
ARCH=$(awk '$1 == "architecture" { print $2; exit }' resources/airTime.PackageInfo)
FILE="$ROOT/artifacts/$NAME-$VERSION-$ARCH.hpkg"
make -j8 "$@"
STAGE=$(mktemp -d /tmp/airtime-package-XXXXXX)
trap 'rm -rf -- "$STAGE"' EXIT
DOCS="$STAGE/documentation/packages/airtime"
mkdir -p "$STAGE/apps" "$DOCS" "$STAGE/data/deskbar/menu/Applications" \
	"$STAGE/boot/post-install" "$ROOT/artifacts"
cp build-haiku/airTime "$STAGE/apps/airTime"
strip --strip-debug "$STAGE/apps/airTime"
# GNU strip drops the appended Haiku resources; put them back.
xres -o "$STAGE/apps/airTime" build-haiku/airTime.rsrc
# The Quick Look add-on, where the SDK had Quick Look.
if [ -f build-haiku/airTime_quicklook ]; then
	mkdir -p "$STAGE/add-ons/quick_look"
	cp build-haiku/airTime_quicklook "$STAGE/add-ons/quick_look/airTime"
	strip --strip-debug "$STAGE/add-ons/quick_look/airTime"
fi
cp resources/airTime.PackageInfo "$STAGE/.PackageInfo"
cp resources/scripts/airtime-register-default.sh "$STAGE/boot/post-install/"
chmod 755 "$STAGE/boot/post-install/airtime-register-default.sh"
cp README.md LICENSE "$DOCS/"
ln -s ../../../../apps/airTime "$STAGE/data/deskbar/menu/Applications/airTime"
( cd "$STAGE" && mimeset --all -f --mimedb data/mime_db --mimedb /boot/system/data/mime_db apps/airTime )
rm -f "$FILE"
package create -C "$STAGE" "$FILE"
printf '%s\n' "$FILE"
