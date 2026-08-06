# Production readiness checklist

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

A pre-deployment checklist for qb services: how to build a portable binary, configure TLS, cap resource use, wire logging and signal handling, run the test suite, and decide what to monitor.

**Prerequisites:** [Getting started](./getting_started.md), [Building from source](../7_reference/building.md) — **See also:** [CMake and dependencies](../7_reference/cmake_dependencies.md), [Performance tuning](./performance_tuning.md), [Resource management](./resource_management.md), [Error handling and resilience](./error_handling.md), [Testing](../7_reference/testing.md), [Core invariants](../7_reference/core_invariants.md), [qb-io invariants](../7_reference/io_invariants.md)

## Summary

A qb deployment is configured at three layers: the **build** (CMake options bake in CPU targeting, link-time optimization, and which features compile in), the **runtime** (TLS verification, per-connection message limits, idle latency, signal handling), and the **operations** (logging, tests, what to watch in production). This page walks each layer in checklist form. Every option, default, and invariant cited here is grounded in the framework's CMake files and headers; the inline `<!-- src -->` comments point at the source of truth so you can re-verify against your checkout.

Defaults are tuned for *development on the build host*. The most common production footguns — host-specific codegen (`QB_ENABLE_NATIVE_ARCH`) and TLS verification overrides — are called out explicitly below.

## 1. Build a portable binary

`QB_ENABLE_NATIVE_ARCH` is **`OFF`** by default, so a default build is already portable. Turning it **on** adds `-march=native` (or `-mcpu=native` where `-march=native` is unsupported, e.g. older Apple Clang on arm64; MSVC uses `/arch:AVX2`), and a binary built that way may execute illegal instructions on an older or different CPU. Check it explicitly before shipping: the `release-native` and `benchmarks` presets turn it on.

<!-- src: qb/cmake/qbConfig.cmake:88-94, qb/cmake/qbCompiler.cmake:233-260 -->

For any binary that ships to a machine other than the one that built it — a container image, a release artifact, a fleet with mixed CPU generations — turn native targeting **off**:

```bash
# Portable release: conservative CPU baseline, runs on any host of the same ISA family.
cmake -DCMAKE_BUILD_TYPE=Release -DQB_ENABLE_NATIVE_ARCH=OFF -B build
cmake --build build --parallel
```

With `QB_ENABLE_NATIVE_ARCH=OFF` and `QB_ENABLE_OPTIMIZATIONS=ON` (the default), GCC/Clang fall back to a portable baseline: `-march=x86-64` on x86-64, `-march=armv8-a` on non-Apple ARM64. On Apple Silicon the toolchain already targets the native CPU, so qb deliberately does *not* force a generic baseline there (forcing `armv8-a` would lose LSE atomics).

<!-- src: qb/cmake/qbCompiler.cmake:248-261 -->

A ready-made preset wraps the same configuration:

```bash
cmake --preset release-portable   # Release + QB_ENABLE_NATIVE_ARCH=OFF
```

All shipped presets (including `release` and `release-lto`) inherit the hidden `base` preset, which sets `QB_ENABLE_NATIVE_ARCH=OFF` — so `cmake --preset release` is portable. So is a raw `cmake -D...` build with no preset: the option itself is declared `OFF` (`qb/cmake/qbConfig.cmake:137`). Use `release-native` to opt into host-tuned codegen.

<!-- src: qb/CMakePresets.json (release-portable, release-native, release) -->

> If you must build native (single-tenant box, you control the hardware), pin the CPU model in your deployment manifest so a hardware swap does not silently break the binary.

**Checklist**

- [ ] Distributable artifact built with `QB_ENABLE_NATIVE_ARCH=OFF` (or the `release-portable` preset).
- [ ] `CMAKE_BUILD_TYPE=Release` (or `RelWithDebInfo` if you keep symbols for crash triage).
- [ ] Leave `QB_ENABLE_FAST_MATH=OFF` (default) unless you have audited every floating-point path — it breaks IEEE-754 compliance.

<!-- src: qb/cmake/qbConfig.cmake:92, qb/cmake/qbCompiler.cmake:210-212,225-231 (fast-math) -->

## 2. Link-time optimization

`QB_ENABLE_LTO` is **`OFF`** by default. Enabling it adds `-flto` (plus `-fuse-linker-plugin` on GCC) on GCC/Clang Release builds, and `/LTCG` on MSVC. LTO can improve runtime performance at the cost of longer link times; it is orthogonal to native targeting, so it composes with a portable build.

