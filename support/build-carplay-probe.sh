#!/bin/sh
# Phone-facing handshake probe only; no truck USB endpoint or media session.
set -eu
export AAWG_IMAGE_PROFILE=carplay-probe
board=raspberrypi0w
revision=$(printf '%.7s' "${AAWG_SOURCE_COMMIT:-unknown}")
build_id="CarPlay-Handshake-Probe-$revision-PiZeroW-$(date -u +%Y%m%dT%H%M%SZ)"
destination=/app/images/$build_id
mkdir "$destination"
make BR2_EXTERNAL=../aa_wireless_dongle/ O=output/$board ${board}_defconfig
make -C output/$board aawg-rebuild
make -C output/$board
cp output/$board/images/sdcard.img "$destination/$build_id.img"
cp output/$board/images/diagnostic-access.txt "$destination/diagnostic-access.txt"
cp /app/CARPLAY-PROBE.md "$destination/TEST-PROCEDURES.md"
(
    cd "$destination"
    sha256sum "$build_id.img" > "$build_id.img.sha256"
)
printf 'build_id=%s\nprofile=carplay-probe\nboard=%s\nsource_commit=%s\nsource_dirty=%s\nCarPlay=Bluetooth/iAP2 handshake probe only; no authentication provider or truck session\n' \
    "$build_id" "$board" "${AAWG_SOURCE_COMMIT:-unknown}" "${AAWG_SOURCE_DIRTY:-unknown}" > "$destination/VERSION.txt"
echo "Probe image: $destination/$build_id.img"
echo "Private access details: $destination/diagnostic-access.txt"
