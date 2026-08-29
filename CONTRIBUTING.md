<!-- Verified-against: qb 3.0.1 (C++20 default, C++23 supported) -->

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

The bug-report issue template prompts for exactly this. One of its fields — *how is qb consumed?*
(`find_package` / `add_subdirectory` / FetchContent / a package manager / hand-written `-I` and `-l`)
— is the fastest discriminator for build and link failures, so answer it even in a free-form report.

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
8. Open a pull request against **`develop`**, with a clear description and a reference to any related
   issue (for example, "Closes #123"). `main` is the released line and moves only at a release; day-to-day
   work lands on `develop`. CI runs on pull requests to either branch, so targeting `main` will not fail
   loudly — it will simply propose your change for the released line, which is almost never what you want.

Maintainers review every pull request and may request changes before merging.

## Code style

Source is formatted with the repository's `.clang-format` and checked against `.clang-tidy`. Format your
changes before committing:

```bash
clang-format -i <changed-files>
```

CI enforces `clang-format` on changed C++ files (a `--dry-run --Werror` check); clang-tidy is not run in
CI, so run it locally via `scripts/clang-tidy.sh` against the `.clang-tidy` config before submitting. Keep
those checks small and clean in each pull request; broad formatting sweeps should be submitted separately
from behavioral changes.

Match the conventions of the surrounding code: the framework is CRTP- and template-heavy (`qb::core` and
`qb::io` build as compiled libraries), uses the `qb::` namespace, and expresses time with the
`qb::duration` / `qb::mono_time` / `qb::wall_time` vocabulary (never the removed `qb::Timestamp` /
`qb::Duration`).

## Testing

Tests use GoogleTest and run under `ctest`, in two tiers:

- **Unit tests** exercise a class or function in isolation — `tests/<module>/unit/`.
- **System tests** exercise interactions through the `qb::Main` engine and actors —
  `tests/<module>/system/`.

Every test carries `tier:<tier>` and `module:<module>` labels, so `ctest -L tier:unit` or
`ctest -L module:qb-core` selects a slice without needing a name regex.

`tests/<module>/benchmark/` is a third directory but not a third tier: those targets are registered
with `qb_add_benchmark`, are not `ctest` entries, and are not correctness gates.

Anything touching async code, coroutines, or the tests themselves should also be run under the
sanitizers before submitting — a data race or a use-after-free here is usually invisible to a plain
Release run. See the [testing guide](./readme/7_reference/testing.md) for details.

## Development environment

See [INSTALL.md](./INSTALL.md) and the [building guide](./readme/7_reference/building.md) for setting up a
build.

## Documentation changes

Documentation lives in `README.md`, the `readme/` tree, and the governance files. Follow the existing
voice and structure, keep every technical claim verifiable against the code, and run `./scripts/doc-lint.sh`
before submitting.
