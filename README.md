# BACnet ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_BACnet?style=flat)](https://github.com/Mo3he/Axis_Cam_BACnet/releases)
[![Build](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_BACnet?style=flat)](LICENSE)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

Exposes Axis camera events as a BACnet/IP device for building management systems.

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB. Use at your own risk.

## Overview

- Describe the main capability here.
- Describe what the app shows in the camera UI.
- Describe where state is persisted.

## Compatibility

| Package | AXIS OS | Architecture | Status |
|---|---|---|---|
| ACAP 4 | 11.11 - 13 | aarch64 | Supported |
| ACAP 4 | 11.11 - 13 | armv7hf | Supported |

The app runs as the unprivileged `sdk` user. It does not require root access.

## Installation

Download the EAP matching the camera architecture from the
[Releases page](https://github.com/Mo3he/Axis_Cam_BACnet/releases), then:

1. Open the camera web interface.
2. Go to **Apps -> Add app**.
3. Upload the EAP and start the app.
4. Open the app settings and configure it.

## Configuration

Settings are stored in the camera's parameter store and are available from
**Apps -> BACnet Server -> Settings**.

| Parameter | Default | Description |
|---|---|---|
| `Enabled` | `no` | Starts the service when set to `yes`. |
| `ServerUrl` | empty | Endpoint the app connects to. |
| `ApiKey` | empty | Credential used to authenticate. Stored write-only. |

The settings page reads and writes through the app's own endpoint at
`/local/BACnet_Server/api/settings`, served by the app itself and reachable only
through the camera's authenticated reverse proxy. Nothing depends on
`/axis-cgi/param.cgi`, which recorder, NVR, and access-control class devices do
not expose, so settings persist on every device class. `param.cgi` is used only
as a fallback if the app's settings server is unavailable.

## Ports & security

- The app opens no ports on the camera's external interfaces.
- The settings endpoint binds `127.0.0.1:2213` and is reachable only
  through the camera's authenticated reverse proxy, restricted to `admin`.
- Credentials are stored in the camera parameter store as `password` type and are
  never returned by the settings endpoint.

## Build from source

Docker or Podman is required; the Axis ACAP Native SDK image does the build.

```sh
./build.sh                 # aarch64 and armv7hf into ./releases
ARCHES=aarch64 ./build.sh  # a single architecture
```

### CI

Every push builds both architectures and uploads the packages as workflow
artifacts. Pushing a `v*` tag creates a release with the packages attached.

## Links

- Releases: <https://github.com/Mo3he/Axis_Cam_BACnet/releases>
- Issues: <https://github.com/Mo3he/Axis_Cam_BACnet/issues>
- ACAP documentation: <https://developer.axis.com/acap/>
- Axis Communications: <https://www.axis.com/>

## License

The packaging and app code in this repository is licensed under BSD 3-Clause
(see [LICENSE](LICENSE)). Bundled upstream components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
