@page ref_testing_md Reference: Testing the QB Actor Framework
@brief A guide to building, running, and writing tests for the QB Actor Framework using Google Test and CTest.

# Reference: Testing the QB Actor Framework

The QB Actor Framework includes a comprehensive suite of unit and system tests designed to ensure correctness, stability, and robustness. These tests are built using the Google Test framework and managed via CTest.

## Test Philosophy in QB

Our testing strategy is divided into two main categories:

*   **Unit Tests (typically found in `qb/source/<module>/tests/unit/`):**
    *   Focus on testing individual classes, functions, or small, isolated modules in detail.
    *   Aim for minimal dependencies to verify component logic in isolation.
    *   Examples: Testing `qb::Timestamp` functionality, `qb::io::uri` parsing, or specific cryptographic functions.

*   **System/Integration Tests (typically found in `qb/source/<module>/tests/system/`):**
    *   Focus on testing the interaction and integration of multiple framework components.
    *   Often involve creating `qb::Main` engine instances, launching multiple actors (potentially across different `VirtualCore`s), and verifying their collective behavior, message passing, lifecycle management, and concurrency aspects.
    *   Examples: Testing actor event delivery, inter-core communication, service actor resolution, or full client-server interactions using `qb-io` components within actors.

## Building the Tests

Tests are compiled as part of the standard QB Framework build process if the `QB_BUILD_TEST` CMake option is enabled (it is often `ON` by default).

1.  **Ensure `QB_BUILD_TEST=ON`:** When configuring CMake:
    ```bash
    # In your build directory
    cmake .. -DQB_BUILD_TEST=ON # Add other options as needed (e.g., -DCMAKE_BUILD_TYPE=Debug)
    ```
2.  **Build the Project:**
    ```bash
    # In your build directory
    cmake --build . --config Debug # Or Release
    # Alternatively: make -jN (Linux/macOS) or build the solution in Visual Studio (Windows)
    ```

Test executables are typically generated in your build directory, often under a path like `build/bin/qb/source/<module>/tests/<type>/` (e.g., `build/bin/qb/source/core/tests/system/qb-core-gtest-system-test-actor-event`).

## Running the Tests

Once built, you have two primary ways to run the tests:

1.  **Using CTest (Recommended for CI and Full Test Suite Execution):**
    CTest is CMake's testing tool and is the preferred way to run all or a subset of tests.
    ```bash
    # Navigate to your build directory first
    cd build

    # Run all discovered tests
    ctest

    # Run tests with verbose output (shows individual test case results)
    ctest -V

    # Run only tests whose names match a regular expression (e.g., all actor event tests)
    ctest -R test-actor-event

    # Run tests in parallel (if supported by your CTest version and test properties)
    # ctest -jN 
    ```

2.  **Running Individual Test Executables Directly:**
    You can also navigate to the directory containing a specific test executable and run it directly. This allows you to use Google Test-specific command-line flags.
    ```bash
    # Example for a specific core system test
    cd build/bin/qb/source/core/tests/system/
    ./qb-core-gtest-system-test-actor-add --gtest_color=yes

    # Example: Run only specific tests within that executable using a filter
    ./qb-core-gtest-system-test-actor-add --gtest_filter=ActorAddTestSuite.SpecificAddTest
    ```
    Refer to the Google Test documentation for a full list of its command-line options.

## Test Structure & Conventions

*   **Location:** Test source files (`test-*.cpp`) reside within the `qb/source/<module>/tests/unit/` or `qb/source/<module>/tests/system/` directories.
*   **Naming:** Test files are generally named `test-<feature_or_component>.cpp` (e.g., `test-actor-event.cpp`, `test-uri.cpp`).
*   **Framework:** Google Test (`gtest/gtest.h`) is used. Tests are defined using `TEST(TestSuiteName, TestName)` or, if using a test fixture, `TEST_F(TestFixtureClassName, TestName)`.
*   **System Test Approach:** Many system tests involve:
    *   Instantiating `qb::Main`.
    *   Adding specific test actor configurations to one or more cores.
    *   Running the engine synchronously for test determinism: `engine.start(false); engine.join();`.
    *   Using `std::atomic` variables, shared counters (protected by mutexes if accessed outside actor context during assertions), or specific response events to gather results or state from actors for assertion.
    *   Asserting expected outcomes using Google Test macros (`EXPECT_EQ`, `ASSERT_TRUE`, etc.), often also checking `engine.hasError()`.

## Writing New Tests

Contributions of new tests are highly encouraged!

1.  **Determine Scope:** Decide if it's a unit test (isolating a class/function) or a system/integration test (multiple components, actors).
2.  **Choose Location:** Place your new `test-myfeature.cpp` file in the appropriate `unit` or `system` subdirectory under the relevant module (e.g., `qb/source/core/tests/system/`).
3.  **Include Headers:** Always include `<gtest/gtest.h>`. Include necessary QB framework headers and any standard C++ headers your test requires.
4.  **Test Fixtures (Optional):** For tests requiring common setup/teardown logic, create a test fixture class inheriting from `public ::testing::Test`. Use `SetUp()` and `TearDown()` virtual methods.
5.  **Define Test Cases:** Use `TEST(MyFeatureTestSuite, DescriptiveTestName)` or `TEST_F(MyFixtureClass, DescriptiveTestName)`.
6.  **System Test Specifics:**
    *   Instantiate `qb::Main engine;`.
    *   Add your test actors using `engine.addActor<MyTestActor>(core_id, ...)` or `engine.core(core_id).builder()...`.
    *   If your test requires actors to perform a sequence of actions or wait for certain conditions, use `qb::io::async::callback` within your test actors to schedule subsequent steps or self-terminating events.
    *   For verifying state across actors or after the engine run, `std::atomic` variables (declared globally or as static members of a test fixture) are often useful for safe communication of results from actors back to the main test thread for assertions. Use mutexes only if absolutely necessary for more complex shared state between the test and actors.
    *   Run the engine synchronously for deterministic system tests: `engine.start(false); engine.join();`.
7.  **Assert Outcomes:** Use Google Test assertions (`EXPECT_TRUE`, `ASSERT_EQ`, `EXPECT_FALSE`, `ASSERT_NE`, `EXPECT_THROW`, etc.) to verify the behavior and state of your components or the overall system.
8.  **Update CMake:** Add your new `.cpp` file to the list of sources in the relevant `CMakeLists.txt` file (e.g., in `qb/source/core/tests/system/CMakeLists.txt`). The target names are usually derived from the filename. Follow the existing patterns in those files.

By following these guidelines, you can contribute effective tests that help maintain the quality and reliability of the QB Actor Framework.

**(Next:** Consult the [QB Actor Framework: Frequently Asked Questions (FAQ)](./faq.md) or the [QB Actor Framework: Glossary of Terms](./glossary.md) for more framework information.**) 