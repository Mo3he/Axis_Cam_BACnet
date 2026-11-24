# Third-party notices

The packaging and app code in this repository is licensed under the BSD 3-Clause
License; bundled upstream components retain their own licenses.

## bacnet-stack

- Project: <https://github.com/bacnet-stack/bacnet-stack>
- Version: 1.7.0, built unmodified from the release tarball (see `Dockerfile`).
- License: the protocol core is `GPL-2.0-or-later WITH GCC-exception-2.0`; the
  object, device and service handler files are `MIT`. Each source file carries
  its SPDX identifier.
- The GCC runtime library exception allows linking bacnet-stack into this app
  without the app becoming GPL. Changes to bacnet-stack's GPL files must be
  published if the package is distributed; this repository makes none.
- Statically linked into the app binary.

## cJSON

- Project: <https://github.com/DaveGamble/cJSON>
- Version: 1.7.18 (`app/cJSON.c`, `app/cJSON.h`).
- License: `MIT`.

## Axis ACAP Native SDK

- Project: <https://developer.axis.com/acap/>
- Used at build time to compile and package the application.

## Trademarks

BACnet is a registered trademark of ASHRAE. All other product names, logos and
brands are property of their respective owners. This is an independent
community project and is not affiliated with or endorsed by Axis
Communications or ASHRAE.
