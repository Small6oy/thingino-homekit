# thingino-homekit (rhkd)

Native Apple HomeKit for [Thingino](https://github.com/themactep/thingino-firmware) cameras. `rhkd` runs on the
camera and appears in the Home app as an IP camera. It needs no Homebridge, Scrypted or hub add-on.

It is built on Apple's open-source [HomeKit ADK](https://github.com/apple/HomeKitADK). The camera streaming
logic is ported from [HAP-NodeJS](https://github.com/homebridge/HAP-NodeJS). Video and audio come straight
from Thingino's raptor streamer (`rvd`, `rad`) through its shared-memory rings, so video is never re-encoded on
the camera. Recording audio is AAC-LC from one of rad's AAC outputs, or, if rad only has Opus, converted from Opus
on the camera.

## What works

- **Pairing:** with the setup code or QR code from the camera's web UI (**Services → HomeKit**). Pairings are
  stored on flash.
- **Live view:** H.264 video and Opus audio over SRTP. While Home is viewing, the substream's bitrate follows
  what iOS asks for, and the configured value is restored afterwards.
- **Snapshots** for the Home tile, from raptor's JPEG channel.
- **Motion sensor,** fed by raptor's motion detection (IVS), with a debounce.
- **Microphone mute.**
- **HomeKit Secure Video:** recordings on motion, with a 4 s prebuffer, in the Home timeline (needs an
  iCloud+ plan and a home hub). Video is raptor's main H.264 stream as is, in fragmented MP4 over the HomeKit
  Data Stream. Audio is AAC-LC 16 kHz: taken as is from a rad output that carries it (`audio3`, `audio` or
  `audio2`, picked by codec), otherwise rad's Opus stream converted on the camera (libopus + faac).
- **Camera settings in Home:** camera on/off, night vision (raptor's IR-cut daemon), status light, snapshot
  policy (Camera Operating Mode).
- **Web UI page:**
  - pairing status, setup code and QR;
  - restart, unpair and new code;
  - a codec compatibility check with a one-click fix.

Not yet: two-way audio (talking through the camera speaker).

Tested on a **Wyze Cam v4** (T41NQ) and a **Wyze Cam v3** (T31X).

## Requirements on the camera

- Thingino with the raptor streamer: `rvd` (video), `rad` (audio), and optionally `rmd` (motion).
- troglobit `mdnsd` for discovery. rhkd writes `/run/hap.service` and sends `mdnsd` a SIGHUP.
- **Codecs:** at least one video stream (main or sub) set to `codec = h264`, and Opus on one of rad's audio
  outputs in `/etc/raptor.conf`: `[audio] codec = opus`, or, with a rad that has extra outputs, for example
  `[audio2] codec = opus` (`on_demand = true` encodes it only while Home is viewing). HomeKit live view only
  takes H.264 and Opus or AAC-ELD, and raptor's AAC encoder can't produce AAC-ELD. For recordings, an AAC output
  at 16 kHz (for example `[audio3] codec = aac`, `bitrate = 32000`, on demand) saves converting Opus on the
  camera.

  `rhkd-setup fixcodecs`, or the **Make compatible** button in the web UI, sets these for you. If the
  codecs are wrong anyway, rhkd logs the reason instead of sending a stream iOS can't play.

## Build

In a Thingino firmware build, enable `BR2_PACKAGE_THINGINO_RHKD`.

Stand-alone, as a static musl mipsel binary:

```sh
./deps.sh                       # fetch HomeKitADK, mbedTLS 2.28, raptor-ipc, Opus and faac at pinned commits, apply patches/
make -C build CROSS=mipsel-linux-musl-
```

The output is `build/out/rhkd` and `build/out/hap-setupgen` (setup code generator).

**Audio options** (make variables):

| Variable | Default | |
|---|---|---|
| `TRANSCODE` | `1` | Include the Opus -> AAC fallback for recordings. `0` leaves out libopus and faac (~180 KB); recordings then need an AAC-LC 16 kHz rad output. |
| `SYSTEM_AUDIO` | `0` | `1` links the system libopus and libfaac instead of building the bundled copies. faac 1.30 and faac >= 2 (the `faac_encoder_*` API) both work. |
| `AUDIO_CFLAGS` | | Include flags for the system headers, e.g. `-I$(SYSROOT)/usr/include/opus`. |

**Match the SoC's floating-point ABI.** Thingino builds T31 (XBurst1) code FP32 with legacy NaN, and T41
(XBurst2) code **FP64 with NAN2008**. A generic FP32 toolchain works on the T31, but on the T41 its
double-precision code (the AAC encoder) breaks on worker threads. Firmware builds get this right through the
Thingino toolchain. For stand-alone builds, `build/docker-build.sh` keeps an incremental tree on a (remote)
Docker host and picks the toolchain per SoC:

```sh
docker build -t thingino-hap-build build/                                     # T31: musl.cc, FP32
docker build -t thingino-hap-build-xburst2 -f build/Dockerfile.xburst2 build/ # T41: Thingino xburst2 toolchain, FP64
build/docker-build.sh                   # -> build/out/
SOC=xburst2 build/docker-build.sh       # -> build/out-xburst2/
TRANSCODE=1 build/docker-build.sh       # docker-build.sh defaults to TRANSCODE=0
```

`docker-build.sh` needs `BUILD_HOST=user@docker-host` (SSH access to the machine your docker context points at): the build tree lives there, so only changed sources are sent and object files are kept between builds.

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
rhkd-setup fixcodecs   # if no audio output is Opus, switch [audio] to Opus; if no stream is H.264, the substream to H.264
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
