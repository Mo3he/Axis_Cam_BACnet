# Changelog

## Unreleased

- Fix: 0.1.1 could fail to start on AXIS OS releases with GLib older than
  2.76, because it used `g_string_free_and_steal`.
- Saving the objects or device settings took about 2 seconds per object,
  because event subscriptions were removed synchronously on the main loop. They
  are now removed asynchronously and a save takes well under a second.

## 0.1.1 - 2026-11-24

- Release 0.1.1.

## 0.1.0

- First beta: read-only BACnet/IP device (protocol revision 28) with Analog,
  Binary and Multi-state Value objects bound to camera events, COV
  subscriptions, and a settings page with an event picker. See docs/PICS.md.
