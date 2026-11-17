# thingino-homekit (rhkd)

Native Apple HomeKit for [Thingino](https://github.com/themactep/thingino-firmware) cameras. `rhkd` runs on the
camera and appears in the Home app as an IP camera. It needs no Homebridge, Scrypted or hub add-on.

It is built on Apple's open-source [HomeKit ADK](https://github.com/apple/HomeKitADK). The camera streaming
logic is ported from [HAP-NodeJS](https://github.com/homebridge/HAP-NodeJS). Video and audio come straight
from Thingino's raptor streamer (`rvd`, `rad`) through its shared-memory rings, so nothing is re-encoded on
the camera.

## What works

- **Pairing:** with the setup code or QR code from the camera's web UI (**Services → HomeKit**). Pairings are
  stored on flash.
- **Live view:** H.264 video and Opus audio over SRTP. While Home is viewing, the substream's bitrate follows
  what iOS asks for, and the configured value is restored afterwards.
- **Snapshots** for the Home tile, from raptor's JPEG channel.
- **Motion sensor,** fed by raptor's motion detection (IVS), with a debounce.
- **Microphone mute.**
- **Web UI page:**
  - pairing status, setup code and QR;
  - restart, unpair and new code;
  - a codec compatibility check with a one-click fix.

Not yet: two-way audio (talking through the camera speaker) and HomeKit Secure Video recording.

Tested on a **Wyze Cam v4** (T41NQ) and a **Wyze Cam v3** (T31X).

## Requirements on the camera

- Thingino with the raptor streamer: `rvd` (video), `rad` (audio), and optionally `rmd` (motion).
- troglobit `mdnsd` for discovery. rhkd writes `/run/hap.service` and sends `mdnsd` a SIGHUP.
- **Codecs:** at least one video stream (main or sub) set to `codec = h264`, and `[audio] codec = opus` in
  `/etc/raptor.conf`. HomeKit live view only takes H.264 and Opus or AAC-ELD, and raptor's AAC encoder
  can't produce AAC-ELD.

  `rhkd-setup fixcodecs`, or the **Make compatible** button in the web UI, sets these for you. If the
  codecs are wrong anyway, rhkd logs the reason instead of sending a stream iOS can't play.

## Build

In a Thingino firmware build, enable `BR2_PACKAGE_THINGINO_RHKD`.

Stand-alone, as a static musl mipsel binary:

```sh
./deps.sh                       # fetch HomeKitADK, mbedTLS 2.28 and raptor-ipc at pinned commits, apply patches/
make -C build CROSS=mipsel-linux-musl-
# or in Docker (also works with a remote Docker context):
docker build -t thingino-hap-build build/ && build/docker-build.sh
```

The output is `build/out/rhkd` and `build/out/hap-setupgen` (setup code generator).

## Install on a running camera (development)

```sh
./install.sh <camera-ip>        # set SSHPASS to use password SSH via sshpass
```

This installs the binary, the init script `S98rhkd`, the settings file `/etc/default/rhkd`, the `rhkd-setup`
helper and the web UI page. If there is no pairing store yet, it creates one. It also runs
`rhkd-setup fixcodecs`. Logs go to syslog: `logread | grep rhkd`.

## Settings (`/etc/default/rhkd`)

| Variable | Default | Meaning |
|---|---|---|
| `RHKD_ENABLED` | `true` | Start at boot |
| `RHKD_NAME` | hostname | Name shown when adding the camera |
| `RHKD_STORE` | `/etc/homekit/store` | Pairing store (keys and pairings) |
| `RHKD_LOG_LEVEL` | `2` | 0 none, 1 default, 2 info, 3 debug |
| `RHKD_VIDEO_RING` | `auto` | `auto` (by requested bitrate), `main` or `sub` |
| `RHKD_SNAPSHOT_RING` | `jpeg1` | JPEG channel used for snapshots |
| `RHKD_MOTION_MIN_HITS` / `_WINDOW_MS` / `_HOLD_SEC` | `2` / `3000` / `10` | Motion debounce |
| `RHKD_AUDIO_CLOCK` | negotiated rate | Opus RTP clock; `48000` for RFC 7587 (HomeKit stutters with it) |
| `RHKD_ADAPT_BITRATE` | `true` | Substream bitrate follows iOS during live view |
| `RHKD_MIN_BITRATE_KBPS` | `300` | Lower limit for that |

## Pairing helper

```sh
rhkd-setup status      # JSON: running, paired, people with access, setup code, codecs
rhkd-setup unpair      # forget all pairings (keeps the setup code)
rhkd-setup newcode     # new setup code (also unpairs)
rhkd-setup codecs      # current codecs and whether HomeKit can use them
rhkd-setup fixcodecs   # switch audio to Opus and, if no stream is H.264, the substream to H.264
```

## Notes

- Accessories built with the open-source ADK aren't MFi-certified, so the Home app shows an "uncertified
  accessory" warning when you add the camera. The ADK is intended for non-commercial use.
- `src/rhkd/version.h` is reported as the accessory firmware revision. It must only ever go up: the ADK refuses to
  start after a downgrade.
- `patches/HomeKitADK/` holds our changes to the archived ADK:
  - camera snapshot support (`POST /resource`);
  - a `/dev/urandom` fallback on kernels without `getrandom` (T31, kernel 3.10).

## License

- rhkd: **GPL-3.0-or-later** ([LICENSE](LICENSE)). It links raptor-ipc (GPL-3.0).
- Apache-2.0, marked per file: the files adapted from HomeKit ADK and HAP-NodeJS, and the
  `pal-thingino` platform layer.
- MIT: `files/var/www/a/vendor/qrcode.js` (qrcode-generator 1.4.4).
- Fetched by `deps.sh`: HomeKit ADK and mbedTLS (Apache-2.0), raptor-ipc (GPL-3.0).
