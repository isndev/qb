<!-- Verified-against: qb 2.0.0 (C++20 default, C++23 supported) -->

# Security policy

## Supported versions

Security fixes are provided for the latest minor of the current major series.

| Version | Supported |
|---------|-----------|
| 2.0.x   | Yes       |
| < 2.0   | No        |

## Reporting a vulnerability

**Do not report security issues through public GitHub issues, pull requests, or discussions.**

Report vulnerabilities privately through GitHub's coordinated disclosure:

1. Go to the repository's **Security** tab.
2. Select **Report a vulnerability** to open a private advisory.
3. Include the affected version, a description of the impact, and a minimal reproduction if possible.

Please include enough detail to reproduce and assess the issue: affected component (qb-core, qb-io, a
transport, crypto, QUIC), configuration and build flags, platform and compiler, and a proof of concept
where applicable.

## What to expect

- **Acknowledgement** of your report within a few business days.
- An initial **assessment** of severity and affected versions.
- Coordination on a fix and a disclosure timeline. We aim to resolve confirmed high-severity issues
  promptly and will keep you informed of progress.
- **Credit** for the discovery in the release notes, unless you prefer to remain anonymous.

Please give us reasonable time to investigate and release a fix before any public disclosure.

## Scope

In scope: memory-safety defects, denial-of-service vectors, authentication or cryptographic weaknesses, and
input-handling flaws in qb's own code (qb-core, qb-io) and in how qb integrates and configures the bundled
third-party components under `qb/modules` (libev, nanolog, nlohmann/json, ska_hash, stduuid).

Out of scope: defects in the upstream third-party projects themselves (libev, nlohmann/json, stduuid, …) —
report those to their maintainers; and issues that require a misconfiguration explicitly warned against in
the documentation. Findings in the optional qbm modules belong to their respective repositories.
