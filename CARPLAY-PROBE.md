# iPhone Bluetooth/iAP2 handshake probe

This profile attempts the phone-facing Bluetooth/iAP2 link and records the last
completed stage. It does not implement MFi authentication, wireless CarPlay
handover/media, or the truck-side USB CarPlay endpoint. No CarPlay screen or
"Use CarPlay" prompt is promised by this test.

The existing Android diagnostic image can still be used to capture truck logs.
Keep it separately; this probe profile skips the Android daemon and USB gadgets.

## Build

```sh
docker compose run --rm bash sh /src/support/build-carplay-probe.sh
```

The distinctly named folder under `images/CarPlay-Handshake-Probe-.../` contains
the image, SHA-256, version, private Wi-Fi/SSH details, and these test procedures.
Image source metadata distinguishes clean and uncommitted source builds.

`AAWG_MODE=carplay-probe` selects this profile. Normal Android images retain
`AAWG_MODE=android`; mode selection occurs at boot. This is explicit mode
selection, not automatic switching between phones.

## Brief test (at home; no truck needed)

1. Power the probe image. Keep the Android phone from automatically connecting.
2. Connect the Linux laptop to `AAWirelessDongle` using this image's access file;
   log in with `ssh -o HostKeyAlias=carplay-handshake-probe root@10.0.0.1`.
   This keeps the new card's SSH identity separate from the Android card's.
   Run `aawg-logs mark iphone-probe-test`.
3. On iPhone 13 Pro Max, open Bluetooth settings and pair with `CarPlay-Probe-*`.
   A cached name from this Pi may still appear; record what the phone displays.
4. Wait about 30 seconds after the pairing attempt. Note any phone message.
5. Run `aawg-logs list`, record the boot name, then download that boot as before:

```bash
mkdir -p ~/dongle-logs
cd ~/dongle-logs
BOOT='paste-the-exact-probe-boot-name-here'
ssh -o HostKeyAlias=carplay-handshake-probe root@10.0.0.1 "aawg-logs export $BOOT" > "$BOOT.tar.gz"
tar -xzf "$BOOT.tar.gz"
cat "$BOOT/summary.txt"
cat "$BOOT/stages.tsv"
grep -F 'CarPlay probe' "$BOOT/system.log"
```

Do not reflash before exporting. A failure or missing RFCOMM connection is a
useful result; record it rather than treating ordinary Bluetooth pairing as
successful CarPlay. Send the archive plus the exact iOS version and phone message.
If the phone pairs but no RFCOMM event appears, also run `bluetoothctl devices
Paired` on the Pi to check whether BlueZ recorded the pairing.
The iPad may support generic iAP accessory communication, but is not a CarPlay
source substitute; use the iPhone for this milestone.

## Interpreting results

| Last stage | What it establishes |
| --- | --- |
| `probe_startup FAIL` | Local service/profile setup failed; see the D-Bus error |
| `rfcomm_wait START` | Probe is listening; no actual iAP2 transport has been observed |
| `rfcomm OK` | BlueZ delivered a paired Bluetooth RFCOMM connection; this is not Apple accessory authentication |
| `iap2_detection OK` | Phone returned the iAP2 support marker |
| `iap2_link OK` | Link synchronization and acknowledgement were observed |
| `authentication_request OK` then `authentication_provider BLOCKED` | Phone asked for the accessory certificate; no provider is configured, so this probe stops without sending one |
| `identification_request OK` then `identification BLOCKED` | Phone asked for identification; this probe stops at that unimplemented stage |
| `TIMEOUT`, `DISCONNECTED`, or `FAIL` | Check the named stage and error; there is no inferred root-cause claim |

There is one bounded connection worker; later retries receive a fresh attempt
number in the same boot. The final actionable stage is retained in the summary.
Payloads, certificates, and challenge contents are not dumped into logs.

## Implementation sources and limits

The probe code in this repository is written independently in C++. No source
from the unlicensed reference repository or Apple SDK has been imported.
Protocol constants/structure were checked against the public primary research
[iAP2 explanation](https://wiomoc.de/misc/posts/mfi_iap.html) and
[reference implementation](https://github.com/signalius/iap2/tree/ffcc89a852670e9c8f15002f6643b31c8175c755).
Bluetooth profile registration follows the [BlueZ Profile API](https://bluez.readthedocs.io/en/latest/profile-api/).

This is a limited diagnostic link implementation, not a general iAP2 stack:
bounded stream parsing, synchronization, acknowledgements, and observation of
initial control messages. Identification, authentication provider integration,
CarPlay-specific EIR/Bonjour handover, and USB bridging remain separate work.
An actual iPhone test is required to validate whether this minimal discovery
profile is sufficient to trigger RFCOMM on the installed iOS version.
