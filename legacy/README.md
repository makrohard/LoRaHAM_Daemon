# Archived legacy daemons — do not build, do not deploy

This directory holds the historical single-file daemons (`loradaemon_320_106.cpp`,
`loradaemon_320_108.cpp`). They are kept for reference only:

- They are **not** the release candidate and receive no fixes.
- They predate the hardened daemon in `../loraham_daemon/` (per-band processes,
  instance locks, CAD/LBT, framed DATA protocol, test suite).
- Defect reports against binaries built from these files are not actionable.

The historical `loraham.de` install script installs a build of this legacy code;
do not use it for current deployments either.

Current daemon, build, tests, and systemd deployment: see
[`../loraham_daemon/README.md`](../loraham_daemon/README.md).
