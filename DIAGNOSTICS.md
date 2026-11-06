# Android Auto logging test image

This development image adds persistent per-boot logs and Wi-Fi/SSH retrieval to
the working Android Auto configuration. **It does not implement CarPlay yet.**
The first test verifies logging and retrieval before CarPlay changes are added.

## Build

```sh
docker compose run --rm bash sh /src/support/build-diagnostics.sh
```

Results go into a new `images/diagnostics/<build-id>/` folder: `sdcard.img`,
checksum, manifest, and private `diagnostic-access.txt`. The existing recovery
image is not overwritten. This logging-only build reuses the Android kernel
cache; future CarPlay kernel/profile changes need a separate build target.

When SSH is enabled without an explicit Wi-Fi password, the build generates an
access-point password and includes it in the card's `aawgd.conf`. Incremental
builds retain that password in the Linux output volume. Read the access file
next to the experimental image for Wi-Fi and SSH details; do not share it with
test logs. The current root login password is `password`. SSH host keys are
generated per card and stored under `/persist/dropbear`.

## Brief test: logs survive power-off

1. Power the experimental Pi. Connect the laptop to `AAWirelessDongle`, using
   the Wi-Fi password in `diagnostic-access.txt`. Accept staying connected when
   Windows reports that the network has no Internet.
2. From PowerShell: `ssh root@10.0.0.1`. Confirm the host key on the first
   connection; use the SSH password in the access file.
3. Run `aawg-logs mark first-power-test`, then `aawg-logs list`. Note the current
   `boot-...` name and verify `persistent=yes`.
4. Power off, then power on again. Reconnect and run `aawg-logs list`. There
   should be a new current boot and the previous boot with your marker.
5. Download the previous boot using the commands below. A real sudden-power-loss
   test is required: host-side tests cannot establish SD-card durability.

## Brief test: truck attempt

1. Use the verified Pi data cable and the truck's working Android Auto port.
2. Attempt Android Auto once, noting any truck/phone message. This image checks
   diagnostic capture; pairing an iPhone does not test CarPlay support.
3. Power the Pi again at home and connect the laptop to its Wi-Fi.
4. List and download the earlier truck boot. The home boot has a different name.

## Download logs from PowerShell

From the repository directory, list saved boots:

```powershell
.\support\Get-DongleLogs.ps1
```

Copy the exact boot name for the attempt, then download it:

```powershell
.\support\Get-DongleLogs.ps1 -Boot boot-000001-REPLACE-WITH-THE-EXACT-ID
```

The helper writes a `.tar.gz` to the local ignored `planning/results/` folder,
without PowerShell text conversion. It lists the archive's SHA-256 after a
successful download. A failed export leaves a `.partial` file; remove that
specific partial file before retrying the same export. Root access does not
require a home router. Keep the Pi connected to the laptop's diagnostic network
until the download finishes.

## What is captured

- `boot.txt`: identity, source commit/dirty flag, profile, and persistence status.
- `system.log` and optional `.0`: syslog, kernel, Android daemon, and hostapd.
- `kernel-start.txt`: bounded early kernel ring-buffer snapshot.
- `snapshot.txt`: interfaces, routes, storage, and USB controller state.
- `stages.tsv`: explicit helper milestones, tab-separated as elapsed uptime,
  boot UUID, attempt ID, endpoint, stage, outcome, detail.
- `summary.txt`: latest explicit helper milestone. This is not yet an automatic
  summary of Android/CarPlay handshakes; read the protocol messages in system.log.

Each OS boot creates a new folder. Service restarts keep the current folder.
The last five boots are retained, with size limits and an 8 MiB free-space
reserve; low space/missing persistent storage falls back to RAM and reports it.
System logs rotate at 768 KiB with one backup; stage logs rotate at 128 KiB with
one backup. Earlier detail can be evicted during unusually chatty attempts.

Periodic synchronization targets a two-second interval; explicit helper stage
events also synchronize. Sudden power loss may still lose recent data or
damage storage. Very early boot failures can happen before logging starts.
Logs survive reboot, but not reflashing the whole card. Export before reflashing.

This is the first diagnostic milestone. Phone/truck CarPlay endpoint code and
its authentication path remain separate work.
