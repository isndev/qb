# Changelog

All notable changes to qb are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/). See [VERSIONING.md](./VERSIONING.md) for the compatibility
policy.

## [Unreleased]

Tracks changes on the development branch that are not yet part of a tagged release.

## [2.0.0]

The 2.0 series modernizes the framework's vocabulary and hardens the runtime. Highlights below; the change
is broad, so entries are grouped rather than exhaustive.

### Added

- C++23 baseline across the framework (`CMAKE_CXX_STANDARD 23`, extensions off).
- A canonical `std::chrono` time vocabulary: `qb::duration` (nanosecond span), `qb::mono_time`
  (steady-clock instants), and `qb::wall_time` (system-clock instants), with helpers in
  `qb/include/qb/system/timestamp.h`.
- QUIC/HTTP3 transport with tri-state `QB_WITH_QUIC` auto-detection (enabled when ngtcp2 is present).
- Tree-wide position-independent code and a modernized CMake configuration (presets, install/export,
  dependency resolution with system-first fallback).

### Changed

- **Time handling migrated to the new chrono model.** All timeout, delay, interval, latency, and deadline
  APIs now take `qb::duration` or a clock-typed instant. The previous `qb::Timestamp` / `qb::Duration`
  types are removed (see *Removed*).
- CMake build modernized: corrected PIC handling, dependency lookup, and install/export of `qb::core` /
  `qb::io` targets.
- Warning hygiene: builds clean under `-Wall -Wextra -Wpedantic`.

### Removed

- `qb::Timestamp` and `qb::Duration`. Replace with `qb::wall_time` / `qb::mono_time` and `qb::duration`
  respectively. See the [migration guide](./readme/6_guides/migration_guide.md).

### Fixed

- qb-io async/transport: use-after-free on protocol switch; stop the I/O watcher before the transport file
  descriptor closes in client/server/acceptor destructors.
- qb-io coroutine: spawned-frame leak and channel use-after-free (parked waiter resumed after destruction).
- qb-io crypto: implemented missing key generation and corrected Argon2 password hashing.
- qb-io URI: out-of-range ports are rejected instead of silently truncated.
- qb-system: lock-free SPSC/MPSC memory-safety and correctness fixes; ring-buffer copy exception safety;
  ISO 8601 parsing treated as UTC.
- qb-core: actor lifecycle, event routing, and core-affinity hardening from a deep audit.

### Security

- QUIC: connection-limit denial-of-service mitigation, fail-closed RNG, and flow-control hardening.
- async/transport and crypto paths hardened to fail closed under malformed or hostile input.

[Unreleased]: https://github.com/isndev/qb/compare/main...HEAD
