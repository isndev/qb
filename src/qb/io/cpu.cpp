
#include <qb/system/cpu.h>
#include <qb/system/parse.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/sysctl.h>

#elif defined(unix) || defined(__unix) || defined(__unix__)
#include <fstream>
#include <regex>
#include <unistd.h>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#elif defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winreg.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- Linux sysfs topology (qb::detail::linux_physical_cores) -- portable C++, so the tests feed it fixture trees
// on every platform; only CPU::TotalCores() on Linux points it at /sys/devices/system/cpu (Huly QB-325).

// The first line of a small sysfs attribute, trimmed; nullopt when it cannot be opened or is empty.
std::optional<std::string>
read_attribute(std::filesystem::path const &path) {
    std::ifstream in(path);
    if (!in)
        return std::nullopt;
    std::string line;
    std::getline(in, line);
    std::size_t b = 0, e = line.size();
    while (b < e && std::isspace(static_cast<unsigned char>(line[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1])))
        --e;
    if (b == e)
        return std::nullopt;
    return line.substr(b, e - b);
}

// One decimal number of a cpulist at `at`, advancing past it; nullopt when there is none or it is absurd.
std::optional<int>
cpulist_number(std::string const &list, std::size_t &at) {
    const std::size_t start = at;
    long              value = 0;
    while (at < list.size() && std::isdigit(static_cast<unsigned char>(list[at]))) {
        value = value * 10 + (list[at] - '0');
        if (value > (1L << 20))
            return std::nullopt; // no host has a million CPUs: a corrupt list
        ++at;
    }
    if (at == start)
        return std::nullopt;
    return static_cast<int>(value);
}

// A kernel cpulist ("0-3,8,10-11") -> the CPU numbers; nullopt on any malformed token or an empty list.
std::optional<std::vector<int>>
parse_cpulist(std::string const &list) {
    std::vector<int> cpus;
    std::size_t      pos = 0;
    while (pos < list.size()) {
        const auto first = cpulist_number(list, pos);
        if (!first)
            return std::nullopt;
        int last = *first;
        if (pos < list.size() && list[pos] == '-') {
            ++pos;
            const auto upper = cpulist_number(list, pos);
            if (!upper || *upper < *first)
                return std::nullopt;
            last = *upper;
        }
        for (int cpu = *first; cpu <= last; ++cpu)
            cpus.push_back(cpu);
        if (pos < list.size()) {
            if (list[pos] != ',')
                return std::nullopt;
            ++pos;
        }
    }
    if (cpus.empty())
        return std::nullopt;
    return cpus;
}

// The sibling set of one CPU, the key of its physical core; nullopt when its topology cannot be read.
std::optional<std::string>
core_key(std::filesystem::path const &topology) {
    if (auto siblings = read_attribute(topology / "core_cpus_list"))
        return siblings;
    if (auto siblings = read_attribute(topology / "thread_siblings_list"))
        return siblings;
    const auto package = read_attribute(topology / "physical_package_id");
    const auto core    = read_attribute(topology / "core_id");
    if (!package || !core)
        return std::nullopt;
    const auto die = read_attribute(topology / "die_id");
    return "p" + *package + "/d" + die.value_or("0") + "/c" + *core;
}

#if defined(_WIN32) || defined(_WIN64)
DWORD
CountSetBits(ULONG_PTR bit_mask) {
    DWORD     left_shift    = static_cast<DWORD>(sizeof(ULONG_PTR) * 8 - 1);
    DWORD     bit_set_count = 0;
    ULONG_PTR bit_test      = (static_cast<ULONG_PTR>(1) << left_shift);

    for (DWORD i = 0; i <= left_shift; ++i) {
        bit_set_count += ((bit_mask & bit_test) ? 1u : 0u);
        bit_test /= 2;
    }

    return bit_set_count;
}
#endif

} // namespace

namespace qb {

std::string
CPU::Architecture() {
#if defined(__APPLE__)
    char   result[1024] = {};
    size_t size         = sizeof(result);

    if (sysctlbyname("machdep.cpu.brand_string", result, &size, nullptr, 0) == 0) {
        return std::string(result);
    }

    return "<unknown>";

#elif defined(unix) || defined(__unix) || defined(__unix__)
    static const std::regex pattern(R"(model name(.*): (.*))");

    std::ifstream stream("/proc/cpuinfo");
    std::string   line;

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_match(line, matches, pattern)) {
            return matches[2].str();
        }
    }

    return "<unknown>";

