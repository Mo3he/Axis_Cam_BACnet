# Third-party notices

The packaging and app code in this repository is licensed under the BSD 3-Clause
License; bundled upstream components retain their own licenses. The full
license texts ship inside every package as `THIRD_PARTY_LICENSES.txt`
(source: [app/THIRD_PARTY_LICENSES.txt](app/THIRD_PARTY_LICENSES.txt)).

## bacnet-stack

- Project: <https://github.com/bacnet-stack/bacnet-stack>
- Version: 1.7.0, built unmodified from the release tarball and statically
  linked (see `Dockerfile`; the tarball is checked against a pinned SHA-256).
- Licenses, as stated by the SPDX identifier in each linked source file:
  - `GPL-2.0-or-later WITH GCC-exception-2.0`: protocol encoding, data link and
    transaction handling. The exception gives unlimited permission to link
    these files into other programs and distribute them without restriction
    from these files. Modified copies of the files themselves would remain
    under the GPL; this repository makes no modifications.
  - `MIT`: Device, object and service handler files.
  - `BSD-3-Clause`: `src/bacnet/basic/sys/mstimer.c`.
  - `CC-PDDC` (public domain): `days.c`, `dst.c` and `color_rgb.c` in
    `src/bacnet/basic/sys`.

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
