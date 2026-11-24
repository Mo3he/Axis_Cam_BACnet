# BACnet ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_BACnet?style=flat)](https://github.com/Mo3he/Axis_Cam_BACnet/releases)
[![Build](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_BACnet?style=flat)](LICENSE)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_BACnet/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

Publishes Axis camera events as a BACnet/IP device, so a building management
system (BMS) can read I/O, tamper, analytics, storage and other camera states
as standard BACnet objects. Built on
[bacnet-stack](https://github.com/bacnet-stack/bacnet-stack).

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB. Use at your own risk.

> **Beta:** a read-only BACnet/IP server with a deliberately small scope (see
> [Limitations](#limitations)). Feedback from anyone running it against a BMS
> is very welcome in [Issues](https://github.com/Mo3he/Axis_Cam_BACnet/issues).

## Overview

- The camera appears on the BACnet network as a Device object with Analog,
  Binary and Multi-state Value objects.
- Each object follows one field of a camera event: for example a Binary Value
  that is active while virtual input 1 is on, or a Multi-state Value that shows
  the continuous recording status.
- Objects are declared on the app's settings page, which lists the events the
  camera actually declares. Values are read-only over BACnet.
- Change-of-value (COV) subscriptions deliver changes without polling.
- Settings are kept in the camera parameter store and the object list in the
  app's data directory; both survive restarts and upgrades.

## Compatibility

| Package | AXIS OS | Architecture | Status |
|---|---|---|---|
| ACAP 4 | 11.11 - 13 | aarch64 | Beta, tested on AXIS OS 12.11 |
| ACAP 4 | 11.11 - 13 | armv7hf | Beta, built but not yet tested on a device |

The app runs as the unprivileged `sdk` user. It does not require root access.

## Installation

Download the EAP matching the camera architecture from the
[Releases page](https://github.com/Mo3he/Axis_Cam_BACnet/releases), install it
from **Apps -> Add app** in the camera web interface and start it. Then open
**Apps -> BACnet Server -> Open**, add objects and enable the device. The
device is disabled after installation, so nothing is published on the network
until it is turned on.

## Configuration

### Device

| Setting | Default | Description |
|---|---|---|
| Enabled | off | Starts the BACnet/IP device. |
| Device instance | automatic | Unique on the BACnet network. Empty derives it from the last six hex digits of the serial number. |
| Device name | model and serial number | Unique on the BACnet network. |
| UDP port | 47808 | BACnet/IP port, 1024 to 65535. |
| Location, Description | empty | Shown in the Device object. |

Model_Name, Serial_Number and Firmware_Revision come from the camera, and
Application_Software_Version from the app.

### Objects

Each object has a type, an instance, a unique name and a camera event:

- **Camera event:** chosen from the events the camera declares.
- **Only when field ... equals:** an optional filter on a source field, for
  example `port` equals `1` for virtual input 1.
- **Value from field:** the data field that carries the value. Fields marked
  "state" are stateful: the object gets the current value as soon as it is
  saved.

How values are converted:

| Object | From | Rule |
|---|---|---|
| Analog Value | number, boolean or numeric text | The number; true is 1. Units and COV increment are configurable. |
| Binary Value | boolean, number or text | True, non-zero, `on`, `active`, `yes` or `high` is active; can be inverted. Active and inactive texts are configurable. |
| Multi-state Value | text or number | Text matches a state name (case-insensitive); a number is the 1-based state. |

A value that cannot be converted leaves the present value unchanged and is
shown on the settings page. Until an object receives its first event value it
reports Reliability NO_SENSOR (Status_Flags FAULT), so a BMS does not take its
initial value for a reading. An object whose event subscription fails reports
Reliability CONFIGURATION_ERROR.

The object list can also be read and written as JSON at
`/local/BACnet_Server/api/objects`:

```json
{
  "objects": [
    {
      "type": "binary-value",
      "instance": 1,
      "name": "Virtual input 1",
      "active_text": "On",
      "inactive_text": "Off",
      "event": {
        "topic": ["tns1:Device", "tnsaxis:IO", "tnsaxis:VirtualInput"],
        "source_key": "port",
        "source_value": "1",
        "data_key": "active"
      }
    }
  ]
}
```

## Ports & security

- **When enabled, the app listens on UDP 47808 (or the configured port) on the
  camera's network interface.** BACnet/IP has no authentication: any host that
  can reach the port can discover the device and read the published values.
  Publish only what may be read on that network, and keep BACnet on a building
  automation VLAN.
- Nothing can be changed over BACnet: every property is read-only, and
  WriteProperty is not a supported service.
- The settings and status API binds `127.0.0.1:2213` and is reachable only
  through the camera's authenticated reverse proxy, restricted to `admin`.
- The device is disabled by default.

## How it works

The app runs bacnet-stack in the GLib main loop together with the camera event
subscriptions, so events and BACnet requests are handled on one thread. Each
object subscribes to its camera event through the ACAP event API and updates
its present value when a matching event arrives. Database_Revision increases
whenever the device settings or the object list change.

## BACnet conformance

Profile BACnet Smart Sensor (B-SS) with DS-RP-B, DS-RPM-B, DS-COV-B, DM-DDB-B
and DM-DOB-B, protocol revision 28, BACnet/IP only. The full draft PICS is in
[docs/PICS.md](docs/PICS.md).

Tested on an AXIS P3288-LV (AXIS OS 12.11.77) with the bacnet-stack
command-line tools and with bacpypes3:

- discovery with Who-Is, and reading every property of the Device, Analog,
  Binary and Multi-state Value objects;
- COV notifications for virtual input changes, delivered within a second;
- writes to present values, Out_Of_Service and the device name rejected;
- Database_Revision unchanged across a restart and increased by configuration
  changes.

## Limitations

- Read-only: no commandable objects and no WriteProperty.
- BACnet/IP only, without BBMD or foreign device registration, so discovery
  needs the BMS on the same subnet or a BBMD on the network.
- The Device object uses bacnet-stack's default vendor identifier (260)
  unless the package is built with its own (see
  [Build from source](#build-from-source)). A released product needs a vendor
  identifier from ASHRAE, and some projects require BTL certification, which
  this beta does not have.
- Occupancy works with an AXIS Object Analytics "occupancy in area" scenario:
  bind an Analog Value to the `human` or `total` field of its event. These
  fields are not stateful, so the object reports NO_SENSOR until the
  scenario's next event, which took up to about 25 seconds in testing.
  Line-crossing count scenarios are not tested.

## Build from source

Docker or Podman is required; the Axis ACAP Native SDK image does the build and
downloads the pinned bacnet-stack release.

```sh
./build.sh                 # aarch64 and armv7hf into ./releases
ARCHES=aarch64 ./build.sh  # a single architecture
```

The BACnet vendor identifier and name are build settings:

```sh
BACNET_VENDOR_ID=1234 BACNET_VENDOR_NAME="Example Corp" ./build.sh
```

### Tests

`tests/run.sh` builds and runs the host unit tests for the object declarations
and value conversion, with AddressSanitizer and UBSan.

### CI

Every push builds both architectures and uploads the packages as workflow
artifacts. Pushing a `v*` tag creates a release with the packages attached.

## Links

- Releases: <https://github.com/Mo3he/Axis_Cam_BACnet/releases>
- Issues: <https://github.com/Mo3he/Axis_Cam_BACnet/issues>
- PICS: [docs/PICS.md](docs/PICS.md)
- bacnet-stack: <https://github.com/bacnet-stack/bacnet-stack>
- ACAP documentation: <https://developer.axis.com/acap/>
- Axis Communications: <https://www.axis.com/>

## License

The packaging and app code in this repository is licensed under BSD 3-Clause
(see [LICENSE](LICENSE)). Bundled upstream components (bacnet-stack, cJSON) are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