#elif defined(_WIN32) || defined(_WIN64)
    HKEY hkey_processor = nullptr;
    LONG error          = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hkey_processor);
    if (error != ERROR_SUCCESS) {
        return "<unknown>";
    }

    auto key = resource(hkey_processor, [](HKEY key_handle) { RegCloseKey(key_handle); });

    CHAR  buffer[_MAX_PATH] = {};
    DWORD buffer_size       = sizeof(buffer);

    error = RegQueryValueExA(key.get(), "ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &buffer_size);
    if (error != ERROR_SUCCESS) {
        return "<unknown>";
    }

    return std::string(buffer);

#else
#error Unsupported platform
#endif
}

int
CPU::Affinity() {
#if defined(__APPLE__)
    int    logical      = 0;
    size_t logical_size = sizeof(logical);

    if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0) {
        return -1;
    }

    return logical;

#elif defined(unix) || defined(__unix) || defined(__unix__)
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    return processors > 0 ? static_cast<int>(processors) : -1;

#elif defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return static_cast<int>(sysinfo.dwNumberOfProcessors);

#else
#error Unsupported platform
#endif
}

int
CPU::LogicalCores() {
    return TotalCores().first;
}

int
CPU::PhysicalCores() {
    return TotalCores().second;
}

std::pair<int, int>
CPU::TotalCores() {
#if defined(__APPLE__)
    int    logical      = 0;
    size_t logical_size = sizeof(logical);
    if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0) {
        logical = -1;
    }

    int    physical      = 0;
    size_t physical_size = sizeof(physical);
    if (sysctlbyname("hw.physicalcpu", &physical, &physical_size, nullptr, 0) != 0) {
        physical = -1;
    }

    return {logical, physical};

#elif defined(unix) || defined(__unix) || defined(__unix__)
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    const int  logical    = processors > 0 ? static_cast<int>(processors) : -1;
#if defined(__linux__)
    // One physical core per distinct sibling set of the online CPUs (Huly QB-325): the logical count returned as
    // the physical one made HyperThreading() false on every SMT host.
    return {logical, detail::linux_physical_cores("/sys/devices/system/cpu")};
#else
    return {logical, -1}; // no portable topology source here: unknown, never the logical count passed off as physical
#endif

#elif defined(_WIN32) || defined(_WIN64)
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = nullptr;
    DWORD                                 length = 0;

    for (;;) {
        const BOOL result = GetLogicalProcessorInformation(buffer, &length);
        if (result != FALSE) {
            break;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            std::free(buffer);
            return {-1, -1};
        }

        std::free(buffer);
        buffer = static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(std::malloc(length));
        if (buffer == nullptr) {
            return {-1, -1};
        }
    }

    std::pair<int, int> result{0, 0};
    auto               *current = buffer;
    DWORD               offset  = 0;

    while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= length) {
        switch (current->Relationship) {
            case RelationProcessorCore:
                result.first += static_cast<int>(CountSetBits(current->ProcessorMask));
                result.second += 1;
                break;

            case RelationNumaNode:
            case RelationCache:
            case RelationProcessorPackage:
                break;

            default:
                std::free(buffer);
                return {-1, -1};
        }

        offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ++current;
    }

    std::free(buffer);
    return result;

#else
#error Unsupported platform
#endif
}

std::int64_t
CPU::ClockSpeed() {
#if defined(__APPLE__)
    std::uint64_t frequency = 0;
    size_t        size      = sizeof(frequency);

    if (sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0) {
        return static_cast<std::int64_t>(frequency);
    }

    return -1;

#elif defined(unix) || defined(__unix) || defined(__unix__)
    static const std::regex pattern(R"(cpu MHz(.*): (.*))");

    std::ifstream stream("/proc/cpuinfo");
    std::string   line;

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_match(line, matches, pattern)) {
            const std::string mhz = matches[2].str();
            return static_cast<std::int64_t>(qb::to_number_prefix<double>(mhz).value_or(0.0) * 1000000.0);
        }
    }

    return -1;

