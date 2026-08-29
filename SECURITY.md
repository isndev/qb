<!-- Verified-against: qb 3.0.1 (C++20 default, C++23 supported) -->

# Security policy

## Supported versions

**Security fixes are provided for one series: the latest minor of the current major.** They are
delivered as a new release of that series, not as a patch to an older one, and there is no backport
commitment to a previous major. This is the same policy [VERSIONING.md](./VERSIONING.md) states for
bug fixes, deliberately worded the same way — a security policy that promises more than the release
policy delivers is worse than one that promises less.

| Version | Supported |
|---------|-----------|
| 3.0.x   | Yes       |
| 2.6.x   | No — superseded by 3.0; fixes ship in the 3.0 series |
| < 2.0   | No        |

Note that 3.0 is a **major** release with source-breaking changes, so "upgrade to the supported
series" is not always a drop-in move; [CHANGELOG.md](./CHANGELOG.md) lists what changed, and the
[migration guide](./readme/6_guides/migration_guide.md) covers the paths that need edits. If that is
a genuine obstacle for a reported vulnerability, say so in the report — the decision is made per
report, and it is better made with that information than without it.

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
third-party components under `src/qb/vendor` — the libev fork `qev` (which carries wepoll on Windows),
nanolog, ska_hash, and stduuid.

Out of scope: defects in the upstream third-party projects themselves (libev, stduuid, nlohmann/json, …) —
report those to their maintainers; and issues that require a misconfiguration explicitly warned against in
the documentation. Findings in the optional qbm modules belong to their respective repositories.
nlohmann/json is a **dependency**, not a bundled component: since 3.0 it is resolved with
`find_package(nlohmann_json)`, so a defect in it is upstream's, while qb's *use* of it is in scope.