<!-- src: qb/cmake/qbConfig.cmake:87, qb/cmake/qbCompiler.cmake:266-289 (LTO block) -->

```bash
cmake --preset release-lto                       # Release + QB_ENABLE_LTO=ON
# or combine with a portable baseline:
cmake -DCMAKE_BUILD_TYPE=Release -DQB_ENABLE_LTO=ON -DQB_ENABLE_NATIVE_ARCH=OFF -B build
```

LTO flags are only applied to the `Release` configuration. If the compiler reports `-flto` as unsupported, qb emits a warning and continues without it rather than failing the build.

<!-- src: qb/cmake/qbCompiler.cmake:277-287 (Release-only -flto + unsupported-flag warning) -->

**Checklist**

- [ ] Decide on LTO per service. Measure: the win is workload-dependent, and the link-time cost is real for large module sets.
- [ ] If you enable LTO, run the full test suite against the LTO build, not only the default build — optimization changes can surface latent undefined behavior.

## 3. TLS configuration

TLS lives in `qb-io` and is gated by `QB_WITH_SSL` (default **`ON`**, backed by OpenSSL via `find_package`). If OpenSSL is not found at configure time, `QB_WITH_SSL` is forced **OFF** and the SSL transports compile out — so confirm the feature is actually enabled in your production build, not silently dropped.

<!-- src: qb/cmake/qbConfig.cmake:99, qb/cmake/qbDependencies.cmake:124-146 -->

Verify with the configuration banner the build prints, or check that `QB_WITH_SSL=1` is in the compile definitions.

<!-- src: qb/cmake/qbConfig.cmake:367-368 (QB_WITH_SSL=1 compile def), 366-385 (configuration banner) -->

### Client connections are secure by default

When `qb-io` builds the client `SSL_CTX` itself — the usual `connect()` / `n_connect()` / async-connector path — it is secure by default: it loads the system trust store, enables `SSL_VERIFY_PEER`, and checks the server certificate against the target hostname (or IP). The `_verify_peer` member starts `true`.

<!-- src: qb/src/qb/io/tcp/ssl/socket.h:342 (_verify_peer = true), 766-782 (set_insecure / secure-by-default doc) -->

The production hazard is the opt-out. `set_insecure()` clears verification (it is meant for self-signed certs in tests, externally-handled pinning, or trusted private channels) and removes protection against man-in-the-middle attacks. Audit your codebase before shipping:

```bash
# No call to set_insecure() should survive into a production build.
grep -rn "set_insecure" your_service/ qbm/
```

Note one asymmetry: when you adopt an externally-created `SSL` handle via `init(SSL*)`, qb-io does **not** touch verification policy — your context's settings are used as-is. If you build the context yourself, you own the verification posture.

<!-- src: qb/src/qb/io/tcp/ssl/socket.h:448-455 (init(SSL*)), 779-780 (verification-untouched note) -->

### Server contexts

Server-side TLS is built explicitly. `qb::io::ssl::create_server_context(method, cert_path, key_path)` constructs a context from a certificate and key; for mutual TLS, `configure_mtls_server_context(ctx, client_ca_file_path, verification_mode = SSL_VERIFY_PEER)` adds client-certificate verification (the default mode is `SSL_VERIFY_PEER`).

<!-- src: qb/src/qb/io/tcp/ssl/socket.h:83-94, 144-154 -->

These functions take `std::filesystem::path` arguments (certificate, key, CA file, CA directory, DH parameters, client certificate). Each filesystem path is resolved through `qb::io::sys::resolve_resource()`: an absolute path is used unchanged, while a relative path is looked up first against the current working directory and then against the running executable's own directory. A binary shipped with its certificates next to it therefore finds them from any working directory, while an absolute deploy path is honoured verbatim. (URL/URI and wire paths are unaffected — those remain `std::string`.)

<!-- src: qb/src/qb/io/system/file.h:368-388 (self_path / self_dir / resolve_resource) -->

After a handshake completes you can introspect the live connection — `get_negotiated_tls_version()`, `get_negotiated_cipher_suite()`, `get_alpn_selected_protocol()`, `get_peer_certificate_chain()` — to log or assert the negotiated parameters.

