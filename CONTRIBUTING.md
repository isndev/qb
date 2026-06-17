<!-- Verified-against: qb 2.0.0 (C++20 default, C++23 supported) -->
# Contributing to qb

Thank you for your interest in improving qb. This document describes how to report issues, propose changes,
and submit code.

By participating you agree to abide by the [Code of Conduct](./CODE_OF_CONDUCT.md). **Security
vulnerabilities must not be filed as public issues** — follow [SECURITY.md](./SECURITY.md) instead.

## Ways to contribute

- Report reproducible bugs.
- Propose enhancements or new features.
- Improve documentation.
- Fix bugs or implement features.
- Add tests.

## Reporting bugs

Before filing an issue:

1. Search the issue tracker for an existing report.
2. Reduce the problem to a minimal, reproducible example.
3. Describe expected versus actual behavior, the steps to reproduce, and your environment: operating
   system, compiler and version, qb version, and the CMake options you built with.

Use the bug-report issue template when one is available.

## Proposing enhancements

Open a discussion or issue that states the problem the change solves, how it would work (with an example
where possible), and why it benefits the framework. Check existing issues and discussions first.

## Pull request process

1. Fork the repository and create a topic branch (for example `feature/short-name` or `fix/issue-123`).
2. Make your change, following the [code style](#code-style).
3. Add or update tests. A bug fix should include a test that fails without the fix.
4. Build and run the full suite locally:
   ```bash
   cmake -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TESTS=ON -B build
   cmake --build build --parallel
   ctest --test-dir build --output-on-failure
   ```
5. If you touched documentation, run the documentation checker:
   ```bash
   ./scripts/doc-lint.sh
   ```
6. Write clear commit messages. Conventional-commit prefixes are encouraged (`feat:`, `fix:`, `docs:`,
   `build:`, `test:`).
7. Sign off your commits to certify the [Developer Certificate of Origin](https://developercertificate.org/):
   ```bash
   git commit -s
   ```
8. Open a pull request against the default branch with a clear description and a reference to any related
   issue (for example, "Closes #123").

Maintainers review every pull request and may request changes before merging.

## Code style

Source is formatted with the repository's `.clang-format` and checked against `.clang-tidy`. Format your
changes before committing:

```bash
clang-format -i <changed-files>
```

CI enforces formatting on changed C++ files and runs clang-tidy on changed C++ translation units. Keep
those checks small and clean in each pull request; broad formatting sweeps should be submitted separately
from behavioral changes.

Match the conventions of the surrounding code: the framework is CRTP-based and largely header-only, uses
the `qb::` namespace, and expresses time with the `qb::duration` / `qb::mono_time` / `qb::wall_time`
vocabulary (never the removed `qb::Timestamp` / `qb::Duration`).

## Testing

Tests use GoogleTest and run under `ctest`.

- **Unit tests** exercise a class or function in isolation — `source/<module>/tests/unit/`.
- **System tests** exercise interactions through the `qb::Main` engine and actors —
  `source/<module>/tests/system/`.

See the [testing guide](./readme/7_reference/testing.md) for details.

## Development environment

See [INSTALL.md](./INSTALL.md) and the [building guide](./readme/7_reference/building.md) for setting up a
build.

## Documentation changes

Documentation lives in `README.md`, the `readme/` tree, and the governance files. Follow the existing
voice and structure, keep every technical claim verifiable against the code, and run `./scripts/doc-lint.sh`
before submitting.
