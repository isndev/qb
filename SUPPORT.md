<!-- Verified-against: qb 3.0.1 (C++20 default, C++23 supported) -->

# Support

How to get help with qb, and what to expect.

## Where to ask

| Channel                | Use it for                                                                              |
|------------------------|-----------------------------------------------------------------------------------------|
| **GitHub Discussions** | Questions, design help, "how do I…", sharing what you built                             |
| **GitHub Issues**      | Reproducible bugs and concrete feature requests                                         |
| **Security advisory**  | Vulnerabilities — privately, per [SECURITY.md](./SECURITY.md). Never in a public issue. |

Before opening an issue, please:

1. Read the relevant [documentation](./readme/README.md) and the [FAQ](./readme/7_reference/faq.md).
2. Search existing issues and discussions.
3. For a bug, prepare a minimal, reproducible example and your environment (OS, compiler and version, qb
   version, build flags). See [CONTRIBUTING.md](./CONTRIBUTING.md) for the bug-report checklist.

## Scope of support

qb is open-source software maintained on a best-effort basis. Support covers building the framework on the
[supported toolchains](./INSTALL.md), using the documented APIs, and confirmed defects. It does not include
guaranteed response times, private consulting, or debugging of application code unrelated to a framework
defect.

## Self-service resources

- [Getting started](./readme/6_guides/getting_started.md) and the [guides](./readme/6_guides/)
- [API overview](./readme/7_reference/api_overview.md) and [invariants](./readme/7_reference/core_invariants.md)
- [Examples](https://github.com/isndev/qb-examples) covering actors, networking, and the qbm modules
- [Glossary](./readme/7_reference/glossary.md) for terminology

## Known issues

Open, triaged issues and their workarounds are tracked in the GitHub issue tracker. Filter by the
`known-issue` label for items with a documented workaround pending a fix.
