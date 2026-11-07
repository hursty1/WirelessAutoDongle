#!/bin/bash

set -u
set -e
set -x

mv ${TARGET_DIR}/etc/aawgd.conf ${BINARIES_DIR}/aawgd.conf
ln -sf /boot/aawgd.conf ${TARGET_DIR}/etc/aawgd.conf

# SSH host identity belongs to this SD card, not the read-only root filesystem.
ln -snf /persist/dropbear "${TARGET_DIR}/etc/dropbear"
chmod 0755 "${TARGET_DIR}/usr/sbin/aawg-logs" "${TARGET_DIR}/etc/init.d/S03aawg_logs" "${TARGET_DIR}/etc/init.d/S94carplay_probe"

case "${AAWG_IMAGE_PROFILE:-android}" in
    android|carplay-probe)
        sed -i "s/^AAWG_MODE=.*/AAWG_MODE=${AAWG_IMAGE_PROFILE:-android}/" "${BINARIES_DIR}/aawgd.conf"
        ;;
    *) echo 'Unknown AAWG_IMAGE_PROFILE' >&2; exit 1 ;;
esac

# Never trace generated access credentials into the build log.
set +x
(
    source "${BINARIES_DIR}/aawgd.conf"
    if [ "${AAWG_ENABLE_SSH:-}" = 1 ]; then
        if [ -z "${AAWG_WIFI_PASSWORD:-}" ]; then
            credential_file="${BINARIES_DIR}/diagnostic-wifi-password"
            if [ ! -s "$credential_file" ]; then
                (umask 077; tr -dc A-Za-z0-9 < /dev/urandom | head -c 20 > "$credential_file")
            fi
            AAWG_WIFI_PASSWORD=$(cat "$credential_file")
            printf "\nAAWG_WIFI_PASSWORD='%s'\n" "$AAWG_WIFI_PASSWORD" >> "${BINARIES_DIR}/aawgd.conf"
        fi
        (umask 077; printf 'SSID: AAWirelessDongle\nWi-Fi password: %s\nSSH: ssh root@10.0.0.1\nSSH password: password\nProfile: %s; full CarPlay is not implemented.\n' "$AAWG_WIFI_PASSWORD" "${AAWG_MODE:-android}" > "${BINARIES_DIR}/diagnostic-access.txt")
    fi
)
set -x

printf 'source_commit=%s\n' "${AAWG_SOURCE_COMMIT:-unknown}" > "${TARGET_DIR}/etc/aawg-build"
printf 'source_dirty=%s\n' "${AAWG_SOURCE_DIRTY:-unknown}" >> "${TARGET_DIR}/etc/aawg-build"

source board/raspberrypi/post-build.sh