#elif defined(_WIN32) || defined(_WIN64)
    HKEY hkey_processor = nullptr;
    LONG error          = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hkey_processor);
    if (error != ERROR_SUCCESS) {
        return -1;
    }

    auto key = resource(hkey_processor, [](HKEY key_handle) { RegCloseKey(key_handle); });

    DWORD mhz         = 0;
    DWORD buffer_size = sizeof(mhz);

    error = RegQueryValueExA(key.get(), "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &buffer_size);
    if (error != ERROR_SUCCESS) {
        return -1;
    }

    return static_cast<std::int64_t>(mhz) * 1000000;

#else
#error Unsupported platform
#endif
}

bool
CPU::HyperThreading() {
    const auto [logical, physical] = TotalCores();
    return logical > 0 && physical > 0 && logical != physical;
}

bool
CPU::ThreadPinningSupported() noexcept {
#if defined(__APPLE__)
    // Probe the kernel once per process. `thread_policy_get` is used rather than
    // `thread_policy_set` on purpose: it reaches the same THREAD_AFFINITY_POLICY
    // implementation (and so the same KERN_NOT_SUPPORTED on arm64) without leaving an
    // affinity tag on whichever thread happens to ask first. The magic static gives the
    // one-time, thread-safe init; the lambda cannot throw, so `noexcept` holds.
    static const bool supported = [] {
        thread_affinity_policy_data_t policy      = {THREAD_AFFINITY_TAG_NULL};
        mach_msg_type_number_t        count       = THREAD_AFFINITY_POLICY_COUNT;
        boolean_t                     get_default = FALSE;

        const kern_return_t ret = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
                                                    reinterpret_cast<thread_policy_t>(&policy), &count, &get_default);
        // Only KERN_NOT_SUPPORTED means "this kernel has no such thing". Any other
        // failure is a per-call problem, not a missing mechanism.
        return ret != KERN_NOT_SUPPORTED;
    }();

    return supported;

#elif defined(unix) || defined(__unix) || defined(__unix__)
    // pthread_setaffinity_np() is implemented; individual requests may still be refused.
    return true;

#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
    // SetThreadAffinityMask() is implemented; individual requests may still be refused.
    return true;
#else
    // Mirrors detail::pin_current_thread below, which only issues SetThreadAffinityMask() under
    // _MSC_VER (a GNU toolchain on Windows gets a #warning and no pinning at all).
    return false;
#endif

#else
#error Unsupported platform
#endif
}

namespace detail {

int
linux_physical_cores(std::string const &cpu_root) {
    namespace fs = std::filesystem;
    const fs::path   root(cpu_root);
    std::vector<int> cpus;
    if (const auto online = read_attribute(root / "online")) {
        auto parsed = parse_cpulist(*online);
        if (!parsed)
            return -1;
        cpus = std::move(*parsed);
    } else {
        // No `online` file: every cpu<N> directory counts (`cpufreq`, `cpuidle`, ... are not CPUs).
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name   = it->path().filename().string();
            bool              digits = name.size() > 3 && name.size() <= 9 && name.compare(0, 3, "cpu") == 0;
            for (std::size_t i = 3; digits && i < name.size(); ++i)
                digits = std::isdigit(static_cast<unsigned char>(name[i])) != 0;
            if (digits)
                cpus.push_back(std::stoi(name.substr(3)));
        }
        if (ec || cpus.empty())
            return -1;
    }
    std::set<std::string> cores;
    for (const int cpu : cpus) {
        const auto key = core_key(root / ("cpu" + std::to_string(cpu)) / "topology");
        if (!key)
            return -1; // an online CPU with no readable topology: unknown, never a partial count
        cores.insert(*key);
    }
    return static_cast<int>(cores.size());
}

// ---- Thread placement: the inherited mask, the usable CPUs, the pin and its undo (Huly QB-1081, QB-1082, QB-1083).
// The one place qb talks to the OS about where a thread runs; `Main` decides what to ask for.

std::vector<unsigned>
inherited_cpus() {
    std::vector<unsigned> cpus;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set))
                cpus.push_back(static_cast<unsigned>(cpu));
    }
