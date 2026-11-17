#!/usr/bin/env bash
# Download every saved boot from the CarPlay probe in one SSH session.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
destination=${1:-"$script_dir/../logs"}
mkdir -p -- "$destination"
partial=$(mktemp "$destination/.iphone-probe-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
trap 'rm -f -- "$partial"' EXIT
archive="$destination/${partial##*/.}.tar.gz"

# Stream an uncompressed tar from the Pi and compress it on the laptop.
ssh -T -o HostKeyAlias=carplay-handshake-probe root@10.0.0.1 '
    set -e
    cd /persist/logs
    set --
    for boot in boot-*; do
        [ -d "$boot" ] && [ ! -L "$boot" ] || continue
        printf "%s\n" "$boot" | grep -Eq "^boot-[0-9]{6,}-[0-9a-f-]+$" || continue
        set -- "$@" "$boot"
    done
    if [ "$#" -eq 0 ]; then
        echo "No saved boot logs found on the Pi." >&2
        exit 1
    fi
    tar -cf - "$@"
' | gzip -c > "$partial"

tar -tzf "$partial" > /dev/null
mv -- "$partial" "$archive"
printf 'Saved all available boot logs to %s\n' "$archive"
