# Mikhlink

## Purpose

Mikhlink is an OBS Studio output plugin for Windows. Its goal is to send an OBS stream through multiple internet connections, survive individual connection failures, and deliver the bonded stream to BELABOX using a compatible transport.

## MVP

- Load as a native OBS Studio plugin.
- Detect available Windows network adapters.
- Show the state of each connection.
- Send the OBS encoded stream through multiple connections.
- Keep the stream running when one connection fails.
- Deliver the stream to BELABOX through an SRTLA-compatible transport.

## Current milestone

Build `mikhlink.dll`, load it in OBS Studio, and confirm that detected network adapters appear in the OBS log.

## Decisions

- Product type: native OBS Studio output plugin.
- Language: C++.
- Build system: CMake.
- First target platform: Windows.
- Development proceeds through small, testable milestones.
- Empty future modules are not created in advance.

## Future components

- OBS output
- Network
- Bonding
- Transport
- UI
- Configuration and logging