#elif defined(_WIN32) || defined(_WIN64)
    DWORD_PTR process = 0;
    DWORD_PTR system  = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        for (unsigned cpu = 0; cpu < sizeof(DWORD_PTR) * CHAR_BIT; ++cpu)
            if (process & (static_cast<DWORD_PTR>(1) << cpu))
                cpus.push_back(cpu);
    }
#endif
    return cpus;
}

unsigned
cgroup_cpu_limit(std::string const &self_cgroup, std::string const &cgroup_root) {
    std::ifstream membership(self_cgroup);
    std::string   line;
    std::string   path;
    bool          unified = false;
    while (std::getline(membership, line)) {
        if (line.rfind("0::", 0) == 0) {
            path    = line.substr(3);
            unified = true;
            break;
        }
    }
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back())))
        path.pop_back();
    // A path outside the mount (`/..` when this process sits above its cgroup namespace's root) names no file here.
    if (!unified || path.empty() || path.front() != '/' || path.find("/..") != std::string::npos)
        return 0u;

    unsigned long long limit = 0;
    for (;;) {
        // The leaf first, then each parent up to the root, whose `cpu.max` does not exist: nothing caps the root.
        const auto file = std::filesystem::path(cgroup_root) / std::filesystem::path(path).relative_path() / "cpu.max";
        if (const auto value = read_attribute(file)) {
            const std::string &text  = *value;
            const auto         space = text.find(' ');
            if (space != std::string::npos && text.compare(0, space, "max") != 0) {
                unsigned long long quota  = 0;
                unsigned long long period = 0;
                const char *const  begin  = text.data();
                const char *const  end    = text.data() + text.size();
                const auto         q      = std::from_chars(begin, begin + space, quota);
                const auto         p      = std::from_chars(begin + space + 1, end, period);
                if (q.ec == std::errc{} && q.ptr == begin + space && p.ec == std::errc{} && quota > 0 && period > 0) {
                    const unsigned long long ceiling = quota / period + (quota % period != 0 ? 1u : 0u);
                    limit                            = limit == 0 ? ceiling : std::min(limit, ceiling);
                }
            }
        }
        if (path == "/")
            break;
        const auto slash = path.rfind('/');
        path             = slash == 0 ? std::string("/") : path.substr(0, slash);
    }
    return static_cast<unsigned>(std::min<unsigned long long>(limit, UINT_MAX));
}

unsigned
usable_cpus() {
    unsigned usable = std::thread::hardware_concurrency();
    if (usable == 0u)
        usable = 1u;
    if (const auto cpus = inherited_cpus(); !cpus.empty())
        usable = std::min(usable, static_cast<unsigned>(cpus.size()));
#if defined(__linux__)
    if (const auto limit = cgroup_cpu_limit("/proc/self/cgroup", "/sys/fs/cgroup"); limit != 0u)
        usable = std::min(usable, limit);
#endif
    return std::max(usable, 1u);
}

thread_placement
place_thread(unsigned const position, std::span<unsigned const> const requested, bool const explicit_request,
             std::span<unsigned const> const inherited) {
    thread_placement placement;
    if (!explicit_request) {
        if (inherited.empty())
            placement.pin.push_back(position); // no mask to read (macOS): core i asks for CPU i, as before 3.3
        else if (position < inherited.size())
            placement.pin.push_back(inherited[position]);
        return placement; // a core past the mask's end runs on the whole mask
    }
    for (const unsigned cpu : requested) {
        const bool inside = inherited.empty() || std::binary_search(inherited.begin(), inherited.end(), cpu);
        (inside ? placement.pin : placement.refused).push_back(cpu);
    }
    for (auto *const cpus : {&placement.pin, &placement.refused}) {
        std::sort(cpus->begin(), cpus->end());
        cpus->erase(std::unique(cpus->begin(), cpus->end()), cpus->end());
    }
    return placement;
}

