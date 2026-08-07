<!--
Short on purpose. CONTRIBUTING.md has the full process; this lists only what a reviewer
here checks first, and what CI will fail on.
-->

## What this changes, and why

<!-- The "why" matters more than the "what" — the diff already says what. -->

Closes #

## Checks

- [ ] Builds clean, and `ctest` passes locally (`-DQB_BUILD_TESTS=ON`).
- [ ] Formatted with the repository `.clang-format` — CI fails on a `--dry-run --Werror` check of changed C++ files.
- [ ] A bug fix comes with a test that **fails without the fix**. Say so if you injected the defect and watched the test go red; a test nobody has seen fail is not yet evidence.
- [ ] Documentation touched? `./scripts/doc-lint.sh` passes. Every `file.h:NNN` citation must land on the line that contains the symbol it names.
- [ ] Commits are signed off (`git commit -s`) per the [DCO](https://developercertificate.org/).

## Anything a reviewer should know

<!--
Especially: a public API or header layout change (it may be an ABI break — see VERSIONING.md),
anything touching async/coroutine lifetime (please run the sanitize and sanitize-thread presets),
or a deliberate trade-off you want challenged.
-->
