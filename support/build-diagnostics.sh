#!/bin/sh
# Run through docker compose's bash service; reuse the unchanged Android kernel cache.
set -eu
export AAWG_IMAGE_PROFILE=android

board=raspberrypi0w
build_id="$(date -u +%Y%m%dT%H%M%SZ)-$(printf '%.12s' "${AAWG_SOURCE_COMMIT:-unknown}")"
parent=/app/images/diagnostics
mkdir -p "$parent"
destination=$parent/$build_id
mkdir "$destination"

make BR2_EXTERNAL=../aa_wireless_dongle/ O=output/$board ${board}_defconfig
make -C output/$board aawg-rebuild
make -C output/$board
binary=output/$board/images
cp "$binary/sdcard.img" "$destination/sdcard.img"
cp "$binary/diagnostic-access.txt" "$destination/diagnostic-access.txt"
(
    cd "$destination"
    sha256sum sdcard.img > sdcard.img.sha256
)
printf 'build_id=%s\nprofile=android-logging\nboard=%s\nsource_commit=%s\nsource_dirty=%s\nCarPlay=not implemented\n' \
    "$build_id" "$board" "${AAWG_SOURCE_COMMIT:-unknown}" "${AAWG_SOURCE_DIRTY:-unknown}" > "$destination/manifest.txt"
echo "Diagnostic image: $destination/sdcard.img"
echo "Access details: $destination/diagnostic-access.txt (keep private)"