int
pin_current_thread(std::span<unsigned const> const cpus) noexcept {
    if (cpus.empty())
        return 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    for (const unsigned cpu : cpus) {
        if (cpu < static_cast<unsigned>(CPU_SETSIZE)) {
            CPU_SET(static_cast<int>(cpu), &set);
            any = true;
        }
    }
    // pthread_setaffinity_np RETURNS its error; it does not set errno.
    return any ? pthread_setaffinity_np(pthread_self(), sizeof(set), &set) : 0;
#elif defined(__APPLE__)
    // macOS has no pthread_setaffinity_np. THREAD_AFFINITY_POLICY takes one tag, and threads sharing a tag are placed
    // to share an L2 cache: a hint, not a pin. The lowest CPU of the set is the tag. Apple Silicon does not implement
    // the policy and answers KERN_NOT_SUPPORTED to every call: taken as success, or every core of every run would warn
    // about a pin this host cannot make -- CPU::ThreadPinningSupported() is the query that tells.
    const unsigned first = *std::min_element(cpus.begin(), cpus.end());
    if (first >= std::thread::hardware_concurrency())
        return EINVAL;
    thread_affinity_policy_data_t policy = {static_cast<integer_t>(first)};
    const kern_return_t           ret    = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
                                                             reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
    return ret == KERN_SUCCESS || ret == KERN_NOT_SUPPORTED ? 0 : static_cast<int>(ret);
#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
    // Within the thread's processor group: a CPU past the mask's width is left out, never a failure.
    DWORD_PTR mask = 0;
    for (const unsigned cpu : cpus)
        if (cpu < sizeof(DWORD_PTR) * CHAR_BIT)
            mask |= static_cast<DWORD_PTR>(1) << cpu;
    if (mask == 0)
        return 0;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0 ? 0 : static_cast<int>(GetLastError());
#else
#warning "Cannot set affinity on windows with GNU Compiler"
    return 0;
#endif
#else
    return 0;
#endif
}

thread_affinity
current_thread_affinity() noexcept {
    thread_affinity affinity;
#if defined(__linux__)
    static_assert(CPU_SETSIZE <= 1024, "thread_affinity::cpus holds a cpu_set_t");
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set))
                affinity.cpus.set(static_cast<std::size_t>(cpu));
        affinity.known = true;
    }
#elif defined(__APPLE__)
    // The tag, or THREAD_AFFINITY_TAG_NULL for none; Apple Silicon answers KERN_NOT_SUPPORTED: nothing to keep.
    thread_affinity_policy_data_t policy      = {THREAD_AFFINITY_TAG_NULL};
    mach_msg_type_number_t        count       = THREAD_AFFINITY_POLICY_COUNT;
    boolean_t                     get_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), &count,
                          &get_default)
        == KERN_SUCCESS) {
        affinity.tag   = policy.affinity_tag;
        affinity.known = true;
    }
#elif (defined(_WIN32) || defined(_WIN64)) && defined(_MSC_VER)
    GROUP_AFFINITY group{};
    if (GetThreadGroupAffinity(GetCurrentThread(), &group)) {
        for (unsigned cpu = 0; cpu < sizeof(KAFFINITY) * CHAR_BIT; ++cpu)
            if (group.Mask & (static_cast<KAFFINITY>(1) << cpu))
                affinity.cpus.set(cpu);
        affinity.group = group.Group;
        affinity.known = true;
    }
#endif
    return affinity;
}

int
restore_thread_affinity(thread_affinity const &affinity) noexcept {
    if (!affinity.known)
        return 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (affinity.cpus.test(static_cast<std::size_t>(cpu)))
            CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy = {static_cast<integer_t>(affinity.tag)};
    const kern_return_t           ret    = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
                                                             reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
    return ret == KERN_SUCCESS || ret == KERN_NOT_SUPPORTED ? 0 : static_cast<int>(ret);
#elif (defined(_WIN32) || defined(_WIN64)) && defined(_MSC_VER)
    GROUP_AFFINITY group{};
    group.Group = affinity.group;
    for (unsigned cpu = 0; cpu < sizeof(KAFFINITY) * CHAR_BIT; ++cpu)
        if (affinity.cpus.test(cpu))
            group.Mask |= static_cast<KAFFINITY>(1) << cpu;
    return SetThreadGroupAffinity(GetCurrentThread(), &group, nullptr) ? 0 : static_cast<int>(GetLastError());
#else
    return 0;
#endif
}

} // namespace detail

} // namespace qb