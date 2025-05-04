# Reference: Testing the QB Framework

The QB framework includes a suite of unit and system tests built using the Google Test framework. These tests verify the correctness of individual components and their interactions.

## Test Philosophy

*   **Unit Tests (`qb/source/*/tests/unit/`):** Focus on isolating and testing specific classes or small modules (e.g., `Timestamp`, `event::router`, `crypto` functions) with minimal dependencies.
*   **System Tests (`qb/source/*/tests/system/`):** Test the integration and interaction of multiple components, often involving the `qb::Main` engine and multiple actors running across cores. These verify higher-level behaviors like message passing, lifecycle management, concurrency, and error handling.

## Building Tests

Tests are built automatically if the `QB_BUILD_TEST` CMake option is enabled (which is often the default).

```bash
# Configure with tests enabled (if not default)
cd build
cmake .. -DQB_BUILD_TEST=ON

# Build all targets, including tests
cmake --build .

# Or build a specific test target (names derived from source files)
cmake --build . --target qb-core-gtest-system-test-actor-event
cake --build . --target qb-io-gtest-test-crypto
```

Test executables are typically placed in the `build/bin/` directory under their respective module and test type (e.g., `build/bin/qb-core/tests/system/`).

## Running Tests

1.  **Using CTest (Recommended):** After building, navigate to the `build` directory and run CTest.
    ```bash
    cd build
    ctest # Run all tests
    ctest -R test-actor # Run tests matching regex "test-actor"
    ctest -V # Run tests with verbose output
    ```
2.  **Running Executables Directly:** Navigate to the test executable location and run it.
    ```bash
    cd build/bin/qb-core/tests/system
    ./qb-core-gtest-system-test-actor-add
    ```
    You can use Google Test command-line flags (e.g., `--gtest_filter=TestSuiteName.TestName`).

## Test Structure

*   **Location:** Tests reside within the `qb/source/<module>/tests/` directories (`unit` or `system`).
*   **Naming:** Test files generally follow the pattern `test-<feature_or_component>.cpp`.
*   **Framework:** Google Test (`gtest/gtest.h`). Tests use `TEST(TestSuiteName, TestName)` or `TEST_F(TestFixtureName, TestName)` macros.
*   **System Tests:** Often involve creating a `qb::Main` instance, adding specific test actors, running the engine synchronously (`main.start(false)`), waiting for completion (`main.join()`), and then asserting conditions (e.g., checking `main.hasError()` or global atomic counters modified by actors).

## Writing New Tests

1.  **Choose Location:** Decide if it's a unit test (isolating a class) or system test (integrating actors/modules) and place it in the appropriate `unit` or `system` directory under the relevant module (`qb-io` or `qb-core`).
2.  **Include Headers:** Include `<gtest/gtest.h>` and necessary QB headers.
3.  **Create Test Fixture (Optional):** Use `class MyTest : public ::testing::Test` for shared setup/teardown logic (`SetUp()`, `TearDown()`).
4.  **Write Test Case:** Use `TEST(...)` or `TEST_F(...)`.
5.  **System Test Setup:**
    *   Instantiate `qb::Main`.
    *   Add necessary test actors using `main.addActor<T>(...)` or `main.core(...).addActor<T>(...)`.
    *   Use `qb::io::async::callback` within actors if delays or specific sequences are needed.
    *   Use `std::atomic` variables or shared state (with mutexes *only if absolutely necessary outside actors*) for communication between test assertions and actor logic.
6.  **Run Engine:** `main.start(false); main.join();` (Synchronous execution is usually easiest for system tests).
7.  **Assert:** Use Google Test assertions (`EXPECT_EQ`, `ASSERT_TRUE`, etc.) to verify the outcome (check `main.hasError()`, atomic counters, actor states if accessible through specific events).
8.  **Add to CMake:** Add the new test source file to the appropriate list (`CORE_TESTS`, `ACTOR_SYSTEM_TESTS`, etc.) in the relevant `tests/system/CMakeLists.txt` or `tests/unit/CMakeLists.txt` file. 