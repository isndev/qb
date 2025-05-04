# Contributing to QB Actor Framework

First off, thank you for considering contributing to the QB Actor Framework! We welcome contributions from the community to help make QB even better.

This document provides guidelines for contributing to the project, ensuring a smooth and effective process for everyone involved.

## How Can I Contribute?

There are many ways to contribute, including:

*   🐞 Reporting bugs
*   ✨ Suggesting enhancements or new features
*   📝 Improving documentation
*   💻 Writing code (fixing bugs, adding features)
*   🧪 Adding tests

## Reporting Bugs

If you find a bug, please ensure the following before submitting an issue:

1.  **Check Existing Issues:** Search the issue tracker to see if the bug has already been reported.
2.  **Reproducible Example:** Provide a minimal, reproducible example of the bug if possible. This helps us diagnose and fix the issue much faster.
3.  **Describe the Issue:** Clearly explain the bug, including:
    *   What you expected to happen.
    *   What actually happened.
    *   Steps to reproduce the behavior.
    *   Your environment (Operating System, Compiler version, QB version).

Use the "Bug Report" issue template if available.

## Suggesting Enhancements

We love hearing ideas for new features or improvements!

1.  **Check Existing Issues/Discussions:** Search the issue tracker and discussions to see if your idea has already been suggested.
2.  **Explain the Idea:** Clearly describe the enhancement or feature you'd like to see.
    *   What problem does it solve?
    *   How would it work? (Provide examples if possible).
    *   Why would it be valuable to the framework?

Use the "Feature Request" issue template if available.

## Pull Request Process

We actively welcome your pull requests!

1.  **Fork the Repository:** Create your own fork of the QB repository on GitHub.
2.  **Create a Branch:** Create a new branch in your fork for your changes (e.g., `git checkout -b feature/my-new-feature` or `fix/issue-123`).
3.  **Develop:** Make your changes in your branch. Adhere to the project's code style (see below).
4.  **Add Tests:** Ensure your changes include relevant unit or system tests. Bug fixes should ideally include a test that demonstrates the fix.
5.  **Ensure Tests Pass:** Run the test suite (`ctest` in the build directory) to make sure all tests pass.
6.  **Commit Changes:** Use clear and concise commit messages. Follow conventional commit message formats if possible (e.g., `feat: Add support for XYZ`, `fix: Correct calculation in Actor::method`).
7.  **Push to Your Fork:** Push your changes to your fork on GitHub (`git push origin feature/my-new-feature`).
8.  **Submit a Pull Request:** Open a pull request from your branch to the main QB repository (usually the `develop` or `main` branch, check project conventions).
    *   Provide a clear title and description of your changes.
    *   Reference any related issues (e.g., "Closes #123").
9.  **Code Review:** Project maintainers will review your code, provide feedback, and potentially request changes.
10. **Merge:** Once approved, your pull request will be merged.

## Code Style

Please adhere to the coding style defined in the `.clang-format` file located in the root of the repository. Most modern C++ IDEs can automatically format code based on this file.

You can typically run `clang-format -i <your_changed_files>` to format your code before committing.

## Testing

Contributions, especially code changes, should be accompanied by tests.

*   **Unit Tests:** Test individual classes and functions in isolation. Place these in `qb/source/<module>/tests/unit/`.
*   **System Tests:** Test the interaction between multiple components, often involving the `qb::Main` engine and actors. Place these in `qb/source/<module>/tests/system/`.

Refer to the [Testing Guide](./readme/7_reference/testing.md) for more details on building and running tests.

## Development Environment Setup

Refer to the [Building Guide](./readme/7_reference/building.md) for instructions on setting up your development environment and building the framework.

## Code of Conduct

Please note that this project is released with a Contributor Code of Conduct. By participating in this project, including in the issue tracker, pull requests, and discussions, you agree to abide by its terms. We are committed to providing a welcoming and harassment-free experience for everyone.

Please review our [Code of Conduct](./CODE_OF_CONDUCT.md).

---

Thank you again for your interest in contributing to QB! 