<!-- src: qb/src/qb/io/tcp/ssl/socket.h:642 (cipher suite), 648 (tls version), 654 (alpn), 689 (peer cert chain) -->

### Windows server bind: exclusive, not reusable

When a server binds its listening port (`socket::pserve`), the address-reuse option differs by platform. POSIX sets `SO_REUSEADDR` so a restarted listener can rebind a port whose previous connections still linger in `TIME_WAIT`. **Windows does not** — there `SO_REUSEADDR` has hijack semantics (a bind to an in-use port *succeeds* but is silently shadowed by the existing socket, so the new listener never accepts). qb instead sets `SO_EXCLUSIVEADDRUSE` on Windows: an in-use bind fails fast with `WSAEADDRINUSE`, and no other process can hijack the port. Windows already permits rebinding `TIME_WAIT` ports with no option set, so this loses nothing. The behaviour is fully internal and guarded by `#ifdef _WIN32`; no application change is required, but be aware that on Windows a second instance bound to the same port fails at bind rather than starting silently broken.

<!-- src: qb/src/qb/io/system/sys__socket.cpp:249-286 (socket::pserve; the _WIN32 SO_EXCLUSIVEADDRUSE / POSIX SO_REUSEADDR fork at 267-271) -->

**Checklist**

- [ ] Confirm `QB_WITH_SSL=1` is in the production build (OpenSSL was found at configure time).
- [ ] No stray `set_insecure()` calls in shipped code paths.
- [ ] Certificate and key paths resolve at deploy time (relative paths resolve against the cwd then the executable's directory via `resolve_resource`; absolute paths are used verbatim); certificate rotation is operationalized.
- [ ] If you accept client certificates, mTLS is configured with `SSL_VERIFY_PEER` (or stricter).
- [ ] On Windows, expect a second instance bound to an in-use port to fail at bind (`SO_EXCLUSIVEADDRUSE`), not start shadowed.

## 4. Resource limits

qb caps inbound work per connection to resist oversized-message denial of service.

### Per-message size limit

Every protocol-driven I/O component carries a `_max_message_size`, initialized to `QB_MAX_MESSAGE_SIZE` (**100 MB** by default — note that a stale doc comment on `max_message_size()` in `qb/io/async/io.h` says 10 MB; the macro definition in `config.h` is the source of truth). A frame larger than the limit marks the protocol invalid and disconnects with reason `-2` ("message too large").

<!-- src: qb/src/qb/io/config.h:163-173, qb/src/qb/io/async/io.h:495,802,1998 (_max_message_size members), 1283,2584 (disconnect reason -2) -->

100 MB is generous for most services. Lower it per component to match the largest legitimate message you accept:

```cpp
// Inside your session/protocol setup. 1 MB cap for a small-frame protocol.
this->set_max_message_size(1 * 1024 * 1024);
```

You can read the active limit back with `max_message_size()`. Setting it too low rejects legitimate traffic; too high re-opens the DoS surface — size it to the workload.

<!-- src: qb/src/qb/io/async/io.h:1048 (max_message_size), 1074 (set_max_message_size) -->

The framework also defines input/output buffer ceilings in the same header for the same reason; see [config.h](../../src/qb/io/config.h) for `QB_MAX_MESSAGE_SIZE` and the buffer-limit macros, all overridable at compile time with `-D`.

<!-- src: qb/src/qb/io/config.h:163-258 -->

### Connection and event-loop ceilings

`qb::io::event::Config` defines compile-time `constexpr` hints — a default poll timeout of 100 ms, a 64-event-per-poll batch, and a `MAX_CONNECTIONS` value of 10000 — but these are **not** wired into the event loop or the accept path; nothing in the framework reads them. The header itself describes `MAX_CONNECTIONS` as "a conceptual maximum ... a soft limit or a hint for sizing internal structures, rather than a hard enforced limit." The real per-handler connection cap is `QB_DEFAULT_MAX_SESSIONS` (default `0`, unlimited), settable at runtime with `set_max_sessions()`; beyond that, OS fd limits apply.

<!-- src: qb/src/qb/io/system/ev_config.h:44-82 (unenforced constants), qb/src/qb/io/config.h:220-233 (QB_DEFAULT_MAX_SESSIONS), qb/src/qb/io/async/io_handler.h:169 (set_max_sessions) -->

### Idle latency vs. CPU

Each `VirtualCore` runs a busy loop by default. `CoreInitializer::setLatency(qb::duration)` controls the trade-off:

- `qb::duration::zero()` (the default) — low-latency mode: the core spins, consuming a full CPU on its assigned core.
- `latency > 0` — the core may sleep up to that duration when idle, cutting CPU use at the cost of worst-case event-handling latency.

<!-- src: qb/src/qb/core/Main.h:242-254 -->

```cpp
// src: derived from qb/src/qb/core/Main.h (CoreInitializer API)
#include <chrono>
#include <qb/main.h>

int main() {
    qb::Main engine;

    // Core 0: trade a little latency for far lower idle CPU on a multi-tenant host.
    // setLatency takes a qb::duration (std::chrono::nanoseconds); any chrono
    // duration converts implicitly.
    engine.core(0).setLatency(std::chrono::microseconds{200});

    // ... addActor<...>(0, ...) ...
    engine.start();   // installs signal handlers, spawns one jthread per core
    engine.join();
    return 0;
}
```

For a busy server every active core at zero latency pins a CPU; on a shared or oversubscribed host, a small non-zero latency is usually the right default. See [Performance tuning](./performance_tuning.md) for sizing guidance.

**Checklist**

- [ ] `set_max_message_size()` tightened per protocol to the largest legitimate frame.
- [ ] Idle latency chosen deliberately: zero only on dedicated cores you can afford to burn.
- [ ] OS-level limits (file descriptors, memory cgroup) sized above the connection count you expect; cap concurrent sessions per handler with `set_max_sessions()` (`QB_DEFAULT_MAX_SESSIONS` is `0` / unlimited by default). The `MAX_CONNECTIONS` constant is an unenforced sizing hint, not a runtime ceiling.

## 5. Logging

Logging is gated by `QB_WITH_LOGGING` (default **`ON`**), which defines `QB_WITH_LOGGING=1` and compiles in the nanolog-backed `qb::io::log` API. When the option is off, the `qb::io::log` namespace (init/setLevel/Level) is not available. The `LOG_*` macros remain defined — as a `qb::io::cout()` fallback when `QB_STDOUT_LOGGING` is set, otherwise as no-ops.

<!-- src: qb/cmake/qbConfig.cmake:98 (QB_WITH_LOGGING option), qb/cmake/qbConfig.cmake:364-365 (QB_WITH_LOGGING=1 compile def), qb/src/qb/io.h:34-81 -->

Initialize logging once at startup, before any logging call. `init` takes the log-file path and a roll size in megabytes (default 128):

```cpp
// src: derived from qb/src/qb/io.h (qb::io::log)
#include <qb/io.h>

int main() {
#ifdef QB_WITH_LOGGING
    qb::io::log::init("/var/log/myservice/app.log", /* roll_MB = */ 128);
    qb::io::log::setLevel(qb::io::log::Level::WARN);   // production: WARN and above
#endif
    // ... engine setup ...
}
```

`setLevel` sets the minimum severity recorded; messages below it are dropped. The level enum, lowest to highest, is `DEBUG, VERBOSE, INFO, WARN, CRIT`.

<!-- src: qb/src/qb/io.h:48-81 -->

Two related options affect diagnostics rather than the file logger: `QB_STDOUT_LOGGING` (default **OFF**) enables a stdout fallback, and `QB_DEBUG_ACTOR` (default **OFF**) enables actor debugging output. Leave both off in production unless you are actively debugging.

<!-- src: qb/cmake/qbConfig.cmake:109-110 (QB_DEBUG_ACTOR / QB_STDOUT_LOGGING options), 330-335 (compile defs) -->

`qb::io::cout()` is a thread-safe console wrapper, but the header itself notes that production code should prefer the logging system over direct console output.

<!-- src: qb/src/qb/io.h:84-100 -->

**Checklist**

- [ ] `QB_WITH_LOGGING=ON` in the production build (it is the default; confirm it was not disabled).
- [ ] `qb::io::log::init(...)` called exactly once before the engine starts.
- [ ] Production log level set to `WARN` (or `INFO` if you need request-level visibility); `DEBUG`/`VERBOSE` are noisy and slow.
- [ ] `QB_STDOUT_LOGGING` and `QB_DEBUG_ACTOR` left OFF.
- [ ] Log file path is writable, on a volume with rotation/retention; the roll size matches your retention policy.

## 6. Signal handling

`qb::Main::start()` installs a `SIGINT` handler automatically (in both async and synchronous modes). The handler is async-signal-safe: it sets an internal `_signal_pending` flag that each `VirtualCore` polls every loop iteration, so the write is observed within the configured mailbox latency and the engine shuts down gracefully.

<!-- src: qb/src/qb/core/Main.cpp:198-201 (onSignal), 290,313 (SIGINT registered) -->

On POSIX the handler is installed with `sigaction` (no `SA_RESETHAND`, with `SA_RESTART`), so a *second* `SIGINT` still triggers the graceful path rather than the historical System-V behavior of resetting to default and terminating the process. On Windows, `std::signal` is used.

<!-- src: qb/src/qb/core/Main.cpp:382-401 (install_signal incl. comment + sigaction body) -->

To shut down on additional signals — typically `SIGTERM` under an init system or container orchestrator — register them after constructing the engine:

```cpp
// src: derived from qb/src/qb/core/Main.cpp (signal API)
#include <qb/main.h>
#include <csignal>

int main() {
    qb::Main engine;
    // ... configure cores and actors ...

    qb::Main::registerSignal(SIGTERM);   // graceful shutdown on SIGTERM too
    qb::Main::ignoreSignal(SIGPIPE);     // ignore broken-pipe (SIG_IGN)

    engine.start();
    engine.join();
    return 0;
}
```

The three signal entry points are static and `noexcept`: `registerSignal(signum)` routes the signal to the graceful-shutdown handler, `unregisterSignal(signum)` restores the OS default disposition (`SIG_DFL`), and `ignoreSignal(signum)` sets `SIG_IGN`.

<!-- src: qb/src/qb/core/Main.cpp:404-418 (registerSignal / unregisterSignal / ignoreSignal) -->

> Only `SIGINT` is registered automatically. If your platform delivers `SIGTERM` on shutdown (most container runtimes and service managers do), you must register it yourself or the process is killed without the graceful drain.

`qb::Main::stop()` is itself async-signal-safe and may be called from a signal handler; it sets the same pending flag and leaves the heavier `std::stop_source` broadcast to `~Main()` / `join()` where normal thread synchronization is safe.

<!-- src: qb/src/qb/core/Main.cpp:340-346 (stop) -->

**Checklist**

- [ ] `SIGTERM` registered if your orchestrator sends it on shutdown.
- [ ] `SIGPIPE` ignored if you do raw socket writes that could hit a closed peer.
- [ ] Shutdown drains in-flight work within your orchestrator's grace period (tune `setLatency` so the pending flag is observed promptly).

## 7. Run the tests

qb ships a GoogleTest suite registered with CTest (`qb_add_test`). Build with tests enabled, then run CTest from the build directory.

<!-- src: qb/readme/7_reference/testing.md -->

```bash
# Configure with tests on (default for the dev/debug presets), build, then test.
cmake --preset dev
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
```

Before shipping, also run the suite under sanitizers — this is where memory-safety and data-race regressions surface. The `sanitize` preset configures AddressSanitizer + UndefinedBehaviorSanitizer; `sanitize-thread` configures ThreadSanitizer. Both instrument every qb / qbm / test target and their link step (the `QB_SANITIZE` flags apply regardless of `CMAKE_BUILD_TYPE`, though these presets configure a Debug build).

<!-- src: qb/CMakePresets.json:90-106 (sanitize, sanitize-thread), qb/cmake/qbCompiler.cmake:310-339 (QB_SANITIZE applied regardless of build type) -->

```bash
cmake --preset sanitize          # ASan + UBSan
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure

cmake --preset sanitize-thread   # ThreadSanitizer (data races)
cmake --build build/sanitize-thread --parallel
ctest --test-dir build/sanitize-thread --output-on-failure
```

`QB_SANITIZE` adds `-fno-sanitize-recover=all`, so the first error aborts — CI-friendly. Note that sanitizers are incompatible with `QB_WITH_PROFILING`: enabling both warns at configure time because tcmalloc/gperftools intercept the same hooks.

<!-- src: qb/cmake/qbCompiler.cmake:317-326 (-fno-sanitize-recover=all + profiling-incompatibility warning) -->

See [Testing](../7_reference/testing.md) for the full reference, including running a single test by name.

**Checklist**

- [ ] Full suite green on the target platform and compiler.
- [ ] Suite green under the `sanitize` preset (ASan + UBSan).
- [ ] Suite green under `sanitize-thread` if the service is genuinely multi-core (multiple `VirtualCore` workers).
- [ ] If you enabled LTO or a portable baseline, the suite was run against *that* build configuration.

## 8. What to monitor

qb does not bundle a metrics exporter; instrument these signals from your application code and runtime.

| Signal | Where it comes from | Why it matters |
|---|---|---|
| Engine init failure | `qb::Main::hasError()` after `start()`; `LOG_CRIT` + a stderr line are emitted | A core failed to initialize — the process is up but not serving. Check on startup. |
| Per-core CPU | OS metrics per worker thread | At `setLatency(0)` each active core pins a CPU; a sudden drop or unexpected pin indicates a misconfiguration. |
| Disconnect reasons | Your protocol/session disconnect path; reason `-2` is "message too large" | A spike in `-2` disconnects means traffic is exceeding `max_message_size` — legitimate growth or an attack. |
| TLS handshake outcomes | `get_negotiated_tls_version()` / `get_negotiated_cipher_suite()` at handshake completion; OpenSSL error strings via `get_last_ssl_error_string()` | Failed handshakes and weak negotiated parameters surface cert/config drift early. |
| Connection count | Your accept path against `set_max_sessions()` (per handler; `QB_DEFAULT_MAX_SESSIONS` is `0`/unlimited) and OS fd limits | Approaching either limit means you are about to refuse connections. |
| Shutdown latency | Time from signal to `join()` return | A drain that exceeds the orchestrator grace period gets SIGKILLed; tune `setLatency`. |
| Log volume / level | The log file and roll behavior | `DEBUG`/`VERBOSE` left on in production inflates I/O and obscures real `WARN`/`ERROR` events. |

<!-- src: qb/src/qb/core/Main.cpp:328-332 (LOG_CRIT/stderr), 323-326 (hasError), qb/src/qb/io/async/io.h:1283,2584 (disconnect reason -2), qb/src/qb/io/tcp/ssl/socket.h:642,648,661 (introspection), qb/src/qb/io/async/io_handler.h:169 (set_max_sessions), qb/src/qb/io/system/ev_config.h:82 (MAX_CONNECTIONS hint) -->

**Checklist**

- [ ] Startup health gate: fail deploy if `hasError()` is true after `start()`.
- [ ] Per-core CPU and per-thread scheduling visible in your dashboards.
- [ ] Disconnect-reason and TLS-handshake metrics exported from application code.
- [ ] Alert on log `ERROR`/`CRIT` rate.

## Pitfalls

- **Shipping a native binary.** A binary built with `QB_ENABLE_NATIVE_ARCH=ON` on a newer CPU can crash with an illegal instruction elsewhere. The option is `OFF` by default and every preset but `release-native` / `benchmarks` keeps it off, so this bites only when it was turned on deliberately — verify before you distribute. (§1)
- **`set_insecure()` reaching production.** It disables TLS peer verification and removes MITM protection. Grep for it before release. (§3)
- **Assuming `QB_WITH_SSL` is on.** If OpenSSL is absent at configure time, SSL is silently forced off and the transports compile out. Confirm `QB_WITH_SSL=1` in the actual build. (§3)
- **Relying on the 10 MB message limit.** The real default is 100 MB (`QB_MAX_MESSAGE_SIZE` in `config.h`); a doc comment in `qb/io/async/io.h` is stale. Set the limit explicitly per protocol. (§4)
- **Only handling `SIGINT`.** `start()` registers `SIGINT` only. Container orchestrators send `SIGTERM` — register it yourself or lose the graceful drain. (§6)
- **Testing only the default build.** A portable, LTO, or sanitizer build can expose latent bugs the default build hides. Run the suite against the configuration you ship. (§7)

## See also

- [Building from source](../7_reference/building.md) — every CMake option, in full.
- [CMake and dependencies](../7_reference/cmake_dependencies.md) — how OpenSSL, zlib, and the bundled deps are resolved.
- [Performance tuning](./performance_tuning.md) — sizing cores, affinity, and latency.
- [Resource management](./resource_management.md) — actor lifetimes and RAII patterns.
- [Error handling and resilience](./error_handling.md) — failure modes and recovery.
- [Testing](../7_reference/testing.md) — the test and CTest reference.
- [qb-io invariants](../7_reference/io_invariants.md) and [Core invariants](../7_reference/core_invariants.md) — the rules the runtime relies on.
