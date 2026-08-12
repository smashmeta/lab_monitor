# Lab Monitor Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the complete `lab_monitor` CMake/vcpkg solution — four shared libraries, a tray client and a server console — with resource monitoring working end to end over DDS.

**Architecture:** Four focused libraries (`lm_core` domain logic with zero I/O, `lm_platform` OS probes behind interfaces, `lm_transport` DDS behind an `ITransport` abstraction, `lm_ui` shared Qt widgets) composed by two Qt Widgets applications. All compliance logic lives in two pure functions in `lm_core` that are unit tested without mocks, DDS or syscalls.

**Tech Stack:** C++23, CMake ≥ 3.28, vcpkg (manifest mode), Qt 5.15.18 Widgets, eProsima Fast DDS 3.4.1, FastCDR 2.3.4, nlohmann-json, spdlog, Boost.ProgramOptions, GoogleTest.

**Spec:** [`docs/superpowers/specs/2026-08-11-lab-monitor-design.md`](../specs/2026-08-11-lab-monitor-design.md)

## Global Constraints

Every task's requirements implicitly include this section.

- **C++23.** `CMAKE_CXX_STANDARD 23`, `CXX_STANDARD_REQUIRED ON`, `CXX_EXTENSIONS OFF`.
- **CMake ≥ 3.28.** The CMake on PATH is 3.24.2 and is **too old** — it has no `Visual Studio 18 2026` generator and cannot detect MSVC 14.51's C++23 support. On Windows use the CMake bundled with Visual Studio 2026:
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` (version 4.3.1).
- **vcpkg baseline is pinned to `4f326c4072038c8624c36a8ba5ed23f616adda53`.** Do not change it. This is the commit that produced the local binary cache; changing it forces multi-hour rebuilds of Qt.
- **Warnings are errors.** `CMAKE_COMPILE_WARNING_AS_ERROR ON`, `/W4` on MSVC, `-Wall -Wextra -Wpedantic -Wshadow` elsewhere. Every target links `lm_warnings`.
- **Naming.** Types `PascalCase`, functions and variables `snake_case`, private members `trailing_`. Include prefix `lm/<lib>/…`. Namespaces `lm::core`, `lm::platform`, `lm::transport`, `lm::ui`.
- **`lm_core` depends on `nlohmann-json` and nothing else.** No Qt, no DDS, no syscalls, no Boost. Adding any other dependency to `lm_core` is a design violation.
- **Exact CMake target names** (verified against the vcpkg ports at this baseline):
  | Package | `find_package` | Link target |
  |---|---|---|
  | nlohmann-json | `find_package(nlohmann_json CONFIG REQUIRED)` | `nlohmann_json::nlohmann_json` |
  | spdlog | `find_package(spdlog CONFIG REQUIRED)` | `spdlog::spdlog` |
  | GoogleTest | `find_package(GTest CONFIG REQUIRED)` | `GTest::gtest` `GTest::gmock` `GTest::gtest_main` |
  | Fast DDS | `find_package(fastdds CONFIG REQUIRED)` | `fastdds` |
  | FastCDR | `find_package(fastcdr CONFIG REQUIRED)` | `fastcdr` |
  | Boost | `find_package(Boost REQUIRED COMPONENTS program_options)` | `Boost::program_options` |
  | Qt | `find_package(Qt5 COMPONENTS Widgets Svg REQUIRED)` | `Qt5::Widgets` `Qt5::Svg` |
- **Commit after every task.** Conventional commit prefixes (`feat:`, `test:`, `build:`, `docs:`).

## Fidelity of this plan, by task

Be aware of where this plan hands you finished code and where it hands you a
specification you must still write code against.

| Tasks | Fidelity |
|---|---|
| **1–10, 14 (Step 1–3)** | **Complete.** Every test and every implementation is given in full. Type it in and it compiles. |
| **11** (Fast DDS binding) | **Specified, not transcribed.** The QoS table, the class structure and the codec mapping are exact, but the `TopicDataType` virtuals are *deliberately* not reproduced — Fast DDS 3.x renamed them relative to 2.x, and a wrong signature written from memory costs more than reading the installed header. Step 1 tells you which header to open. |
| **12 (Step 7), 13 (Steps 4–6), 14 (Steps 4–8)** | **Specified, not transcribed.** Widget and window construction is described precisely — every class, signal, QoS, threading rule and interaction — but without full `.cpp` bodies. These are conventional Qt Widgets layouts; the decisions that are easy to get wrong (stable model ordering, coalescing, queued connections, `closeEvent` semantics) are all stated explicitly. |

The tested logic — the two pure `lm_core` functions, the codecs, the registry, the
model and the coalescer — is given in full, because that is where correctness is hard
and where regressions are invisible. The Qt layout code is where it is cheapest to
iterate visually, which is exactly what the user asked to do after the first build.

## File Structure

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Options, standard, subdirectory wiring |
| `CMakePresets.json` | Windows (VS 2026 generator) and Linux (Ninja) presets |
| `vcpkg.json` | Pinned manifest, `gui` feature |
| `cmake/LabMonitorWarnings.cmake` | `lm_warnings` INTERFACE target |
| `cmake/LabMonitorRuntime.cmake` | `copy_runtime_dependencies()` (ported from discnet) |
| `cmake/LabMonitorTesting.cmake` | `lm_add_test()` helper |
| `libs/core/include/lm/core/types.hpp` | Ids, enums, `Capabilities` |
| `libs/core/include/lm/core/version.hpp` | `Version`, `VersionConstraint`, parsing and comparison |
| `libs/core/include/lm/core/rule.hpp` | `Rule` and its payload variants |
| `libs/core/include/lm/core/host_facts.hpp` | `HostFacts` snapshot structs |
| `libs/core/include/lm/core/template_bundle.hpp` | `Template`, `TemplateBundle`, `rules_for()` |
| `libs/core/include/lm/core/compliance.hpp` | `CheckResult`, `ComplianceReport`, `evaluate()` |
| `libs/core/include/lm/core/fleet.hpp` | `HostState`, `FleetView`, `reconcile()` |
| `libs/core/include/lm/core/json.hpp` | nlohmann adapters for every core type |
| `libs/platform/include/lm/platform/probes.hpp` | Probe interfaces + `HostProbes` |
| `libs/platform/include/lm/platform/fakes.hpp` | Test doubles for every probe |
| `libs/platform/src/{windows,linux}/` | Per-OS probe implementations |
| `libs/transport/include/lm/transport/transport.hpp` | `ITransport`, topic callbacks |
| `libs/transport/src/in_memory_transport.cpp` | Test/offline transport |
| `libs/transport/src/fastdds/` | `TopicDataType` codecs + `FastDdsTransport` |
| `libs/ui/` | Theme, status pill, sparkline, meter bar, table models |
| `apps/client/` | Tray controller, detail window |
| `apps/server/` | Fleet window, status ribbon |

---

## Task 1: Build system skeleton

**Files:**
- Create: `.gitignore`, `.clang-format`, `.editorconfig`, `README.md`
- Create: `vcpkg.json`, `CMakePresets.json`, `CMakeLists.txt`
- Create: `cmake/LabMonitorWarnings.cmake`, `cmake/LabMonitorRuntime.cmake`, `cmake/LabMonitorTesting.cmake`
- Create: `libs/core/CMakeLists.txt`, `libs/core/include/lm/core/types.hpp`, `libs/core/src/types.cpp`
- Test: `libs/core/tests/test_smoke.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `lm_warnings` INTERFACE target; `copy_runtime_dependencies(target)`; `lm_add_test(<name> SOURCES ... LINK ...)`; `lm_core` static library target; `lm::core::Capabilities` with `add()`, `has()`, `raw()`.

- [ ] **Step 1: Write `.gitignore`**

```gitignore
build/
out/
install/
.vs/
.vscode/
vcpkg_installed/
CMakeUserPresets.json
compile_commands.json
*.user
```

- [ ] **Step 2: Write `vcpkg.json`**

```json
{
  "name": "lab-monitor",
  "version": "0.1.0",
  "builtin-baseline": "4f326c4072038c8624c36a8ba5ed23f616adda53",
  "dependencies": [
    "boost-program-options",
    "nlohmann-json",
    "fastdds",
    "fastcdr",
    "spdlog",
    "gtest"
  ],
  "features": {
    "gui": {
      "description": "Qt Widgets user interfaces",
      "dependencies": [ "qt5-base", "qt5-svg" ]
    }
  },
  "default-features": [ "gui" ]
}
```

- [ ] **Step 3: Write `cmake/LabMonitorWarnings.cmake`**

```cmake
add_library(lm_warnings INTERFACE)
add_library(lm::warnings ALIAS lm_warnings)

if(MSVC)
  target_compile_options(lm_warnings INTERFACE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  target_compile_definitions(lm_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lm_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()
```

- [ ] **Step 4: Write `cmake/LabMonitorRuntime.cmake`**

Ported from `discnet`. Stages Qt and Fast DDS DLLs next to executables on Windows.

```cmake
function(copy_runtime_dependencies target)
  add_custom_command(
    TARGET "${target}" POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying runtime dependencies for ${target}...")
endfunction()
```

- [ ] **Step 5: Write `cmake/LabMonitorTesting.cmake`**

```cmake
find_package(GTest CONFIG REQUIRED)

function(lm_add_test target)
  cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE
    ${ARG_LINK} GTest::gtest GTest::gmock GTest::gtest_main lm_warnings)
  add_test(NAME ${target} COMMAND ${target})
  copy_runtime_dependencies(${target})
endfunction()
```

- [ ] **Step 6: Write the top-level `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(lab_monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

option(LM_BUILD_GUI "Build the Qt GUI applications" ON)
option(LM_BUILD_INTEGRATION_TESTS "Build tests that open a real DDS domain" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(LabMonitorWarnings)
include(LabMonitorRuntime)
include(LabMonitorTesting)

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

enable_testing()

add_subdirectory(libs/core)
```

Later tasks append `add_subdirectory` lines for `libs/platform`, `libs/transport`, and — inside `if(LM_BUILD_GUI)` — `libs/ui`, `apps/client`, `apps/server`.

- [ ] **Step 7: Write `CMakePresets.json`**

Windows uses the Visual Studio 2026 generator (multi-config, so build and test presets pass `--config`). Linux uses Ninja.

```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 28, "patch": 0 },
  "configurePresets": [
    {
      "name": "windows",
      "generator": "Visual Studio 18 2026",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/windows",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-windows" },
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "windows-headless",
      "inherits": "windows",
      "binaryDir": "${sourceDir}/build/windows-headless",
      "cacheVariables": {
        "LM_BUILD_GUI": "OFF",
        "VCPKG_MANIFEST_NO_DEFAULT_FEATURES": "ON"
      }
    },
    {
      "name": "linux-debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-debug",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      },
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" }
    },
    {
      "name": "linux-headless",
      "inherits": "linux-debug",
      "binaryDir": "${sourceDir}/build/linux-headless",
      "cacheVariables": {
        "LM_BUILD_GUI": "OFF",
        "VCPKG_MANIFEST_NO_DEFAULT_FEATURES": "ON"
      }
    }
  ],
  "buildPresets": [
    { "name": "windows-debug", "configurePreset": "windows", "configuration": "Debug" },
    { "name": "windows-release", "configurePreset": "windows", "configuration": "Release" },
    { "name": "windows-headless", "configurePreset": "windows-headless", "configuration": "Debug" },
    { "name": "linux-debug", "configurePreset": "linux-debug" },
    { "name": "linux-headless", "configurePreset": "linux-headless" }
  ],
  "testPresets": [
    {
      "name": "windows-debug",
      "configurePreset": "windows",
      "configuration": "Debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "linux-debug",
      "configurePreset": "linux-debug",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

- [ ] **Step 8: Write `libs/core/include/lm/core/types.hpp`**

`Capabilities` is a bitset wrapper. It exists now because Task 1 needs something real to test, and every later task depends on it.

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lm::core {

using HostId = std::string;
using RuleId = std::string;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class RuleKind { Process, Service, Registry };
enum class Presence { MustBePresent, MustBeAbsent };
enum class CheckStatus { Pass, Fail, NotApplicable, Error };
enum class ServiceState { Running, Stopped, Unknown };

enum class Capability : std::uint32_t {
    Resources = 1u << 0,
    Processes = 1u << 1,
    Services  = 1u << 2,
    Registry  = 1u << 3,
};

/// A set of capabilities a client advertises. Rules whose required capability
/// is absent evaluate to CheckStatus::NotApplicable rather than Pass or Fail.
class Capabilities {
public:
    Capabilities() = default;
    explicit Capabilities(std::uint32_t raw) : raw_(raw) {}

    Capabilities& add(Capability c) {
        raw_ |= static_cast<std::uint32_t>(c);
        return *this;
    }

    [[nodiscard]] bool has(Capability c) const {
        return (raw_ & static_cast<std::uint32_t>(c)) != 0u;
    }

    [[nodiscard]] std::uint32_t raw() const { return raw_; }

    friend bool operator==(const Capabilities&, const Capabilities&) = default;

private:
    std::uint32_t raw_ = 0;
};

/// The capabilities of the platform this binary was compiled for.
[[nodiscard]] Capabilities platform_capabilities();

/// Maps a rule kind onto the capability required to evaluate it.
[[nodiscard]] Capability required_capability(RuleKind kind);

}  // namespace lm::core
```

- [ ] **Step 9: Write `libs/core/src/types.cpp`**

```cpp
#include "lm/core/types.hpp"

namespace lm::core {

Capabilities platform_capabilities() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
#ifdef _WIN32
    caps.add(Capability::Registry);
#endif
    return caps;
}

Capability required_capability(RuleKind kind) {
    switch (kind) {
        case RuleKind::Process:  return Capability::Processes;
        case RuleKind::Service:  return Capability::Services;
        case RuleKind::Registry: return Capability::Registry;
    }
    return Capability::Resources;
}

}  // namespace lm::core
```

- [ ] **Step 10: Write the failing test `libs/core/tests/test_smoke.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/core/types.hpp"

using namespace lm::core;

TEST(Capabilities, StartsEmpty) {
    const Capabilities caps;
    EXPECT_FALSE(caps.has(Capability::Resources));
    EXPECT_FALSE(caps.has(Capability::Registry));
}

TEST(Capabilities, AddIsIndependentPerFlag) {
    Capabilities caps;
    caps.add(Capability::Processes);
    EXPECT_TRUE(caps.has(Capability::Processes));
    EXPECT_FALSE(caps.has(Capability::Services));
}

TEST(Capabilities, RegistryOnlyOnWindows) {
    const Capabilities caps = platform_capabilities();
    EXPECT_TRUE(caps.has(Capability::Resources));
#ifdef _WIN32
    EXPECT_TRUE(caps.has(Capability::Registry));
#else
    EXPECT_FALSE(caps.has(Capability::Registry));
#endif
}

TEST(RequiredCapability, MapsEachRuleKind) {
    EXPECT_EQ(required_capability(RuleKind::Process), Capability::Processes);
    EXPECT_EQ(required_capability(RuleKind::Service), Capability::Services);
    EXPECT_EQ(required_capability(RuleKind::Registry), Capability::Registry);
}
```

- [ ] **Step 11: Write `libs/core/CMakeLists.txt`**

```cmake
find_package(nlohmann_json CONFIG REQUIRED)

add_library(lm_core STATIC
  src/types.cpp)
add_library(lm::core ALIAS lm_core)

target_include_directories(lm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_core PUBLIC nlohmann_json::nlohmann_json PRIVATE lm_warnings)

lm_add_test(lm_core_tests SOURCES tests/test_smoke.cpp LINK lm_core)
```

- [ ] **Step 12: Configure and verify the test fails to build before implementation is complete**

If Steps 8–9 were skipped, configuration succeeds but `lm_core_tests` fails to compile with "no member named 'has'". Run:

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows
```

Expected: configure completes, vcpkg restores boost/spdlog/gtest/qt5-base from the binary cache and builds only `fastdds` and `fastcdr`.

- [ ] **Step 13: Build and run the tests**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: 4 tests pass.

- [ ] **Step 14: Verify the headless preset works**

```bash
cmake --preset windows-headless
cmake --build --preset windows-headless
```

Expected: configures and builds with no Qt in the dependency graph.

- [ ] **Step 15: Write `README.md`**

Must document: the CMake ≥ 3.28 requirement and the exact VS-bundled CMake path, `VCPKG_ROOT`, the four presets, and how to run tests.

- [ ] **Step 16: Commit**

```bash
git add .gitignore .clang-format .editorconfig README.md vcpkg.json CMakePresets.json CMakeLists.txt cmake libs
git commit -m "build: add CMake/vcpkg build system and lm_core skeleton"
```

---

## Task 2: Version parsing and comparison

**Files:**
- Create: `libs/core/include/lm/core/version.hpp`, `libs/core/src/version.cpp`
- Modify: `libs/core/CMakeLists.txt` (add `src/version.cpp`, add test source)
- Test: `libs/core/tests/test_version.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `Version` (`std::vector<int> parts`), `parse_version(std::string_view) -> std::optional<Version>`, `compare(const Version&, const Version&) -> int`, `ComparisonOp`, `VersionConstraint{op, value}`, `satisfies(const Version&, const VersionConstraint&) -> bool`, `to_string(const Version&) -> std::string`.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_version.cpp`**

The important behaviour: trailing zeros are insignificant, so `1.2` and `1.2.0` compare equal. This is what makes real-world version constraints behave sanely.

```cpp
#include <gtest/gtest.h>

#include "lm/core/version.hpp"

using namespace lm::core;

TEST(ParseVersion, AcceptsDottedNumbers) {
    const auto v = parse_version("10.0.19045.1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->parts, (std::vector<int>{10, 0, 19045, 1}));
}

TEST(ParseVersion, AcceptsSingleComponent) {
    const auto v = parse_version("7");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->parts, (std::vector<int>{7}));
}

TEST(ParseVersion, RejectsMalformedInput) {
    EXPECT_FALSE(parse_version("").has_value());
    EXPECT_FALSE(parse_version("1..2").has_value());
    EXPECT_FALSE(parse_version("1.2.").has_value());
    EXPECT_FALSE(parse_version("1.x").has_value());
    EXPECT_FALSE(parse_version("-1.2").has_value());
    EXPECT_FALSE(parse_version("v1.2").has_value());
}

TEST(CompareVersion, TrailingZerosAreInsignificant) {
    EXPECT_EQ(compare(*parse_version("1.2"), *parse_version("1.2.0")), 0);
    EXPECT_EQ(compare(*parse_version("1.2.0.0"), *parse_version("1.2")), 0);
}

TEST(CompareVersion, OrdersNumericallyNotLexically) {
    EXPECT_LT(compare(*parse_version("1.9"), *parse_version("1.10")), 0);
    EXPECT_GT(compare(*parse_version("2.0"), *parse_version("1.99")), 0);
}

TEST(Satisfies, HandlesEveryOperator) {
    const Version actual = *parse_version("3.4.1");

    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.1.0")}));
    EXPECT_FALSE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.2")}));

    EXPECT_TRUE(satisfies(actual, {ComparisonOp::NotEqual, *parse_version("3.4.2")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::GreaterEqual, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::GreaterEqual, *parse_version("3.0")}));
    EXPECT_FALSE(satisfies(actual, {ComparisonOp::Greater, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Less, *parse_version("4.0")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::LessEqual, *parse_version("3.4.1")}));
}

TEST(ToString, RoundTripsThroughParse) {
    EXPECT_EQ(to_string(*parse_version("10.0.19045")), "10.0.19045");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset windows-debug
```

Expected: FAIL — `lm/core/version.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/version.hpp`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lm::core {

struct Version {
    std::vector<int> parts;
    friend bool operator==(const Version&, const Version&) = default;
};

enum class ComparisonOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct VersionConstraint {
    ComparisonOp op = ComparisonOp::GreaterEqual;
    Version value;
    friend bool operator==(const VersionConstraint&, const VersionConstraint&) = default;
};

/// Parses a dotted numeric version. Returns nullopt for empty input, empty
/// components, negative numbers, or any non-digit character.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

/// Three-way comparison. Missing trailing components are treated as zero, so
/// "1.2" and "1.2.0" compare equal.
[[nodiscard]] int compare(const Version& lhs, const Version& rhs);

[[nodiscard]] bool satisfies(const Version& actual, const VersionConstraint& constraint);

[[nodiscard]] std::string to_string(const Version& version);

[[nodiscard]] std::string to_string(ComparisonOp op);
[[nodiscard]] std::optional<ComparisonOp> parse_comparison_op(std::string_view text);

}  // namespace lm::core
```

- [ ] **Step 4: Write `libs/core/src/version.cpp`**

```cpp
#include "lm/core/version.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace lm::core {
namespace {

int component_at(const Version& v, std::size_t index) {
    return index < v.parts.size() ? v.parts[index] : 0;
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    Version result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = text.find('.', start);
        const std::string_view part =
            text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);

        if (part.empty() || !std::all_of(part.begin(), part.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return std::nullopt;
        }

        int value = 0;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || ptr != part.data() + part.size()) {
            return std::nullopt;
        }
        result.parts.push_back(value);

        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    return result;
}

int compare(const Version& lhs, const Version& rhs) {
    const std::size_t count = std::max(lhs.parts.size(), rhs.parts.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int a = component_at(lhs, i);
        const int b = component_at(rhs, i);
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return 0;
}

bool satisfies(const Version& actual, const VersionConstraint& constraint) {
    const int result = compare(actual, constraint.value);
    switch (constraint.op) {
        case ComparisonOp::Equal:        return result == 0;
        case ComparisonOp::NotEqual:     return result != 0;
        case ComparisonOp::Less:         return result < 0;
        case ComparisonOp::LessEqual:    return result <= 0;
        case ComparisonOp::Greater:      return result > 0;
        case ComparisonOp::GreaterEqual: return result >= 0;
    }
    return false;
}

std::string to_string(const Version& version) {
    std::string out;
    for (std::size_t i = 0; i < version.parts.size(); ++i) {
        if (i > 0) {
            out += '.';
        }
        out += std::to_string(version.parts[i]);
    }
    return out;
}

std::string to_string(ComparisonOp op) {
    switch (op) {
        case ComparisonOp::Equal:        return "==";
        case ComparisonOp::NotEqual:     return "!=";
        case ComparisonOp::Less:         return "<";
        case ComparisonOp::LessEqual:    return "<=";
        case ComparisonOp::Greater:      return ">";
        case ComparisonOp::GreaterEqual: return ">=";
    }
    return "==";
}

std::optional<ComparisonOp> parse_comparison_op(std::string_view text) {
    if (text == "==") return ComparisonOp::Equal;
    if (text == "!=") return ComparisonOp::NotEqual;
    if (text == "<")  return ComparisonOp::Less;
    if (text == "<=") return ComparisonOp::LessEqual;
    if (text == ">")  return ComparisonOp::Greater;
    if (text == ">=") return ComparisonOp::GreaterEqual;
    return std::nullopt;
}

}  // namespace lm::core
```

- [ ] **Step 5: Update `libs/core/CMakeLists.txt`**

```cmake
add_library(lm_core STATIC
  src/types.cpp
  src/version.cpp)

lm_add_test(lm_core_tests
  SOURCES tests/test_smoke.cpp tests/test_version.cpp
  LINK lm_core)
```

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/core
git commit -m "feat: add version parsing and constraint comparison to lm_core"
```

---

## Task 3: Rules, host facts and template bundles

**Files:**
- Create: `libs/core/include/lm/core/rule.hpp`, `libs/core/include/lm/core/host_facts.hpp`
- Create: `libs/core/include/lm/core/template_bundle.hpp`, `libs/core/src/template_bundle.cpp`
- Create: `libs/core/src/rule.cpp`
- Modify: `libs/core/CMakeLists.txt`
- Test: `libs/core/tests/test_template_bundle.cpp`

**Interfaces:**
- Consumes: `HostId`, `RuleId`, `RuleKind`, `Presence`, `ServiceState` (Task 1); `VersionConstraint` (Task 2).
- Produces: `ProcessRule`, `ServiceRule`, `RegistryRule`, `RulePayload` variant, `Rule`, `kind_of(const Rule&) -> RuleKind`, `registry_key(const RegistryRule&) -> std::string`; `DiskUsage`, `ResourceSample`, `ProcessInfo`, `ServiceInfo`, `RegistryValue`, `HostFacts`; `Template`, `TemplateBundle`, `rules_for(const TemplateBundle&, const HostId&) -> std::vector<const Rule*>`.

> **Design note — one deviation from the spec.** The spec's `Rule` carries both a
> `kind` field and a payload. That is two sources of truth that can disagree. This plan
> drops the field and derives the kind from the payload variant via `kind_of()`.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_template_bundle.cpp`**

The behaviour that matters: `rules_for()` composes the baseline with every assigned template by union, ignores templates a host is not assigned, and never returns duplicates when two assigned templates share a rule id.

```cpp
#include <gtest/gtest.h>

#include <algorithm>

#include "lm/core/template_bundle.hpp"

using namespace lm::core;

namespace {

Rule process_rule(RuleId id, std::string exe, Presence presence = Presence::MustBePresent) {
    Rule rule;
    rule.id = std::move(id);
    rule.description = "process " + exe;
    rule.expectation = presence;
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

std::vector<RuleId> ids_of(const std::vector<const Rule*>& rules) {
    std::vector<RuleId> out;
    out.reserve(rules.size());
    for (const Rule* rule : rules) {
        out.push_back(rule->id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

TemplateBundle make_bundle() {
    TemplateBundle bundle;
    bundle.revision = 4;
    bundle.baseline.name = "baseline";
    bundle.baseline.rules.push_back(process_rule("base-1", "antivirus.exe"));

    Template workstation;
    workstation.name = "Lab Workstation";
    workstation.rules.push_back(process_rule("ws-1", "labtool.exe"));
    workstation.rules.push_back(process_rule("shared-1", "agent.exe"));

    Template build_server;
    build_server.name = "Build Server";
    build_server.rules.push_back(process_rule("bs-1", "buildd.exe"));
    build_server.rules.push_back(process_rule("shared-1", "agent.exe"));

    bundle.templates = {workstation, build_server};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    bundle.assignments["PC-002"] = {"Lab Workstation", "Build Server"};
    bundle.assignments["PC-003"] = {"Nonexistent Template"};
    return bundle;
}

}  // namespace

TEST(KindOf, DerivesFromPayload) {
    EXPECT_EQ(kind_of(process_rule("a", "x.exe")), RuleKind::Process);

    Rule service;
    service.payload = ServiceRule{"spooler", ServiceState::Running};
    EXPECT_EQ(kind_of(service), RuleKind::Service);

    Rule registry;
    registry.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                    "Version", RegistryMatch::Exists, ""};
    EXPECT_EQ(kind_of(registry), RuleKind::Registry);
}

TEST(RulesFor, UnassignedHostGetsBaselineOnly) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "UNKNOWN-PC")), (std::vector<RuleId>{"base-1"}));
}

TEST(RulesFor, AppliesBaselinePlusAssignedTemplate) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-001")),
              (std::vector<RuleId>{"base-1", "shared-1", "ws-1"}));
}

TEST(RulesFor, DeduplicatesRulesSharedByTwoTemplates) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-002")),
              (std::vector<RuleId>{"base-1", "bs-1", "shared-1", "ws-1"}));
}

TEST(RulesFor, IgnoresAssignmentsToMissingTemplates) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-003")), (std::vector<RuleId>{"base-1"}));
}

TEST(RulesFor, EmptyBundleYieldsNoRules) {
    const TemplateBundle empty;
    EXPECT_TRUE(rules_for(empty, "PC-001").empty());
}

TEST(RegistryKey, BuildsCanonicalLookupPath) {
    const RegistryRule rule{RegistryHive::LocalMachine, "SOFTWARE\\Acme\\Tool",
                            "Version", RegistryMatch::Exists, ""};
    EXPECT_EQ(registry_key(rule), "HKLM\\SOFTWARE\\Acme\\Tool\\\\Version");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset windows-debug
```

Expected: FAIL — `lm/core/template_bundle.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/rule.hpp`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <variant>

#include "lm/core/types.hpp"
#include "lm/core/version.hpp"

namespace lm::core {

enum class RegistryHive { LocalMachine, CurrentUser, ClassesRoot, Users };
enum class RegistryMatch { Exists, Equals, Contains };

struct ProcessRule {
    std::string executable;
    friend bool operator==(const ProcessRule&, const ProcessRule&) = default;
};

struct ServiceRule {
    std::string service_name;
    std::optional<ServiceState> expected_state;
    friend bool operator==(const ServiceRule&, const ServiceRule&) = default;
};

struct RegistryRule {
    RegistryHive hive = RegistryHive::LocalMachine;
    std::string key_path;
    std::string value_name;
    RegistryMatch match = RegistryMatch::Exists;
    std::string expected_value;
    friend bool operator==(const RegistryRule&, const RegistryRule&) = default;
};

using RulePayload = std::variant<ProcessRule, ServiceRule, RegistryRule>;

struct Rule {
    RuleId id;
    std::string description;
    Presence expectation = Presence::MustBePresent;
    RulePayload payload;
    /// Process rules only. Ignored for service and registry rules.
    std::optional<VersionConstraint> version;
    friend bool operator==(const Rule&, const Rule&) = default;
};

/// Derives the rule kind from the payload variant. The kind is never stored,
/// so it cannot disagree with the payload.
[[nodiscard]] RuleKind kind_of(const Rule& rule);

[[nodiscard]] std::string to_string(RegistryHive hive);
[[nodiscard]] std::optional<RegistryHive> parse_registry_hive(std::string_view text);

/// Canonical lookup key for HostFacts::registry, of the form
/// "HKLM\<key_path>\\<value_name>".
[[nodiscard]] std::string registry_key(const RegistryRule& rule);

}  // namespace lm::core
```

- [ ] **Step 4: Write `libs/core/src/rule.cpp`**

```cpp
#include "lm/core/rule.hpp"

namespace lm::core {

RuleKind kind_of(const Rule& rule) {
    return std::visit(
        [](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ProcessRule>) {
                return RuleKind::Process;
            } else if constexpr (std::is_same_v<T, ServiceRule>) {
                return RuleKind::Service;
            } else {
                return RuleKind::Registry;
            }
        },
        rule.payload);
}

std::string to_string(RegistryHive hive) {
    switch (hive) {
        case RegistryHive::LocalMachine: return "HKLM";
        case RegistryHive::CurrentUser:  return "HKCU";
        case RegistryHive::ClassesRoot:  return "HKCR";
        case RegistryHive::Users:        return "HKU";
    }
    return "HKLM";
}

std::optional<RegistryHive> parse_registry_hive(std::string_view text) {
    if (text == "HKLM") return RegistryHive::LocalMachine;
    if (text == "HKCU") return RegistryHive::CurrentUser;
    if (text == "HKCR") return RegistryHive::ClassesRoot;
    if (text == "HKU")  return RegistryHive::Users;
    return std::nullopt;
}

std::string registry_key(const RegistryRule& rule) {
    return to_string(rule.hive) + "\\" + rule.key_path + "\\\\" + rule.value_name;
}

}  // namespace lm::core
```

- [ ] **Step 5: Write `libs/core/include/lm/core/host_facts.hpp`**

`registry` is a map keyed by `registry_key()` so `evaluate()` can look values up in
one step without re-deriving paths.

```cpp
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/types.hpp"
#include "lm/core/version.hpp"

namespace lm::core {

struct DiskUsage {
    std::string mount;
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    friend bool operator==(const DiskUsage&, const DiskUsage&) = default;

    [[nodiscard]] double used_percent() const {
        return total_bytes == 0
                   ? 0.0
                   : 100.0 * static_cast<double>(total_bytes - free_bytes) /
                         static_cast<double>(total_bytes);
    }
};

struct ResourceSample {
    double cpu_percent = 0.0;
    std::uint64_t mem_total_bytes = 0;
    std::uint64_t mem_used_bytes = 0;
    std::vector<DiskUsage> disks;
    friend bool operator==(const ResourceSample&, const ResourceSample&) = default;
};

struct ProcessInfo {
    std::string executable;
    std::optional<Version> version;
    friend bool operator==(const ProcessInfo&, const ProcessInfo&) = default;
};

struct ServiceInfo {
    std::string name;
    ServiceState state = ServiceState::Unknown;
    friend bool operator==(const ServiceInfo&, const ServiceInfo&) = default;
};

struct RegistryValue {
    bool exists = false;
    std::string data;
    /// Set when the read itself failed, as opposed to the value being absent.
    std::string error;
    friend bool operator==(const RegistryValue&, const RegistryValue&) = default;
};

struct HostFacts {
    HostId host_id;
    ResourceSample resources;
    std::vector<ProcessInfo> processes;
    std::vector<ServiceInfo> services;
    /// Keyed by registry_key(). Absent entries mean the rule was never probed.
    std::map<std::string, RegistryValue> registry;
    friend bool operator==(const HostFacts&, const HostFacts&) = default;
};

}  // namespace lm::core
```

- [ ] **Step 6: Write `libs/core/include/lm/core/template_bundle.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lm/core/rule.hpp"

namespace lm::core {

struct Template {
    std::string name;
    std::vector<Rule> rules;
    friend bool operator==(const Template&, const Template&) = default;
};

struct TemplateBundle {
    std::uint64_t revision = 0;
    std::string hash;
    Template baseline;
    std::vector<Template> templates;
    /// Host id -> names of assigned templates.
    std::map<HostId, std::vector<std::string>> assignments;
    friend bool operator==(const TemplateBundle&, const TemplateBundle&) = default;
};

/// Returns the baseline rules plus the rules of every template assigned to the
/// host, deduplicated by rule id. Assignments naming a template that does not
/// exist are ignored. Pointers remain valid as long as the bundle does.
[[nodiscard]] std::vector<const Rule*> rules_for(const TemplateBundle& bundle,
                                                 const HostId& host_id);

}  // namespace lm::core
```

- [ ] **Step 7: Write `libs/core/src/template_bundle.cpp`**

```cpp
#include "lm/core/template_bundle.hpp"

#include <algorithm>
#include <unordered_set>

namespace lm::core {

std::vector<const Rule*> rules_for(const TemplateBundle& bundle, const HostId& host_id) {
    std::vector<const Rule*> result;
    std::unordered_set<std::string> seen;

    const auto append = [&](const Template& tmpl) {
        for (const Rule& rule : tmpl.rules) {
            if (seen.insert(rule.id).second) {
                result.push_back(&rule);
            }
        }
    };

    append(bundle.baseline);

    const auto assignment = bundle.assignments.find(host_id);
    if (assignment == bundle.assignments.end()) {
        return result;
    }

    for (const std::string& name : assignment->second) {
        const auto tmpl = std::find_if(bundle.templates.begin(), bundle.templates.end(),
                                       [&](const Template& t) { return t.name == name; });
        if (tmpl != bundle.templates.end()) {
            append(*tmpl);
        }
    }

    return result;
}

}  // namespace lm::core
```

- [ ] **Step 8: Update `libs/core/CMakeLists.txt`**

```cmake
add_library(lm_core STATIC
  src/types.cpp
  src/version.cpp
  src/rule.cpp
  src/template_bundle.cpp)

lm_add_test(lm_core_tests
  SOURCES
    tests/test_smoke.cpp
    tests/test_version.cpp
    tests/test_template_bundle.cpp
  LINK lm_core)
```

- [ ] **Step 9: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 10: Commit**

```bash
git add libs/core
git commit -m "feat: add rule, host facts and template bundle types to lm_core"
```

---

## Task 4: JSON serialisation and content hashing

**Files:**
- Create: `libs/core/include/lm/core/json.hpp`, `libs/core/src/json.cpp`
- Modify: `libs/core/CMakeLists.txt`
- Test: `libs/core/tests/test_json.cpp`

**Interfaces:**
- Consumes: every type from Tasks 1–3.
- Produces: `to_json`/`from_json` ADL overloads for `Version`, `VersionConstraint`, `ProcessRule`, `ServiceRule`, `RegistryRule`, `Rule`, `Template`, `TemplateBundle`; `content_hash(const TemplateBundle&) -> std::string`; `parse_bundle(std::string_view) -> std::expected<TemplateBundle, std::string>`; `serialise_bundle(const TemplateBundle&) -> std::string`.

> **Note on hashing.** `content_hash` covers the baseline, templates and assignments —
> **not** `revision` or `hash` themselves. This lets the server detect "the draft differs
> from what was published" independently of the revision counter.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_json.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/core/json.hpp"

using namespace lm::core;

namespace {

TemplateBundle sample_bundle() {
    TemplateBundle bundle;
    bundle.revision = 7;

    Rule process;
    process.id = "r1";
    process.description = "Antivirus must run";
    process.expectation = Presence::MustBePresent;
    process.payload = ProcessRule{"antivirus.exe"};
    process.version = VersionConstraint{ComparisonOp::GreaterEqual, *parse_version("2.1")};

    Rule service;
    service.id = "r2";
    service.description = "Telnet must be absent";
    service.expectation = Presence::MustBeAbsent;
    service.payload = ServiceRule{"telnet", std::nullopt};

    Rule registry;
    registry.id = "r3";
    registry.description = "Tool version pinned";
    registry.expectation = Presence::MustBePresent;
    registry.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                    "Version", RegistryMatch::Equals, "4.2"};

    bundle.baseline.name = "baseline";
    bundle.baseline.rules = {process};

    Template workstation;
    workstation.name = "Lab Workstation";
    workstation.rules = {service, registry};
    bundle.templates = {workstation};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    return bundle;
}

}  // namespace

TEST(BundleJson, RoundTripsExactly) {
    const TemplateBundle original = sample_bundle();
    const std::string text = serialise_bundle(original);

    const auto parsed = parse_bundle(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, original);
}

TEST(BundleJson, RoundTripsAnEmptyBundle) {
    const TemplateBundle empty;
    const auto parsed = parse_bundle(serialise_bundle(empty));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, empty);
}

TEST(BundleJson, ReportsMalformedJsonAsError) {
    const auto parsed = parse_bundle("{ not json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_FALSE(parsed.error().empty());
}

TEST(BundleJson, ReportsUnknownEnumAsError) {
    const auto parsed = parse_bundle(R"({"revision":1,"hash":"","baseline":{"name":"b",
        "rules":[{"id":"x","description":"","expectation":"Sometimes",
        "payload":{"type":"process","executable":"a.exe"}}]},
        "templates":[],"assignments":{}})");
    ASSERT_FALSE(parsed.has_value());
}

TEST(ContentHash, IsStableAcrossSerialisation) {
    const TemplateBundle original = sample_bundle();
    const auto parsed = parse_bundle(serialise_bundle(original));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(content_hash(original), content_hash(*parsed));
}

TEST(ContentHash, IgnoresRevisionAndHashFields) {
    TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.revision = 999;
    b.hash = "stale";
    EXPECT_EQ(content_hash(a), content_hash(b));
}

TEST(ContentHash, ChangesWhenARuleChanges) {
    const TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.templates[0].rules[0].expectation = Presence::MustBePresent;
    EXPECT_NE(content_hash(a), content_hash(b));
}

TEST(ContentHash, ChangesWhenAnAssignmentChanges) {
    const TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.assignments["PC-002"] = {"Lab Workstation"};
    EXPECT_NE(content_hash(a), content_hash(b));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset windows-debug
```

Expected: FAIL — `lm/core/json.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/json.hpp`**

```cpp
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lm/core/compliance_fwd.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::core {

void to_json(nlohmann::json& j, const Version& value);
void from_json(const nlohmann::json& j, Version& value);

void to_json(nlohmann::json& j, const VersionConstraint& value);
void from_json(const nlohmann::json& j, VersionConstraint& value);

void to_json(nlohmann::json& j, const Rule& value);
void from_json(const nlohmann::json& j, Rule& value);

void to_json(nlohmann::json& j, const Template& value);
void from_json(const nlohmann::json& j, Template& value);

void to_json(nlohmann::json& j, const TemplateBundle& value);
void from_json(const nlohmann::json& j, TemplateBundle& value);

[[nodiscard]] std::string serialise_bundle(const TemplateBundle& bundle);

/// Parses a bundle. Returns the parse or validation error message on failure;
/// never throws.
[[nodiscard]] std::expected<TemplateBundle, std::string> parse_bundle(std::string_view text);

/// Stable hash over baseline, templates and assignments. Deliberately excludes
/// revision and hash so a draft can be compared against a published bundle.
[[nodiscard]] std::string content_hash(const TemplateBundle& bundle);

}  // namespace lm::core
```

Create `libs/core/include/lm/core/compliance_fwd.hpp` as an empty forward-declaration
header for now — Task 5 fills it in. It exists so `json.hpp`'s include list does not
change later.

```cpp
#pragma once

namespace lm::core {
struct CheckResult;
struct ComplianceReport;
}  // namespace lm::core
```

- [ ] **Step 4: Write `libs/core/src/json.cpp`**

Enum strings are written explicitly rather than via `NLOHMANN_JSON_SERIALIZE_ENUM`,
because that macro silently maps unknown strings to the first enumerator — which would
turn a typo in a hand-edited template into a valid rule. Here an unknown string throws
and `parse_bundle` reports it.

```cpp
#include "lm/core/json.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace lm::core {
namespace {

std::string presence_to_string(Presence value) {
    return value == Presence::MustBePresent ? "MustBePresent" : "MustBeAbsent";
}

Presence presence_from_string(const std::string& text) {
    if (text == "MustBePresent") return Presence::MustBePresent;
    if (text == "MustBeAbsent") return Presence::MustBeAbsent;
    throw std::runtime_error("unknown expectation: " + text);
}

std::string service_state_to_string(ServiceState value) {
    switch (value) {
        case ServiceState::Running: return "Running";
        case ServiceState::Stopped: return "Stopped";
        case ServiceState::Unknown: return "Unknown";
    }
    return "Unknown";
}

ServiceState service_state_from_string(const std::string& text) {
    if (text == "Running") return ServiceState::Running;
    if (text == "Stopped") return ServiceState::Stopped;
    if (text == "Unknown") return ServiceState::Unknown;
    throw std::runtime_error("unknown service state: " + text);
}

std::string registry_match_to_string(RegistryMatch value) {
    switch (value) {
        case RegistryMatch::Exists:   return "Exists";
        case RegistryMatch::Equals:   return "Equals";
        case RegistryMatch::Contains: return "Contains";
    }
    return "Exists";
}

RegistryMatch registry_match_from_string(const std::string& text) {
    if (text == "Exists")   return RegistryMatch::Exists;
    if (text == "Equals")   return RegistryMatch::Equals;
    if (text == "Contains") return RegistryMatch::Contains;
    throw std::runtime_error("unknown registry match: " + text);
}

}  // namespace

void to_json(nlohmann::json& j, const Version& value) { j = to_string(value); }

void from_json(const nlohmann::json& j, Version& value) {
    const auto parsed = parse_version(j.get<std::string>());
    if (!parsed) {
        throw std::runtime_error("malformed version: " + j.get<std::string>());
    }
    value = *parsed;
}

void to_json(nlohmann::json& j, const VersionConstraint& value) {
    j = nlohmann::json{{"op", to_string(value.op)}, {"value", value.value}};
}

void from_json(const nlohmann::json& j, VersionConstraint& value) {
    const auto op = parse_comparison_op(j.at("op").get<std::string>());
    if (!op) {
        throw std::runtime_error("unknown comparison operator");
    }
    value.op = *op;
    j.at("value").get_to(value.value);
}

void to_json(nlohmann::json& j, const Rule& value) {
    nlohmann::json payload;
    std::visit(
        [&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ProcessRule>) {
                payload = {{"type", "process"}, {"executable", p.executable}};
            } else if constexpr (std::is_same_v<T, ServiceRule>) {
                payload = {{"type", "service"}, {"service_name", p.service_name}};
                if (p.expected_state) {
                    payload["expected_state"] = service_state_to_string(*p.expected_state);
                }
            } else {
                payload = {{"type", "registry"},
                           {"hive", to_string(p.hive)},
                           {"key_path", p.key_path},
                           {"value_name", p.value_name},
                           {"match", registry_match_to_string(p.match)},
                           {"expected_value", p.expected_value}};
            }
        },
        value.payload);

    j = nlohmann::json{{"id", value.id},
                       {"description", value.description},
                       {"expectation", presence_to_string(value.expectation)},
                       {"payload", payload}};
    if (value.version) {
        j["version"] = *value.version;
    }
}

void from_json(const nlohmann::json& j, Rule& value) {
    j.at("id").get_to(value.id);
    j.at("description").get_to(value.description);
    value.expectation = presence_from_string(j.at("expectation").get<std::string>());

    const nlohmann::json& payload = j.at("payload");
    const std::string type = payload.at("type").get<std::string>();
    if (type == "process") {
        value.payload = ProcessRule{payload.at("executable").get<std::string>()};
    } else if (type == "service") {
        ServiceRule rule;
        payload.at("service_name").get_to(rule.service_name);
        if (payload.contains("expected_state")) {
            rule.expected_state =
                service_state_from_string(payload.at("expected_state").get<std::string>());
        }
        value.payload = rule;
    } else if (type == "registry") {
        RegistryRule rule;
        const auto hive = parse_registry_hive(payload.at("hive").get<std::string>());
        if (!hive) {
            throw std::runtime_error("unknown registry hive");
        }
        rule.hive = *hive;
        payload.at("key_path").get_to(rule.key_path);
        payload.at("value_name").get_to(rule.value_name);
        rule.match = registry_match_from_string(payload.at("match").get<std::string>());
        payload.at("expected_value").get_to(rule.expected_value);
        value.payload = rule;
    } else {
        throw std::runtime_error("unknown rule payload type: " + type);
    }

    value.version.reset();
    if (j.contains("version")) {
        value.version = j.at("version").get<VersionConstraint>();
    }
}

void to_json(nlohmann::json& j, const Template& value) {
    j = nlohmann::json{{"name", value.name}, {"rules", value.rules}};
}

void from_json(const nlohmann::json& j, Template& value) {
    j.at("name").get_to(value.name);
    j.at("rules").get_to(value.rules);
}

void to_json(nlohmann::json& j, const TemplateBundle& value) {
    j = nlohmann::json{{"revision", value.revision},
                       {"hash", value.hash},
                       {"baseline", value.baseline},
                       {"templates", value.templates},
                       {"assignments", value.assignments}};
}

void from_json(const nlohmann::json& j, TemplateBundle& value) {
    j.at("revision").get_to(value.revision);
    j.at("hash").get_to(value.hash);
    j.at("baseline").get_to(value.baseline);
    j.at("templates").get_to(value.templates);
    j.at("assignments").get_to(value.assignments);
}

std::string serialise_bundle(const TemplateBundle& bundle) {
    return nlohmann::json(bundle).dump(2);
}

std::expected<TemplateBundle, std::string> parse_bundle(std::string_view text) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        return parsed.get<TemplateBundle>();
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}

std::string content_hash(const TemplateBundle& bundle) {
    TemplateBundle content = bundle;
    content.revision = 0;
    content.hash.clear();

    // Ordered dump: nlohmann's default object type sorts keys, and templates and
    // assignments are already stored in a deterministic order.
    const std::string canonical = nlohmann::json(content).dump();

    // FNV-1a 64. Deliberately not std::hash, which is implementation-defined and
    // would give different results across platforms and standard libraries — this
    // value is persisted to disk and compared after restart.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = "0123456789abcdef"[hash & 0xfu];
        hash >>= 4;
    }
    return out;
}

}  // namespace lm::core
```

- [ ] **Step 5: Update `libs/core/CMakeLists.txt`**

```cmake
add_library(lm_core STATIC
  src/types.cpp
  src/version.cpp
  src/rule.cpp
  src/template_bundle.cpp
  src/json.cpp)

lm_add_test(lm_core_tests
  SOURCES
    tests/test_smoke.cpp
    tests/test_version.cpp
    tests/test_template_bundle.cpp
    tests/test_json.cpp
  LINK lm_core)
```

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/core
git commit -m "feat: add JSON serialisation and content hashing to lm_core"
```

---

## Task 5: `evaluate()` — the compliance rule engine

**Files:**
- Create: `libs/core/include/lm/core/compliance.hpp`, `libs/core/src/compliance.cpp`
- Modify: `libs/core/CMakeLists.txt`
- Test: `libs/core/tests/test_evaluate.cpp`

**Interfaces:**
- Consumes: `rules_for()` (Task 3), `HostFacts` (Task 3), `Capabilities`/`required_capability()` (Task 1), `satisfies()` (Task 2).
- Produces: `CheckResult{rule_id, status, observed, message}`, `ComplianceReport{host_id, applied_revision, results}`, `evaluate(const TemplateBundle&, const HostFacts&, Capabilities) -> ComplianceReport`, `count_by_status(const ComplianceReport&, CheckStatus) -> std::size_t`, `is_compliant(const ComplianceReport&) -> bool`.

**Semantics this task locks in:**

| Situation | Result |
|---|---|
| Required capability missing | `NotApplicable` |
| Executable / service name match | Case-insensitive on every platform |
| `MustBePresent`, found, version satisfied | `Pass` |
| `MustBePresent`, found, version violated | `Fail` |
| `MustBePresent`, found, version constraint but no version readable | `Error` |
| `MustBeAbsent`, found | `Fail` (version constraint ignored) |
| Registry key not present in `HostFacts::registry` | `Error` — never silently a pass |
| Registry read failed (`RegistryValue::error` set) | `Error` carrying the OS message |

`is_compliant` treats only `Fail` as non-compliant. `NotApplicable` and `Error` are
reported but do not make a host non-compliant, so a Linux box is never red for a
registry rule and a transient probe failure is not an outage.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_evaluate.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/core/compliance.hpp"

using namespace lm::core;

namespace {

Capabilities all_capabilities() {
    return Capabilities{}
        .add(Capability::Resources)
        .add(Capability::Processes)
        .add(Capability::Services)
        .add(Capability::Registry);
}

TemplateBundle bundle_with(Rule rule) {
    TemplateBundle bundle;
    bundle.revision = 3;
    bundle.baseline.name = "baseline";
    bundle.baseline.rules.push_back(std::move(rule));
    return bundle;
}

Rule process_rule(Presence expectation, std::optional<VersionConstraint> version = std::nullopt) {
    Rule rule;
    rule.id = "p1";
    rule.expectation = expectation;
    rule.payload = ProcessRule{"antivirus.exe"};
    rule.version = std::move(version);
    return rule;
}

Rule service_rule(Presence expectation, std::optional<ServiceState> state = std::nullopt) {
    Rule rule;
    rule.id = "s1";
    rule.expectation = expectation;
    rule.payload = ServiceRule{"spooler", state};
    return rule;
}

Rule registry_rule(Presence expectation, RegistryMatch match, std::string expected) {
    Rule rule;
    rule.id = "g1";
    rule.expectation = expectation;
    rule.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                "Version", match, std::move(expected)};
    return rule;
}

HostFacts facts_with_process(std::string exe, std::optional<Version> version = std::nullopt) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.processes.push_back(ProcessInfo{std::move(exe), std::move(version)});
    return facts;
}

CheckStatus status_of(const ComplianceReport& report) {
    EXPECT_EQ(report.results.size(), 1u);
    return report.results.front().status;
}

}  // namespace

// --- capability gating -----------------------------------------------------

TEST(Evaluate, RegistryRuleIsNotApplicableWithoutTheCapability) {
    const Capabilities linux_caps =
        Capabilities{}.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
    const auto report = evaluate(bundle_with(registry_rule(Presence::MustBePresent,
                                                           RegistryMatch::Exists, "")),
                                 HostFacts{}, linux_caps);
    EXPECT_EQ(status_of(report), CheckStatus::NotApplicable);
}

TEST(Evaluate, NotApplicableDoesNotMakeAHostNonCompliant) {
    const Capabilities linux_caps = Capabilities{}.add(Capability::Processes);
    const auto report = evaluate(bundle_with(registry_rule(Presence::MustBePresent,
                                                           RegistryMatch::Exists, "")),
                                 HostFacts{}, linux_caps);
    EXPECT_TRUE(is_compliant(report));
}

// --- report metadata -------------------------------------------------------

TEST(Evaluate, CarriesHostIdAndAppliedRevision) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(report.host_id, "PC-001");
    EXPECT_EQ(report.applied_revision, 3u);
}

TEST(Evaluate, EmptyBundleProducesEmptyCompliantReport) {
    const auto report = evaluate(TemplateBundle{}, facts_with_process("anything.exe"),
                                 all_capabilities());
    EXPECT_TRUE(report.results.empty());
    EXPECT_TRUE(is_compliant(report));
}

// --- process rules ---------------------------------------------------------

TEST(Evaluate, ProcessMustBePresentPassesWhenRunning) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessNameMatchIsCaseInsensitive) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("AntiVirus.EXE"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessMustBePresentFailsWhenAbsent) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_FALSE(is_compliant(report));
}

TEST(Evaluate, ProcessMustBeAbsentPassesWhenAbsent) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessMustBeAbsentFailsWhenRunning) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
}

TEST(Evaluate, ProcessVersionConstraintSatisfied) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("2.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", parse_version("2.1")),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessVersionConstraintViolatedFails) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", parse_version("2.1")),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_NE(report.results.front().observed.find("2.1"), std::string::npos);
}

TEST(Evaluate, ProcessVersionUnreadableIsAnErrorNotAFailure) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", std::nullopt),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
    EXPECT_FALSE(report.results.front().message.empty());
}

TEST(Evaluate, MustBeAbsentIgnoresVersionConstraint) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent, constraint)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

// --- service rules ---------------------------------------------------------

TEST(Evaluate, ServiceMustBePresentPassesWhenInstalled) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"Spooler", ServiceState::Running});
    const auto report = evaluate(bundle_with(service_rule(Presence::MustBePresent)), facts,
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ServiceExpectedStateMismatchFails) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Stopped});
    const auto report = evaluate(
        bundle_with(service_rule(Presence::MustBePresent, ServiceState::Running)), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_NE(report.results.front().observed.find("Stopped"), std::string::npos);
}

TEST(Evaluate, ServiceExpectedStateMatchPasses) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Running});
    const auto report = evaluate(
        bundle_with(service_rule(Presence::MustBePresent, ServiceState::Running)), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ServiceMustBeAbsentFailsWhenInstalled) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Stopped});
    const auto report = evaluate(bundle_with(service_rule(Presence::MustBeAbsent)), facts,
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
}

// --- registry rules --------------------------------------------------------

TEST(Evaluate, RegistryUnprobedKeyIsAnError) {
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")),
        HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
}

TEST(Evaluate, RegistryReadFailureSurfacesTheOsMessage) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] =
        RegistryValue{false, "", "ERROR_ACCESS_DENIED"};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
    EXPECT_EQ(report.results.front().message, "ERROR_ACCESS_DENIED");
}

TEST(Evaluate, RegistryExistsPasses) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, RegistryEqualsComparesExactValue) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2", ""};

    const auto match = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "4.2")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(match), CheckStatus::Pass);

    const auto mismatch = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "4.3")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(mismatch), CheckStatus::Fail);
}

TEST(Evaluate, RegistryContainsMatchesSubstring) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2-beta", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Contains, "beta")),
        facts, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, RegistryMustBeAbsentPassesWhenValueMissing) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{false, "", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBeAbsent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

// --- aggregation -----------------------------------------------------------

TEST(CountByStatus, TalliesEachStatus) {
    TemplateBundle bundle;
    bundle.baseline.rules = {process_rule(Presence::MustBePresent),
                             registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")};
    bundle.baseline.rules[1].id = "g1";

    const Capabilities caps = Capabilities{}.add(Capability::Processes);
    const auto report = evaluate(bundle, HostFacts{}, caps);

    EXPECT_EQ(count_by_status(report, CheckStatus::Fail), 1u);
    EXPECT_EQ(count_by_status(report, CheckStatus::NotApplicable), 1u);
    EXPECT_EQ(count_by_status(report, CheckStatus::Pass), 0u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset windows-debug
```

Expected: FAIL — `lm/core/compliance.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/compliance.hpp`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::core {

struct CheckResult {
    RuleId rule_id;
    CheckStatus status = CheckStatus::NotApplicable;
    /// Human-readable description of what was actually observed.
    std::string observed;
    /// Populated only when status == CheckStatus::Error.
    std::string message;
    friend bool operator==(const CheckResult&, const CheckResult&) = default;
};

struct ComplianceReport {
    HostId host_id;
    std::uint64_t applied_revision = 0;
    std::vector<CheckResult> results;
    friend bool operator==(const ComplianceReport&, const ComplianceReport&) = default;
};

/// Evaluates every rule applying to facts.host_id. Pure: no I/O, no clock, no
/// global state. Rules whose required capability is absent from caps yield
/// CheckStatus::NotApplicable.
[[nodiscard]] ComplianceReport evaluate(const TemplateBundle& bundle, const HostFacts& facts,
                                        Capabilities caps);

[[nodiscard]] std::size_t count_by_status(const ComplianceReport& report, CheckStatus status);

/// Only CheckStatus::Fail counts against compliance. NotApplicable and Error are
/// reported but do not mark a host as non-compliant.
[[nodiscard]] bool is_compliant(const ComplianceReport& report);

}  // namespace lm::core
```

- [ ] **Step 4: Write `libs/core/src/compliance.cpp`**

```cpp
#include "lm/core/compliance.hpp"

#include <algorithm>
#include <cctype>

namespace lm::core {
namespace {

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

CheckResult resolve(const Rule& rule, bool present, std::string observed) {
    CheckResult result;
    result.rule_id = rule.id;
    result.observed = std::move(observed);
    const bool wanted = rule.expectation == Presence::MustBePresent;
    result.status = (present == wanted) ? CheckStatus::Pass : CheckStatus::Fail;
    return result;
}

CheckResult error(const Rule& rule, std::string observed, std::string message) {
    CheckResult result;
    result.rule_id = rule.id;
    result.status = CheckStatus::Error;
    result.observed = std::move(observed);
    result.message = std::move(message);
    return result;
}

CheckResult evaluate_process(const Rule& rule, const ProcessRule& payload,
                             const HostFacts& facts) {
    const auto found = std::find_if(
        facts.processes.begin(), facts.processes.end(),
        [&](const ProcessInfo& info) { return equals_ignore_case(info.executable, payload.executable); });

    if (found == facts.processes.end()) {
        return resolve(rule, false, "not running");
    }

    // A version constraint only qualifies presence. For MustBeAbsent the process
    // being there is already a failure, whatever its version.
    if (rule.version && rule.expectation == Presence::MustBePresent) {
        if (!found->version) {
            return error(rule, "running, version unreadable",
                         "process found but its version could not be determined");
        }
        const bool ok = satisfies(*found->version, *rule.version);
        CheckResult result;
        result.rule_id = rule.id;
        result.observed = "running, version " + to_string(*found->version);
        result.status = ok ? CheckStatus::Pass : CheckStatus::Fail;
        return result;
    }

    return resolve(rule, true, "running");
}

CheckResult evaluate_service(const Rule& rule, const ServiceRule& payload,
                             const HostFacts& facts) {
    const auto found = std::find_if(
        facts.services.begin(), facts.services.end(),
        [&](const ServiceInfo& info) { return equals_ignore_case(info.name, payload.service_name); });

    if (found == facts.services.end()) {
        return resolve(rule, false, "not installed");
    }

    const std::string state = [&] {
        switch (found->state) {
            case ServiceState::Running: return "Running";
            case ServiceState::Stopped: return "Stopped";
            case ServiceState::Unknown: return "Unknown";
        }
        return "Unknown";
    }();

    if (payload.expected_state && rule.expectation == Presence::MustBePresent) {
        CheckResult result;
        result.rule_id = rule.id;
        result.observed = state;
        result.status =
            (found->state == *payload.expected_state) ? CheckStatus::Pass : CheckStatus::Fail;
        return result;
    }

    return resolve(rule, true, state);
}

CheckResult evaluate_registry(const Rule& rule, const RegistryRule& payload,
                              const HostFacts& facts) {
    const auto entry = facts.registry.find(registry_key(payload));
    if (entry == facts.registry.end()) {
        return error(rule, "not probed", "registry value was not read on this host");
    }
    if (!entry->second.error.empty()) {
        return error(rule, "read failed", entry->second.error);
    }

    const RegistryValue& value = entry->second;
    if (!value.exists) {
        return resolve(rule, false, "value absent");
    }

    switch (payload.match) {
        case RegistryMatch::Exists:
            return resolve(rule, true, value.data);
        case RegistryMatch::Equals:
            return resolve(rule, value.data == payload.expected_value, value.data);
        case RegistryMatch::Contains:
            return resolve(rule, value.data.find(payload.expected_value) != std::string::npos,
                           value.data);
    }
    return error(rule, value.data, "unhandled registry match mode");
}

}  // namespace

ComplianceReport evaluate(const TemplateBundle& bundle, const HostFacts& facts,
                          Capabilities caps) {
    ComplianceReport report;
    report.host_id = facts.host_id;
    report.applied_revision = bundle.revision;

    for (const Rule* rule : rules_for(bundle, facts.host_id)) {
        if (!caps.has(required_capability(kind_of(*rule)))) {
            CheckResult result;
            result.rule_id = rule->id;
            result.status = CheckStatus::NotApplicable;
            result.observed = "not supported on this platform";
            report.results.push_back(std::move(result));
            continue;
        }

        report.results.push_back(std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ProcessRule>) {
                    return evaluate_process(*rule, payload, facts);
                } else if constexpr (std::is_same_v<T, ServiceRule>) {
                    return evaluate_service(*rule, payload, facts);
                } else {
                    return evaluate_registry(*rule, payload, facts);
                }
            },
            rule->payload));
    }

    return report;
}

std::size_t count_by_status(const ComplianceReport& report, CheckStatus status) {
    return static_cast<std::size_t>(
        std::count_if(report.results.begin(), report.results.end(),
                      [&](const CheckResult& result) { return result.status == status; }));
}

bool is_compliant(const ComplianceReport& report) {
    return count_by_status(report, CheckStatus::Fail) == 0;
}

}  // namespace lm::core
```

- [ ] **Step 5: Update `libs/core/CMakeLists.txt`**

Add `src/compliance.cpp` to the library sources and `tests/test_evaluate.cpp` to the
`lm_add_test` source list.

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add libs/core
git commit -m "feat: add compliance rule engine to lm_core"
```

---

## Task 6: `reconcile()` — fleet state machine

**Files:**
- Create: `libs/core/include/lm/core/fleet.hpp`, `libs/core/src/fleet.cpp`
- Modify: `libs/core/CMakeLists.txt`
- Test: `libs/core/tests/test_fleet.cpp`

**Interfaces:**
- Consumes: `HostId`, `TimePoint`, `Capabilities` (Task 1).
- Produces: `HostState`, `ExpectedHost{host_id, address}`, `DiscoveredClient{host_id, last_seen, caps, applied_revision}`, `FleetEntry{host_id, address, state, last_seen, stale, caps}`, `FleetCounts`, `FleetView{entries, counts}`, `ReconcileOptions{liveliness_lease, current_revision}`, `reconcile(...) -> FleetView`, `to_string(HostState)`.

**Semantics this task locks in:**

| Expected? | Reporting? | Within lease? | State |
|---|---|---|---|
| yes | yes | yes | `Online` |
| yes | yes | no | `Offline` |
| yes | no | — | `Missing` |
| no | yes | — | `Unexpected` |

`Unexpected` is deliberately **independent of the liveliness lease** — a machine that
was never expected is noteworthy whether or not it is currently chatty. How long such
records are retained is the caller's decision, not `reconcile`'s.

**Staleness is orthogonal to state.** A host is `stale` when it is reporting but its
`applied_revision` is behind `current_revision`. `Missing` hosts are never stale, because
they have never applied anything.

**Ordering:** entries are sorted most-urgent-first — `Missing`, `Offline`, `Unexpected`,
`Online` — then by host id. The server sidebar renders this order directly.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_fleet.cpp`**

```cpp
#include <gtest/gtest.h>

#include <algorithm>

#include "lm/core/fleet.hpp"

using namespace lm::core;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = Clock::time_point{} + 1'000'000s;

ReconcileOptions options(std::uint64_t revision = 0) {
    ReconcileOptions opts;
    opts.liveliness_lease = 10s;
    opts.current_revision = revision;
    return opts;
}

DiscoveredClient client(HostId id, TimePoint last_seen, std::uint64_t revision = 0) {
    DiscoveredClient c;
    c.host_id = std::move(id);
    c.last_seen = last_seen;
    c.applied_revision = revision;
    c.caps = platform_capabilities();
    return c;
}

const FleetEntry& entry_for(const FleetView& view, const HostId& id) {
    const auto found = std::find_if(view.entries.begin(), view.entries.end(),
                                    [&](const FleetEntry& e) { return e.host_id == id; });
    EXPECT_NE(found, view.entries.end());
    return *found;
}

}  // namespace

// --- the four states -------------------------------------------------------

TEST(Reconcile, ExpectedAndReportingWithinLeaseIsOnline) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {client("PC-001", kNow - 2s)}, kNow,
                                options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
    EXPECT_EQ(view.counts.online, 1u);
}

TEST(Reconcile, ExpectedButSilentBeyondLeaseIsOffline) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {client("PC-001", kNow - 30s)}, kNow,
                                options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Offline);
    EXPECT_EQ(view.counts.offline, 1u);
}

TEST(Reconcile, ExpectedButNeverSeenIsMissing) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {}, kNow, options());
    const FleetEntry& entry = entry_for(view, "PC-001");
    EXPECT_EQ(entry.state, HostState::Missing);
    EXPECT_FALSE(entry.last_seen.has_value());
    EXPECT_EQ(view.counts.missing, 1u);
}

TEST(Reconcile, ReportingButNotExpectedIsUnexpected) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow - 1s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "ROGUE-PC").state, HostState::Unexpected);
    EXPECT_EQ(view.counts.unexpected, 1u);
}

TEST(Reconcile, UnexpectedStaysUnexpectedBeyondTheLease) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow - 500s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "ROGUE-PC").state, HostState::Unexpected);
}

// --- lease boundary --------------------------------------------------------

TEST(Reconcile, ExactlyAtTheLeaseBoundaryIsStillOnline) {
    const auto view = reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 10s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}

TEST(Reconcile, OneTickPastTheLeaseIsOffline) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 10s - 1ms)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Offline);
}

TEST(Reconcile, ClockSkewFromTheFutureIsTreatedAsOnline) {
    const auto view = reconcile({{"PC-001", ""}}, {client("PC-001", kNow + 5s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}

// --- staleness -------------------------------------------------------------

TEST(Reconcile, ClientOnAnOlderRevisionIsStale) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow, 3)}, kNow, options(5));
    EXPECT_TRUE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 1u);
}

TEST(Reconcile, ClientOnTheCurrentRevisionIsNotStale) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow, 5)}, kNow, options(5));
    EXPECT_FALSE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 0u);
}

TEST(Reconcile, MissingHostsAreNeverStale) {
    const auto view = reconcile({{"PC-001", ""}}, {}, kNow, options(5));
    EXPECT_FALSE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 0u);
}

TEST(Reconcile, OfflineHostsStillReportTheirLastAppliedRevision) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 60s, 3)}, kNow, options(5));
    const FleetEntry& entry = entry_for(view, "PC-001");
    EXPECT_EQ(entry.state, HostState::Offline);
    EXPECT_TRUE(entry.stale);
}

// --- metadata and ordering -------------------------------------------------

TEST(Reconcile, CarriesTheConfiguredAddress) {
    const auto view = reconcile({{"PC-001", "10.0.0.7"}}, {client("PC-001", kNow)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").address, "10.0.0.7");
}

TEST(Reconcile, UnexpectedHostsHaveNoConfiguredAddress) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow)}, kNow, options());
    EXPECT_TRUE(entry_for(view, "ROGUE-PC").address.empty());
}

TEST(Reconcile, SortsMostUrgentFirstThenByName) {
    const std::vector<ExpectedHost> expected{
        {"PC-ONLINE-B", ""}, {"PC-ONLINE-A", ""}, {"PC-MISSING", ""}, {"PC-OFFLINE", ""}};
    const std::vector<DiscoveredClient> discovered{client("PC-ONLINE-B", kNow),
                                                   client("PC-ONLINE-A", kNow),
                                                   client("PC-OFFLINE", kNow - 60s),
                                                   client("ROGUE", kNow)};

    const auto view = reconcile(expected, discovered, kNow, options());

    std::vector<HostId> order;
    for (const FleetEntry& entry : view.entries) {
        order.push_back(entry.host_id);
    }
    EXPECT_EQ(order, (std::vector<HostId>{"PC-MISSING", "PC-OFFLINE", "ROGUE", "PC-ONLINE-A",
                                          "PC-ONLINE-B"}));
}

TEST(Reconcile, EmptyInputsProduceAnEmptyView) {
    const auto view = reconcile({}, {}, kNow, options());
    EXPECT_TRUE(view.entries.empty());
    EXPECT_EQ(view.counts.online, 0u);
    EXPECT_EQ(view.counts.missing, 0u);
}

TEST(Reconcile, DuplicateDiscoveryReportsCollapseToOneEntry) {
    const auto view = reconcile({{"PC-001", ""}},
                                {client("PC-001", kNow - 60s), client("PC-001", kNow)}, kNow,
                                options());
    EXPECT_EQ(view.entries.size(), 1u);
    // The most recent sighting wins.
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset windows-debug
```

Expected: FAIL — `lm/core/fleet.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/fleet.hpp`**

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/types.hpp"

namespace lm::core {

enum class HostState { Missing, Offline, Unexpected, Online };

struct ExpectedHost {
    HostId host_id;
    /// Hostname or IP address, as configured by the operator. Informational.
    std::string address;
    friend bool operator==(const ExpectedHost&, const ExpectedHost&) = default;
};

struct DiscoveredClient {
    HostId host_id;
    TimePoint last_seen{};
    Capabilities caps;
    std::uint64_t applied_revision = 0;
    friend bool operator==(const DiscoveredClient&, const DiscoveredClient&) = default;
};

struct FleetEntry {
    HostId host_id;
    std::string address;
    HostState state = HostState::Missing;
    std::optional<TimePoint> last_seen;
    /// Reporting, but on an older template revision than the server's current one.
    bool stale = false;
    Capabilities caps;
    friend bool operator==(const FleetEntry&, const FleetEntry&) = default;
};

struct FleetCounts {
    std::size_t online = 0;
    std::size_t offline = 0;
    std::size_t missing = 0;
    std::size_t unexpected = 0;
    std::size_t stale = 0;
    friend bool operator==(const FleetCounts&, const FleetCounts&) = default;
};

struct FleetView {
    /// Sorted most-urgent-first: Missing, Offline, Unexpected, Online; then by host id.
    std::vector<FleetEntry> entries;
    FleetCounts counts;
    friend bool operator==(const FleetView&, const FleetView&) = default;
};

struct ReconcileOptions {
    /// Matches the DDS Liveliness lease on the ClientAnnounce topic.
    std::chrono::milliseconds liveliness_lease = std::chrono::seconds{10};
    std::uint64_t current_revision = 0;
};

/// Pure: no I/O and no clock — `now` is supplied by the caller.
[[nodiscard]] FleetView reconcile(const std::vector<ExpectedHost>& expected,
                                  const std::vector<DiscoveredClient>& discovered, TimePoint now,
                                  ReconcileOptions options);

[[nodiscard]] std::string to_string(HostState state);

}  // namespace lm::core
```

- [ ] **Step 4: Write `libs/core/src/fleet.cpp`**

```cpp
#include "lm/core/fleet.hpp"

#include <algorithm>
#include <map>

namespace lm::core {
namespace {

/// Lower sorts first. Mirrors the declaration order of HostState.
int urgency(HostState state) {
    switch (state) {
        case HostState::Missing:    return 0;
        case HostState::Offline:    return 1;
        case HostState::Unexpected: return 2;
        case HostState::Online:     return 3;
    }
    return 4;
}

}  // namespace

FleetView reconcile(const std::vector<ExpectedHost>& expected,
                    const std::vector<DiscoveredClient>& discovered, TimePoint now,
                    ReconcileOptions options) {
    // Collapse duplicate sightings, keeping the most recent per host.
    std::map<HostId, DiscoveredClient> latest;
    for (const DiscoveredClient& client : discovered) {
        const auto existing = latest.find(client.host_id);
        if (existing == latest.end() || client.last_seen > existing->second.last_seen) {
            latest.insert_or_assign(client.host_id, client);
        }
    }

    const auto within_lease = [&](TimePoint last_seen) {
        if (last_seen >= now) {
            return true;  // clock skew from the client — treat as current
        }
        return (now - last_seen) <= options.liveliness_lease;
    };

    FleetView view;
    std::vector<HostId> accounted;

    for (const ExpectedHost& host : expected) {
        FleetEntry entry;
        entry.host_id = host.host_id;
        entry.address = host.address;

        const auto seen = latest.find(host.host_id);
        if (seen == latest.end()) {
            entry.state = HostState::Missing;
        } else {
            entry.last_seen = seen->second.last_seen;
            entry.caps = seen->second.caps;
            entry.stale = seen->second.applied_revision != options.current_revision;
            entry.state = within_lease(seen->second.last_seen) ? HostState::Online
                                                               : HostState::Offline;
            accounted.push_back(host.host_id);
        }
        view.entries.push_back(std::move(entry));
    }

    for (const auto& [host_id, client] : latest) {
        if (std::find(accounted.begin(), accounted.end(), host_id) != accounted.end()) {
            continue;
        }
        FleetEntry entry;
        entry.host_id = host_id;
        entry.state = HostState::Unexpected;
        entry.last_seen = client.last_seen;
        entry.caps = client.caps;
        entry.stale = client.applied_revision != options.current_revision;
        view.entries.push_back(std::move(entry));
    }

    std::sort(view.entries.begin(), view.entries.end(),
              [](const FleetEntry& a, const FleetEntry& b) {
                  const int ua = urgency(a.state);
                  const int ub = urgency(b.state);
                  return ua != ub ? ua < ub : a.host_id < b.host_id;
              });

    for (const FleetEntry& entry : view.entries) {
        switch (entry.state) {
            case HostState::Online:     ++view.counts.online; break;
            case HostState::Offline:    ++view.counts.offline; break;
            case HostState::Missing:    ++view.counts.missing; break;
            case HostState::Unexpected: ++view.counts.unexpected; break;
        }
        if (entry.stale) {
            ++view.counts.stale;
        }
    }

    return view;
}

std::string to_string(HostState state) {
    switch (state) {
        case HostState::Online:     return "Online";
        case HostState::Offline:    return "Offline";
        case HostState::Missing:    return "Missing";
        case HostState::Unexpected: return "Unexpected";
    }
    return "Unknown";
}

}  // namespace lm::core
```

- [ ] **Step 5: Update `libs/core/CMakeLists.txt`**

Add `src/fleet.cpp` to the library sources and `tests/test_fleet.cpp` to the
`lm_add_test` source list.

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass. `lm_core` is now feature-complete and every critical
function is covered without mocks, DDS, Qt or syscalls.

- [ ] **Step 7: Commit**

```bash
git add libs/core
git commit -m "feat: add fleet reconciliation state machine to lm_core"
```

---

## Task 7: `lm_platform` probe interfaces, fakes and `HostProbes`

**Files:**
- Create: `libs/platform/CMakeLists.txt`
- Create: `libs/platform/include/lm/platform/probes.hpp`, `libs/platform/include/lm/platform/fakes.hpp`
- Create: `libs/platform/src/host_probes.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(libs/platform)`)
- Test: `libs/platform/tests/test_host_probes.cpp`

**Interfaces:**
- Consumes: `HostFacts`, `TemplateBundle`, `rules_for()`, `registry_key()`, `Capabilities` (Tasks 1–3).
- Produces: `IResourceProbe`, `IProcessProbe`, `IServiceProbe`, `IRegistryProbe`, `ProbeSet`, `HostProbes` with `collect(const TemplateBundle&)`, `sample_resources()`, `capabilities()`, `host_id()`; `local_host_name()`; `make_platform_probes()`; and the fakes `FakeResourceProbe`, `FakeProcessProbe`, `FakeServiceProbe`, `FakeRegistryProbe`.

> **`fakes.hpp` ships in the library's public headers**, not in its test directory, so
> `lm_transport` and the applications can build against deterministic probes in their
> own tests and in `--offline` mode.

**The behaviour worth testing here is laziness.** `collect()` must only enumerate
processes when the host actually has a process rule, and must only read the specific
registry values referenced by its rules. Enumerating every service on every tick when
no rule needs it is the difference between a client that idles quietly and one that
shows up in Task Manager.

- [ ] **Step 1: Write the failing test `libs/platform/tests/test_host_probes.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/platform/fakes.hpp"
#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

Rule process_rule(RuleId id, std::string exe) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

Rule registry_rule(RuleId id, std::string key_path, std::string value_name) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = RegistryRule{RegistryHive::LocalMachine, std::move(key_path),
                                std::move(value_name), RegistryMatch::Exists, ""};
    return rule;
}

Rule service_rule(RuleId id, std::string name) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = ServiceRule{std::move(name), std::nullopt};
    return rule;
}

/// Builds a HostProbes wired to fakes, handing back raw pointers so the test can
/// inspect call counts afterwards.
struct Harness {
    FakeResourceProbe* resources = nullptr;
    FakeProcessProbe* processes = nullptr;
    FakeServiceProbe* services = nullptr;
    FakeRegistryProbe* registry = nullptr;
    std::unique_ptr<HostProbes> probes;

    explicit Harness(Capabilities caps = Capabilities{}
                                             .add(Capability::Resources)
                                             .add(Capability::Processes)
                                             .add(Capability::Services)
                                             .add(Capability::Registry)) {
        auto resource_probe = std::make_unique<FakeResourceProbe>();
        auto process_probe = std::make_unique<FakeProcessProbe>();
        auto service_probe = std::make_unique<FakeServiceProbe>();
        auto registry_probe = std::make_unique<FakeRegistryProbe>();

        resources = resource_probe.get();
        processes = process_probe.get();
        services = service_probe.get();
        registry = registry_probe.get();

        ProbeSet set;
        set.resources = std::move(resource_probe);
        set.processes = std::move(process_probe);
        set.services = std::move(service_probe);
        set.registry = std::move(registry_probe);

        probes = std::make_unique<HostProbes>("PC-001", std::move(set), caps);
    }
};

TemplateBundle bundle_for_pc001(std::vector<Rule> rules) {
    TemplateBundle bundle;
    bundle.revision = 1;
    Template tmpl;
    tmpl.name = "Lab Workstation";
    tmpl.rules = std::move(rules);
    bundle.templates = {tmpl};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    return bundle;
}

}  // namespace

TEST(HostProbes, AlwaysSamplesResources) {
    Harness harness;
    harness.resources->next.cpu_percent = 42.0;

    const HostFacts facts = harness.probes->collect(TemplateBundle{});

    EXPECT_EQ(facts.host_id, "PC-001");
    EXPECT_DOUBLE_EQ(facts.resources.cpu_percent, 42.0);
    EXPECT_EQ(harness.resources->calls, 1);
}

TEST(HostProbes, EmptyBundleProbesNothingElse) {
    Harness harness;
    harness.probes->collect(TemplateBundle{});

    EXPECT_EQ(harness.processes->calls, 0);
    EXPECT_EQ(harness.services->calls, 0);
    EXPECT_TRUE(harness.registry->reads.empty());
}

TEST(HostProbes, EnumeratesProcessesOnlyWhenAProcessRuleExists) {
    Harness harness;
    harness.probes->collect(bundle_for_pc001({service_rule("s1", "spooler")}));
    EXPECT_EQ(harness.processes->calls, 0);

    harness.probes->collect(bundle_for_pc001({process_rule("p1", "a.exe")}));
    EXPECT_EQ(harness.processes->calls, 1);
    EXPECT_EQ(harness.services->calls, 1);  // one from the first collect
}

TEST(HostProbes, EnumeratesServicesOnlyWhenAServiceRuleExists) {
    Harness harness;
    harness.probes->collect(bundle_for_pc001({process_rule("p1", "a.exe")}));
    EXPECT_EQ(harness.services->calls, 0);
}

TEST(HostProbes, IgnoresRulesAssignedToOtherHosts) {
    Harness harness;
    TemplateBundle bundle = bundle_for_pc001({process_rule("p1", "a.exe")});
    bundle.assignments.clear();
    bundle.assignments["PC-999"] = {"Lab Workstation"};

    harness.probes->collect(bundle);
    EXPECT_EQ(harness.processes->calls, 0);
}

TEST(HostProbes, ReadsOnlyTheReferencedRegistryValues) {
    Harness harness;
    harness.registry->values["HKLM\\SOFTWARE\\Acme\\\\Version"] =
        RegistryValue{true, "4.2", ""};

    const HostFacts facts = harness.probes->collect(
        bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version")}));

    EXPECT_EQ(harness.registry->reads,
              (std::vector<std::string>{"HKLM\\SOFTWARE\\Acme\\\\Version"}));
    ASSERT_TRUE(facts.registry.contains("HKLM\\SOFTWARE\\Acme\\\\Version"));
    EXPECT_EQ(facts.registry.at("HKLM\\SOFTWARE\\Acme\\\\Version").data, "4.2");
}

TEST(HostProbes, ReadsEachRegistryKeyOnlyOnceWhenTwoRulesShareIt) {
    Harness harness;
    harness.probes->collect(bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version"),
                                              registry_rule("g2", "SOFTWARE\\Acme", "Version")}));
    EXPECT_EQ(harness.registry->reads.size(), 1u);
}

TEST(HostProbes, SkipsRegistryEntirelyWithoutTheCapability) {
    Harness harness{Capabilities{}.add(Capability::Resources).add(Capability::Processes)};
    harness.probes->collect(bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version")}));
    EXPECT_TRUE(harness.registry->reads.empty());
}

TEST(HostProbes, MissingProbeImplementationIsTreatedAsNoCapability) {
    ProbeSet set;
    set.resources = std::make_unique<FakeResourceProbe>();
    // processes, services and registry deliberately left null.
    HostProbes probes{"PC-001", std::move(set), platform_capabilities()};

    EXPECT_FALSE(probes.capabilities().has(Capability::Processes));
    EXPECT_FALSE(probes.capabilities().has(Capability::Registry));
    EXPECT_TRUE(probes.capabilities().has(Capability::Resources));
}

TEST(HostProbes, SampleResourcesDoesNotTouchOtherProbes) {
    Harness harness;
    harness.probes->sample_resources();
    EXPECT_EQ(harness.resources->calls, 1);
    EXPECT_EQ(harness.processes->calls, 0);
}

TEST(LocalHostName, IsNotEmpty) {
    EXPECT_FALSE(local_host_name().empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `lm/platform/probes.hpp` not found.

- [ ] **Step 3: Write `libs/platform/include/lm/platform/probes.hpp`**

```cpp
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::platform {

class IResourceProbe {
public:
    virtual ~IResourceProbe() = default;
    virtual core::ResourceSample sample() = 0;
};

class IProcessProbe {
public:
    virtual ~IProcessProbe() = default;
    virtual std::vector<core::ProcessInfo> enumerate() = 0;
};

class IServiceProbe {
public:
    virtual ~IServiceProbe() = default;
    virtual std::vector<core::ServiceInfo> enumerate() = 0;
};

class IRegistryProbe {
public:
    virtual ~IRegistryProbe() = default;
    virtual core::RegistryValue read(const core::RegistryRule& rule) = 0;
};

/// Null members mean the capability is unavailable on this platform.
struct ProbeSet {
    std::unique_ptr<IResourceProbe> resources;
    std::unique_ptr<IProcessProbe> processes;
    std::unique_ptr<IServiceProbe> services;
    std::unique_ptr<IRegistryProbe> registry;
};

/// Assembles HostFacts snapshots. Probes lazily: only the categories the host's
/// own rules actually reference are queried.
class HostProbes {
public:
    /// Capabilities are intersected with the probes that were actually supplied,
    /// so a null probe can never advertise a capability it cannot serve.
    HostProbes(core::HostId host_id, ProbeSet probes, core::Capabilities caps);

    [[nodiscard]] core::HostFacts collect(const core::TemplateBundle& bundle);

    /// Resource-only sampling for the fast 2 s tick.
    [[nodiscard]] core::ResourceSample sample_resources();

    [[nodiscard]] core::Capabilities capabilities() const { return caps_; }
    [[nodiscard]] const core::HostId& host_id() const { return host_id_; }

private:
    core::HostId host_id_;
    ProbeSet probes_;
    core::Capabilities caps_;
};

/// The machine's hostname, used as the client identifier.
[[nodiscard]] std::string local_host_name();

/// Builds the probe set for the platform this binary was compiled for.
[[nodiscard]] ProbeSet make_platform_probes();

}  // namespace lm::platform
```

- [ ] **Step 4: Write `libs/platform/include/lm/platform/fakes.hpp`**

```cpp
#pragma once

#include <map>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {

class FakeResourceProbe : public IResourceProbe {
public:
    core::ResourceSample next;
    int calls = 0;

    core::ResourceSample sample() override {
        ++calls;
        return next;
    }
};

class FakeProcessProbe : public IProcessProbe {
public:
    std::vector<core::ProcessInfo> next;
    int calls = 0;

    std::vector<core::ProcessInfo> enumerate() override {
        ++calls;
        return next;
    }
};

class FakeServiceProbe : public IServiceProbe {
public:
    std::vector<core::ServiceInfo> next;
    int calls = 0;

    std::vector<core::ServiceInfo> enumerate() override {
        ++calls;
        return next;
    }
};

class FakeRegistryProbe : public IRegistryProbe {
public:
    /// Keyed by core::registry_key(). Unlisted keys read back as absent.
    std::map<std::string, core::RegistryValue> values;
    /// Every key this probe was asked for, in order.
    std::vector<std::string> reads;

    core::RegistryValue read(const core::RegistryRule& rule) override {
        const std::string key = core::registry_key(rule);
        reads.push_back(key);
        const auto found = values.find(key);
        return found == values.end() ? core::RegistryValue{} : found->second;
    }
};

}  // namespace lm::platform
```

- [ ] **Step 5: Write `libs/platform/src/host_probes.cpp`**

```cpp
#include "lm/platform/probes.hpp"

#include <set>

namespace lm::platform {
namespace {

core::Capabilities intersect(core::Capabilities declared, const ProbeSet& probes) {
    core::Capabilities result;
    if (probes.resources && declared.has(core::Capability::Resources)) {
        result.add(core::Capability::Resources);
    }
    if (probes.processes && declared.has(core::Capability::Processes)) {
        result.add(core::Capability::Processes);
    }
    if (probes.services && declared.has(core::Capability::Services)) {
        result.add(core::Capability::Services);
    }
    if (probes.registry && declared.has(core::Capability::Registry)) {
        result.add(core::Capability::Registry);
    }
    return result;
}

}  // namespace

HostProbes::HostProbes(core::HostId host_id, ProbeSet probes, core::Capabilities caps)
    : host_id_(std::move(host_id)), probes_(std::move(probes)), caps_(intersect(caps, probes_)) {}

core::ResourceSample HostProbes::sample_resources() {
    return probes_.resources ? probes_.resources->sample() : core::ResourceSample{};
}

core::HostFacts HostProbes::collect(const core::TemplateBundle& bundle) {
    core::HostFacts facts;
    facts.host_id = host_id_;
    facts.resources = sample_resources();

    const std::vector<const core::Rule*> rules = core::rules_for(bundle, host_id_);

    bool needs_processes = false;
    bool needs_services = false;
    std::set<std::string> registry_keys;
    std::vector<const core::RegistryRule*> registry_rules;

    for (const core::Rule* rule : rules) {
        switch (core::kind_of(*rule)) {
            case core::RuleKind::Process:
                needs_processes = true;
                break;
            case core::RuleKind::Service:
                needs_services = true;
                break;
            case core::RuleKind::Registry: {
                const auto& payload = std::get<core::RegistryRule>(rule->payload);
                if (registry_keys.insert(core::registry_key(payload)).second) {
                    registry_rules.push_back(&payload);
                }
                break;
            }
        }
    }

    if (needs_processes && caps_.has(core::Capability::Processes)) {
        facts.processes = probes_.processes->enumerate();
    }
    if (needs_services && caps_.has(core::Capability::Services)) {
        facts.services = probes_.services->enumerate();
    }
    if (caps_.has(core::Capability::Registry)) {
        for (const core::RegistryRule* rule : registry_rules) {
            facts.registry.emplace(core::registry_key(*rule), probes_.registry->read(*rule));
        }
    }

    return facts;
}

}  // namespace lm::platform
```

- [ ] **Step 6: Write `libs/platform/CMakeLists.txt`**

`local_host_name()` and `make_platform_probes()` are implemented per-OS in Task 8. Add
placeholder source files now so the library links: `src/windows/platform_windows.cpp`
and `src/linux/platform_linux.cpp`, each defining both functions with
`make_platform_probes()` returning an empty `ProbeSet` for the moment.

```cmake
add_library(lm_platform STATIC
  src/host_probes.cpp)
add_library(lm::platform ALIAS lm_platform)

if(WIN32)
  target_sources(lm_platform PRIVATE src/windows/platform_windows.cpp)
else()
  target_sources(lm_platform PRIVATE src/linux/platform_linux.cpp)
endif()

target_include_directories(lm_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_platform PUBLIC lm_core PRIVATE lm_warnings)

lm_add_test(lm_platform_tests SOURCES tests/test_host_probes.cpp LINK lm_platform)
```

Add `add_subdirectory(libs/platform)` to the top-level `CMakeLists.txt`.

- [ ] **Step 7: Write the minimal per-OS stubs**

`src/windows/platform_windows.cpp`:

```cpp
#include <windows.h>

#include "lm/platform/probes.hpp"

namespace lm::platform {

std::string local_host_name() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size) == 0) {
        return "unknown-host";
    }
    return std::string(buffer, size);
}

ProbeSet make_platform_probes() {
    return ProbeSet{};  // Task 8 fills this in.
}

}  // namespace lm::platform
```

`src/linux/platform_linux.cpp`:

```cpp
#include <unistd.h>

#include "lm/platform/probes.hpp"

namespace lm::platform {

std::string local_host_name() {
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "unknown-host";
    }
    return std::string(buffer);
}

ProbeSet make_platform_probes() {
    return ProbeSet{};  // Task 8 fills this in.
}

}  // namespace lm::platform
```

- [ ] **Step 8: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add libs/platform CMakeLists.txt
git commit -m "feat: add lm_platform probe interfaces, fakes and lazy HostProbes"
```

---

## Task 8: Cross-platform resource probes

**Files:**
- Create: `libs/platform/src/windows/resource_probe_windows.cpp`
- Create: `libs/platform/src/linux/resource_probe_linux.cpp`
- Modify: `libs/platform/src/windows/platform_windows.cpp`, `libs/platform/src/linux/platform_linux.cpp`
- Modify: `libs/platform/CMakeLists.txt`
- Test: `libs/platform/tests/test_resource_probe.cpp`

**Interfaces:**
- Consumes: `IResourceProbe`, `ProbeSet`, `core::ResourceSample`, `core::DiskUsage` (Task 7).
- Produces: `make_resource_probe() -> std::unique_ptr<IResourceProbe>` (one declaration, two implementations), and a `make_platform_probes()` that returns a populated `resources` member.

> **CPU load is a delta, not an instant.** Both implementations compare the current
> counters against the previous call, so the probe is stateful and **the first call
> always reports 0 %**. The client's 2 s timer makes this invisible after the first
> tick. The tests assert this explicitly so nobody "fixes" it later by sleeping inside
> `sample()`.

Windows uses `GetSystemTimes` rather than PDH — no extra library, no counter-path
strings, and it is the same data PDH reports for total CPU.

- [ ] **Step 1: Write the failing test `libs/platform/tests/test_resource_probe.cpp`**

```cpp
#include <gtest/gtest.h>

#include <cmath>

#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

TEST(ResourceProbe, FirstSampleReportsZeroCpuByDesign) {
    const auto probe = make_resource_probe();
    ASSERT_NE(probe, nullptr);
    EXPECT_DOUBLE_EQ(probe->sample().cpu_percent, 0.0);
}

TEST(ResourceProbe, CpuStaysWithinRangeAcrossSamples) {
    const auto probe = make_resource_probe();
    probe->sample();

    // Burn a little CPU so the second delta is non-degenerate.
    volatile double sink = 0.0;
    for (int i = 1; i < 4'000'000; ++i) {
        sink += std::sqrt(static_cast<double>(i));
    }

    const ResourceSample sample = probe->sample();
    EXPECT_GE(sample.cpu_percent, 0.0);
    EXPECT_LE(sample.cpu_percent, 100.0);
    EXPECT_FALSE(std::isnan(sample.cpu_percent));
}

TEST(ResourceProbe, ReportsPlausibleMemory) {
    const auto probe = make_resource_probe();
    const ResourceSample sample = probe->sample();

    EXPECT_GT(sample.mem_total_bytes, 0u);
    EXPECT_LE(sample.mem_used_bytes, sample.mem_total_bytes);
    // Any machine running this test has at least 256 MB.
    EXPECT_GT(sample.mem_total_bytes, 256ull * 1024 * 1024);
}

TEST(ResourceProbe, ReportsAtLeastOneVolume) {
    const auto probe = make_resource_probe();
    const ResourceSample sample = probe->sample();

    ASSERT_FALSE(sample.disks.empty());
    for (const DiskUsage& disk : sample.disks) {
        EXPECT_FALSE(disk.mount.empty());
        EXPECT_GT(disk.total_bytes, 0u);
        EXPECT_LE(disk.free_bytes, disk.total_bytes);
        EXPECT_GE(disk.used_percent(), 0.0);
        EXPECT_LE(disk.used_percent(), 100.0);
    }
}

TEST(DiskUsage, UsedPercentHandlesZeroTotalWithoutDividingByZero) {
    const DiskUsage empty{"/none", 0, 0};
    EXPECT_DOUBLE_EQ(empty.used_percent(), 0.0);
}

TEST(PlatformProbes, ProvideResourcesAndMatchDeclaredCapabilities) {
    ProbeSet probes = make_platform_probes();
    ASSERT_NE(probes.resources, nullptr);

    HostProbes host{local_host_name(), std::move(probes), platform_capabilities()};
    EXPECT_TRUE(host.capabilities().has(Capability::Resources));
    EXPECT_FALSE(host.host_id().empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `make_resource_probe` is not declared.

- [ ] **Step 3: Declare `make_resource_probe()` in `libs/platform/include/lm/platform/probes.hpp`**

Add next to `make_platform_probes()`:

```cpp
/// Builds the resource probe for this platform. Stateful: CPU load is computed
/// as a delta against the previous call, so the first sample reports 0 %.
[[nodiscard]] std::unique_ptr<IResourceProbe> make_resource_probe();
```

- [ ] **Step 4: Write `libs/platform/src/windows/resource_probe_windows.cpp`**

```cpp
#include <windows.h>

#include <memory>

#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

std::uint64_t to_uint64(const FILETIME& value) {
    ULARGE_INTEGER converted;
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

class WindowsResourceProbe : public IResourceProbe {
public:
    core::ResourceSample sample() override {
        core::ResourceSample result;
        result.cpu_percent = sample_cpu();
        sample_memory(result);
        result.disks = sample_disks();
        return result;
    }

private:
    double sample_cpu() {
        FILETIME idle_time{};
        FILETIME kernel_time{};
        FILETIME user_time{};
        if (GetSystemTimes(&idle_time, &kernel_time, &user_time) == 0) {
            return 0.0;
        }

        const std::uint64_t idle = to_uint64(idle_time);
        // Kernel time already includes idle time.
        const std::uint64_t total = to_uint64(kernel_time) + to_uint64(user_time);

        if (!primed_) {
            primed_ = true;
            previous_idle_ = idle;
            previous_total_ = total;
            return 0.0;
        }

        const std::uint64_t idle_delta = idle - previous_idle_;
        const std::uint64_t total_delta = total - previous_total_;
        previous_idle_ = idle;
        previous_total_ = total;

        if (total_delta == 0) {
            return 0.0;
        }

        const double busy = static_cast<double>(total_delta - idle_delta);
        const double percent = 100.0 * busy / static_cast<double>(total_delta);
        return percent < 0.0 ? 0.0 : (percent > 100.0 ? 100.0 : percent);
    }

    static void sample_memory(core::ResourceSample& result) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) == 0) {
            return;
        }
        result.mem_total_bytes = status.ullTotalPhys;
        result.mem_used_bytes = status.ullTotalPhys - status.ullAvailPhys;
    }

    static std::vector<core::DiskUsage> sample_disks() {
        std::vector<core::DiskUsage> disks;

        wchar_t buffer[512] = {};
        const DWORD length = GetLogicalDriveStringsW(
            static_cast<DWORD>(std::size(buffer)) - 1, buffer);
        if (length == 0) {
            return disks;
        }

        for (const wchar_t* drive = buffer; *drive != L'\0'; drive += wcslen(drive) + 1) {
            if (GetDriveTypeW(drive) != DRIVE_FIXED) {
                continue;
            }

            ULARGE_INTEGER free_to_caller{};
            ULARGE_INTEGER total{};
            ULARGE_INTEGER total_free{};
            if (GetDiskFreeSpaceExW(drive, &free_to_caller, &total, &total_free) == 0) {
                continue;
            }

            core::DiskUsage usage;
            // Drive strings are ASCII-safe ("C:\"), so a narrowing copy is sound.
            for (const wchar_t* c = drive; *c != L'\0'; ++c) {
                usage.mount.push_back(static_cast<char>(*c));
            }
            usage.total_bytes = total.QuadPart;
            usage.free_bytes = total_free.QuadPart;
            disks.push_back(std::move(usage));
        }

        return disks;
    }

    bool primed_ = false;
    std::uint64_t previous_idle_ = 0;
    std::uint64_t previous_total_ = 0;
};

}  // namespace

std::unique_ptr<IResourceProbe> make_resource_probe() {
    return std::make_unique<WindowsResourceProbe>();
}

}  // namespace lm::platform
```

- [ ] **Step 5: Write `libs/platform/src/linux/resource_probe_linux.cpp`**

```cpp
#include <sys/statvfs.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

/// Filesystems that are not real storage and would otherwise clutter the view.
bool is_pseudo_filesystem(const std::string& type) {
    static const std::vector<std::string> kPseudo = {
        "proc",   "sysfs",     "devtmpfs", "devpts", "tmpfs",   "cgroup",  "cgroup2",
        "pstore", "securityfs", "debugfs",  "tracefs", "mqueue", "hugetlbfs", "overlay",
        "squashfs", "autofs",  "binfmt_misc", "configfs", "fusectl", "bpf", "ramfs"};
    for (const std::string& pseudo : kPseudo) {
        if (type == pseudo) {
            return true;
        }
    }
    return false;
}

class LinuxResourceProbe : public IResourceProbe {
public:
    core::ResourceSample sample() override {
        core::ResourceSample result;
        result.cpu_percent = sample_cpu();
        sample_memory(result);
        result.disks = sample_disks();
        return result;
    }

private:
    double sample_cpu() {
        std::ifstream stat("/proc/stat");
        if (!stat) {
            return 0.0;
        }

        std::string line;
        if (!std::getline(stat, line) || line.rfind("cpu ", 0) != 0) {
            return 0.0;
        }

        std::istringstream fields(line.substr(4));
        std::uint64_t value = 0;
        std::uint64_t total = 0;
        std::uint64_t idle = 0;
        for (int index = 0; fields >> value; ++index) {
            total += value;
            // Fields 3 and 4 are idle and iowait.
            if (index == 3 || index == 4) {
                idle += value;
            }
        }

        if (!primed_) {
            primed_ = true;
            previous_idle_ = idle;
            previous_total_ = total;
            return 0.0;
        }

        const std::uint64_t idle_delta = idle - previous_idle_;
        const std::uint64_t total_delta = total - previous_total_;
        previous_idle_ = idle;
        previous_total_ = total;

        if (total_delta == 0) {
            return 0.0;
        }

        const double busy = static_cast<double>(total_delta - idle_delta);
        const double percent = 100.0 * busy / static_cast<double>(total_delta);
        return percent < 0.0 ? 0.0 : (percent > 100.0 ? 100.0 : percent);
    }

    static void sample_memory(core::ResourceSample& result) {
        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo) {
            return;
        }

        std::uint64_t total_kb = 0;
        std::uint64_t available_kb = 0;
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        while (meminfo >> key >> value >> unit) {
            if (key == "MemTotal:") {
                total_kb = value;
            } else if (key == "MemAvailable:") {
                available_kb = value;
                break;
            }
        }

        result.mem_total_bytes = total_kb * 1024;
        result.mem_used_bytes = (total_kb - available_kb) * 1024;
    }

    static std::vector<core::DiskUsage> sample_disks() {
        std::vector<core::DiskUsage> disks;

        std::ifstream mounts("/proc/mounts");
        if (!mounts) {
            return disks;
        }

        std::string device;
        std::string mount_point;
        std::string type;
        std::string remainder;
        while (mounts >> device >> mount_point >> type) {
            std::getline(mounts, remainder);
            if (is_pseudo_filesystem(type)) {
                continue;
            }

            struct statvfs stats {};
            if (statvfs(mount_point.c_str(), &stats) != 0 || stats.f_blocks == 0) {
                continue;
            }

            core::DiskUsage usage;
            usage.mount = mount_point;
            usage.total_bytes = static_cast<std::uint64_t>(stats.f_blocks) * stats.f_frsize;
            usage.free_bytes = static_cast<std::uint64_t>(stats.f_bavail) * stats.f_frsize;
            disks.push_back(std::move(usage));
        }

        return disks;
    }

    bool primed_ = false;
    std::uint64_t previous_idle_ = 0;
    std::uint64_t previous_total_ = 0;
};

}  // namespace

std::unique_ptr<IResourceProbe> make_resource_probe() {
    return std::make_unique<LinuxResourceProbe>();
}

}  // namespace lm::platform
```

- [ ] **Step 6: Populate `make_platform_probes()` in both per-OS files**

Replace the stub body in `platform_windows.cpp` and `platform_linux.cpp` with:

```cpp
ProbeSet make_platform_probes() {
    ProbeSet probes;
    probes.resources = make_resource_probe();
    // processes, services and registry arrive in a later iteration; HostProbes
    // intersects capabilities with the probes actually supplied, so the client
    // honestly advertises resources only.
    return probes;
}
```

- [ ] **Step 7: Update `libs/platform/CMakeLists.txt`**

```cmake
if(WIN32)
  target_sources(lm_platform PRIVATE
    src/windows/platform_windows.cpp
    src/windows/resource_probe_windows.cpp)
else()
  target_sources(lm_platform PRIVATE
    src/linux/platform_linux.cpp
    src/linux/resource_probe_linux.cpp)
endif()

lm_add_test(lm_platform_tests
  SOURCES tests/test_host_probes.cpp tests/test_resource_probe.cpp
  LINK lm_platform)
```

- [ ] **Step 8: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass, including the sanity checks against the real machine.

- [ ] **Step 9: Commit**

```bash
git add libs/platform
git commit -m "feat: add Windows and Linux resource probes"
```

---

## Task 9: Transport interfaces and `InMemoryTransport`

**Files:**
- Create: `libs/transport/CMakeLists.txt`
- Create: `libs/transport/include/lm/transport/messages.hpp`, `libs/transport/include/lm/transport/transport.hpp`
- Create: `libs/transport/include/lm/transport/in_memory_transport.hpp`, `libs/transport/src/in_memory_transport.cpp`
- Modify: `CMakeLists.txt`
- Test: `libs/transport/tests/test_in_memory_transport.cpp`

**Interfaces:**
- Consumes: `core::HostId`, `core::ResourceSample`, `core::ComplianceReport`, `serialise_bundle`/`parse_bundle` (Tasks 1–5).
- Produces: `ClientAnnounce`, `ResourceSampleMessage`, `TemplateBundleMessage`, `ComplianceReportMessage`, `ConnectionState`; `IClientTransport`, `IServerTransport`; `MessageBus`, `make_in_memory_client(MessageBus&)`, `make_in_memory_server(MessageBus&)`.

> **Two interfaces, not one.** `IClientTransport` and `IServerTransport` are separate so
> each application only sees the operations its role can perform. A client physically
> cannot publish a `TemplateBundle`.

> **Bundles and reports travel as JSON strings inside a small envelope.** `ResourceSample`
> and `ClientAnnounce` are flat and frequent, so they get hand-written CDR in Task 10.
> `TemplateBundle` and `ComplianceReport` are structurally deep — variants, optionals,
> nested vectors — and travel at 30 s or on-change. Reusing `serialise_bundle()` for
> those removes roughly two thirds of the hand-written codec work and its associated
> risk, at a cost of a few hundred bytes per infrequent message.

- [ ] **Step 1: Write the failing test `libs/transport/tests/test_in_memory_transport.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/core/json.hpp"  // serialise_bundle, parse_bundle, content_hash
#include "lm/transport/in_memory_transport.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

ClientAnnounce announce(HostId id) {
    ClientAnnounce message;
    message.host_id = std::move(id);
    message.agent_version = "0.1.0";
    message.capabilities = platform_capabilities().raw();
    return message;
}

TemplateBundleMessage bundle_message(std::uint64_t revision) {
    TemplateBundle bundle;
    bundle.revision = revision;
    bundle.baseline.name = "baseline";

    TemplateBundleMessage message;
    message.revision = revision;
    message.hash = content_hash(bundle);
    message.json = serialise_bundle(bundle);
    return message;
}

}  // namespace

TEST(InMemoryTransport, ServerReceivesClientAnnouncements) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<HostId> seen;
    server->on_announce([&](const ClientAnnounce& message) { seen.push_back(message.host_id); });

    client->publish_announce(announce("PC-001"));
    EXPECT_EQ(seen, (std::vector<HostId>{"PC-001"}));
}

TEST(InMemoryTransport, ServerReceivesResourceSamples) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<ResourceSampleMessage> received;
    server->on_resources([&](const ResourceSampleMessage& message) { received = message; });

    ResourceSampleMessage sample;
    sample.host_id = "PC-001";
    sample.sample.cpu_percent = 37.5;
    sample.sample.mem_total_bytes = 8ull * 1024 * 1024 * 1024;
    sample.sample.disks.push_back(DiskUsage{"C:\\", 500'000'000'000ull, 120'000'000'000ull});
    client->publish_resources(sample);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->host_id, "PC-001");
    EXPECT_DOUBLE_EQ(received->sample.cpu_percent, 37.5);
    EXPECT_EQ(received->sample.disks.size(), 1u);
}

TEST(InMemoryTransport, ClientReceivesPublishedBundles) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    server->publish_bundle(bundle_message(4));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->revision, 4u);
    const auto parsed = parse_bundle(received->json);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->revision, 4u);
}

TEST(InMemoryTransport, LateJoiningClientGetsTheCurrentBundle) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    server->publish_bundle(bundle_message(7));

    // Client attaches only after the bundle was published — this models the
    // TransientLocal durability the DDS transport relies on.
    const auto client = make_in_memory_client(bus);

    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->revision, 7u);
}

TEST(InMemoryTransport, ServerReceivesComplianceReports) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<ComplianceReportMessage> received;
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    ComplianceReportMessage message;
    message.report.host_id = "PC-001";
    message.report.applied_revision = 4;
    message.report.results.push_back(CheckResult{"r1", CheckStatus::Pass, "running", ""});
    client->publish_report(message);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->report.host_id, "PC-001");
    EXPECT_EQ(received->report.applied_revision, 4u);
    ASSERT_EQ(received->report.results.size(), 1u);
    EXPECT_EQ(received->report.results.front().status, CheckStatus::Pass);
}

TEST(InMemoryTransport, MultipleClientsAllReachTheServer) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto first = make_in_memory_client(bus);
    const auto second = make_in_memory_client(bus);

    std::vector<HostId> seen;
    server->on_announce([&](const ClientAnnounce& message) { seen.push_back(message.host_id); });

    first->publish_announce(announce("PC-001"));
    second->publish_announce(announce("PC-002"));

    EXPECT_EQ(seen, (std::vector<HostId>{"PC-001", "PC-002"}));
}

TEST(InMemoryTransport, EveryClientReceivesTheSameBundle) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto first = make_in_memory_client(bus);
    const auto second = make_in_memory_client(bus);

    int deliveries = 0;
    first->on_bundle([&](const TemplateBundleMessage&) { ++deliveries; });
    second->on_bundle([&](const TemplateBundleMessage&) { ++deliveries; });

    server->publish_bundle(bundle_message(1));
    EXPECT_EQ(deliveries, 2);
}

TEST(InMemoryTransport, ReportsConnectedImmediately) {
    MessageBus bus;
    const auto client = make_in_memory_client(bus);
    EXPECT_EQ(client->state(), ConnectionState::Connected);
}

TEST(InMemoryTransport, PublishingWithNoSubscriberIsHarmless) {
    MessageBus bus;
    const auto client = make_in_memory_client(bus);
    EXPECT_NO_THROW(client->publish_announce(announce("PC-001")));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `lm/transport/in_memory_transport.hpp` not found.

- [ ] **Step 3: Write `libs/transport/include/lm/transport/messages.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"

namespace lm::transport {

/// Topic: ClientAnnounce. Reliable, TransientLocal, keyed by host_id.
struct ClientAnnounce {
    core::HostId host_id;
    std::string agent_version;
    /// core::Capabilities::raw()
    std::uint32_t capabilities = 0;
    friend bool operator==(const ClientAnnounce&, const ClientAnnounce&) = default;
};

/// Topic: ResourceSample. BestEffort, Volatile, KeepLast(1), keyed by host_id.
struct ResourceSampleMessage {
    core::HostId host_id;
    core::ResourceSample sample;
    friend bool operator==(const ResourceSampleMessage&, const ResourceSampleMessage&) = default;
};

/// Topic: TemplateBundle. Reliable, TransientLocal, KeepLast(1).
/// The bundle travels as JSON; revision and hash are duplicated in the envelope
/// so a receiver can discard a message it already has without parsing it.
struct TemplateBundleMessage {
    std::uint64_t revision = 0;
    std::string hash;
    std::string json;
    friend bool operator==(const TemplateBundleMessage&, const TemplateBundleMessage&) = default;
};

/// Topic: ComplianceReport. Reliable, TransientLocal, keyed by host_id.
struct ComplianceReportMessage {
    core::ComplianceReport report;
    friend bool operator==(const ComplianceReportMessage&, const ComplianceReportMessage&) = default;
};

enum class ConnectionState { Disconnected, Connected };

}  // namespace lm::transport
```

- [ ] **Step 4: Write `libs/transport/include/lm/transport/transport.hpp`**

```cpp
#pragma once

#include <functional>

#include "lm/transport/messages.hpp"

namespace lm::transport {

/// The client half: publishes its own state, receives templates.
class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    virtual void publish_announce(const ClientAnnounce& message) = 0;
    virtual void publish_resources(const ResourceSampleMessage& message) = 0;
    virtual void publish_report(const ComplianceReportMessage& message) = 0;

    /// Invoked immediately with the current bundle if one has already been
    /// published, mirroring TransientLocal durability.
    virtual void on_bundle(std::function<void(const TemplateBundleMessage&)> handler) = 0;
    virtual void on_connection_changed(std::function<void(ConnectionState)> handler) = 0;

    [[nodiscard]] virtual ConnectionState state() const = 0;
};

/// The server half: publishes templates, receives client state.
class IServerTransport {
public:
    virtual ~IServerTransport() = default;

    virtual void publish_bundle(const TemplateBundleMessage& message) = 0;

    virtual void on_announce(std::function<void(const ClientAnnounce&)> handler) = 0;
    virtual void on_resources(std::function<void(const ResourceSampleMessage&)> handler) = 0;
    virtual void on_report(std::function<void(const ComplianceReportMessage&)> handler) = 0;
    /// Fired when DDS liveliness reports a client has gone silent.
    virtual void on_client_lost(std::function<void(const core::HostId&)> handler) = 0;

    [[nodiscard]] virtual ConnectionState state() const = 0;
};

}  // namespace lm::transport
```

- [ ] **Step 5: Write `libs/transport/include/lm/transport/in_memory_transport.hpp`**

```cpp
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "lm/transport/transport.hpp"

namespace lm::transport {

/// An in-process stand-in for a DDS domain. Delivery is synchronous, so tests
/// need no waiting or polling. Retains the last published bundle so clients
/// attaching afterwards still receive it.
class MessageBus {
public:
    void publish_announce(const ClientAnnounce& message);
    void publish_resources(const ResourceSampleMessage& message);
    void publish_report(const ComplianceReportMessage& message);
    void publish_bundle(const TemplateBundleMessage& message);

    void subscribe_announce(std::function<void(const ClientAnnounce&)> handler);
    void subscribe_resources(std::function<void(const ResourceSampleMessage&)> handler);
    void subscribe_report(std::function<void(const ComplianceReportMessage&)> handler);
    /// Immediately replays the retained bundle, if any.
    void subscribe_bundle(std::function<void(const TemplateBundleMessage&)> handler);

private:
    std::vector<std::function<void(const ClientAnnounce&)>> announce_handlers_;
    std::vector<std::function<void(const ResourceSampleMessage&)>> resource_handlers_;
    std::vector<std::function<void(const ComplianceReportMessage&)>> report_handlers_;
    std::vector<std::function<void(const TemplateBundleMessage&)>> bundle_handlers_;
    std::optional<TemplateBundleMessage> retained_bundle_;
};

[[nodiscard]] std::unique_ptr<IClientTransport> make_in_memory_client(MessageBus& bus);
[[nodiscard]] std::unique_ptr<IServerTransport> make_in_memory_server(MessageBus& bus);

}  // namespace lm::transport
```

- [ ] **Step 6: Write `libs/transport/src/in_memory_transport.cpp`**

```cpp
#include "lm/transport/in_memory_transport.hpp"

namespace lm::transport {
namespace {

template <typename Message>
void deliver(const std::vector<std::function<void(const Message&)>>& handlers,
             const Message& message) {
    for (const auto& handler : handlers) {
        if (handler) {
            handler(message);
        }
    }
}

class InMemoryClientTransport : public IClientTransport {
public:
    explicit InMemoryClientTransport(MessageBus& bus) : bus_(bus) {}

    void publish_announce(const ClientAnnounce& message) override { bus_.publish_announce(message); }
    void publish_resources(const ResourceSampleMessage& message) override {
        bus_.publish_resources(message);
    }
    void publish_report(const ComplianceReportMessage& message) override {
        bus_.publish_report(message);
    }

    void on_bundle(std::function<void(const TemplateBundleMessage&)> handler) override {
        bus_.subscribe_bundle(std::move(handler));
    }

    void on_connection_changed(std::function<void(ConnectionState)> handler) override {
        if (handler) {
            handler(ConnectionState::Connected);
        }
    }

    [[nodiscard]] ConnectionState state() const override { return ConnectionState::Connected; }

private:
    MessageBus& bus_;
};

class InMemoryServerTransport : public IServerTransport {
public:
    explicit InMemoryServerTransport(MessageBus& bus) : bus_(bus) {}

    void publish_bundle(const TemplateBundleMessage& message) override {
        bus_.publish_bundle(message);
    }

    void on_announce(std::function<void(const ClientAnnounce&)> handler) override {
        bus_.subscribe_announce(std::move(handler));
    }
    void on_resources(std::function<void(const ResourceSampleMessage&)> handler) override {
        bus_.subscribe_resources(std::move(handler));
    }
    void on_report(std::function<void(const ComplianceReportMessage&)> handler) override {
        bus_.subscribe_report(std::move(handler));
    }
    void on_client_lost(std::function<void(const core::HostId&)>) override {
        // The in-memory bus has no liveliness concept; clients never disappear.
    }

    [[nodiscard]] ConnectionState state() const override { return ConnectionState::Connected; }

private:
    MessageBus& bus_;
};

}  // namespace

void MessageBus::publish_announce(const ClientAnnounce& message) {
    deliver(announce_handlers_, message);
}

void MessageBus::publish_resources(const ResourceSampleMessage& message) {
    deliver(resource_handlers_, message);
}

void MessageBus::publish_report(const ComplianceReportMessage& message) {
    deliver(report_handlers_, message);
}

void MessageBus::publish_bundle(const TemplateBundleMessage& message) {
    retained_bundle_ = message;
    deliver(bundle_handlers_, message);
}

void MessageBus::subscribe_announce(std::function<void(const ClientAnnounce&)> handler) {
    announce_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_resources(std::function<void(const ResourceSampleMessage&)> handler) {
    resource_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_report(std::function<void(const ComplianceReportMessage&)> handler) {
    report_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_bundle(std::function<void(const TemplateBundleMessage&)> handler) {
    if (handler && retained_bundle_) {
        handler(*retained_bundle_);
    }
    bundle_handlers_.push_back(std::move(handler));
}

std::unique_ptr<IClientTransport> make_in_memory_client(MessageBus& bus) {
    return std::make_unique<InMemoryClientTransport>(bus);
}

std::unique_ptr<IServerTransport> make_in_memory_server(MessageBus& bus) {
    return std::make_unique<InMemoryServerTransport>(bus);
}

}  // namespace lm::transport
```

- [ ] **Step 7: Write `libs/transport/CMakeLists.txt`**

```cmake
add_library(lm_transport STATIC
  src/in_memory_transport.cpp)
add_library(lm::transport ALIAS lm_transport)

target_include_directories(lm_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_transport PUBLIC lm_core PRIVATE lm_warnings)

lm_add_test(lm_transport_tests
  SOURCES tests/test_in_memory_transport.cpp
  LINK lm_transport)
```

Add `add_subdirectory(libs/transport)` to the top-level `CMakeLists.txt`.

- [ ] **Step 8: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass. Fast DDS is not linked yet — that arrives in Tasks 10–11.

- [ ] **Step 9: Commit**

```bash
git add libs/transport CMakeLists.txt
git commit -m "feat: add transport interfaces and in-memory implementation"
```

---

## Task 10: Wire codecs

**Files:**
- Create: `libs/transport/include/lm/transport/codec.hpp`, `libs/transport/src/codec.cpp`
- Modify: `libs/transport/CMakeLists.txt`
- Test: `libs/transport/tests/test_codec.cpp`

**Interfaces:**
- Consumes: the four message types from Task 9.
- Produces: for each message type `M`, `std::vector<std::uint8_t> encode(const M&)` and `bool decode(std::span<const std::uint8_t>, M&)`; plus `key_of(const M&) -> std::string` for the three keyed topics.

> **Why this is a separate task from the DDS binding.** Serialisation is pure and can be
> exhaustively round-trip tested with no DDS domain, no network and no Fast DDS types.
> Task 11 then only has to bind these functions to Fast DDS's `TopicDataType`, which
> keeps the part that needs a live domain as small as possible.

> **Codecs are needed for all four topics.** `TemplateBundleMessage` carries JSON but
> still needs an envelope codec (revision, hash, json). `ComplianceReportMessage` is flat
> enough — a host id, a revision, and a vector of four-field results — that it gets a
> direct codec rather than a JSON detour.

- [ ] **Step 1: Write the failing test `libs/transport/tests/test_codec.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/transport/codec.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

template <typename Message>
Message round_trip(const Message& original) {
    const std::vector<std::uint8_t> bytes = encode(original);
    EXPECT_FALSE(bytes.empty());
    Message decoded;
    EXPECT_TRUE(decode(bytes, decoded));
    return decoded;
}

}  // namespace

TEST(Codec, ClientAnnounceRoundTrips) {
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "0.1.0";
    original.capabilities = platform_capabilities().raw();

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ClientAnnounceHandlesEmptyStrings) {
    const ClientAnnounce original;
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ResourceSampleRoundTripsWithMultipleDisks) {
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.cpu_percent = 37.25;
    original.sample.mem_total_bytes = 17'179'869'184ull;
    original.sample.mem_used_bytes = 9'000'000'000ull;
    original.sample.disks = {DiskUsage{"C:\\", 500'107'862'016ull, 123'456'789'012ull},
                             DiskUsage{"D:\\", 2'000'398'934'016ull, 1'500'000'000'000ull}};

    const ResourceSampleMessage decoded = round_trip(original);
    EXPECT_EQ(decoded, original);
    EXPECT_DOUBLE_EQ(decoded.sample.cpu_percent, 37.25);
}

TEST(Codec, ResourceSampleRoundTripsWithNoDisks) {
    ResourceSampleMessage original;
    original.host_id = "HEADLESS";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, TemplateBundleEnvelopeRoundTrips) {
    TemplateBundleMessage original;
    original.revision = 18'446'744'073'709'551'615ull;  // max uint64
    original.hash = "0123456789abcdef";
    original.json = R"({"revision":42,"templates":[]})";

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ComplianceReportRoundTripsEveryStatus) {
    ComplianceReportMessage original;
    original.report.host_id = "PC-001";
    original.report.applied_revision = 9;
    original.report.results = {
        CheckResult{"r1", CheckStatus::Pass, "running", ""},
        CheckResult{"r2", CheckStatus::Fail, "not running", ""},
        CheckResult{"r3", CheckStatus::NotApplicable, "not supported on this platform", ""},
        CheckResult{"r4", CheckStatus::Error, "read failed", "ERROR_ACCESS_DENIED"}};

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ComplianceReportRoundTripsWithNoResults) {
    ComplianceReportMessage original;
    original.report.host_id = "PC-001";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, HandlesNonAsciiAndEmbeddedSeparators) {
    ClientAnnounce original;
    original.host_id = "PC-Ø-001";
    original.agent_version = "0.1.0\nbuild\ttab";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, DecodeRejectsTruncatedPayload) {
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "0.1.0";
    std::vector<std::uint8_t> bytes = encode(original);
    ASSERT_GT(bytes.size(), 4u);
    bytes.resize(bytes.size() / 2);

    ClientAnnounce decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, DecodeRejectsEmptyPayload) {
    ClientAnnounce decoded;
    EXPECT_FALSE(decode(std::vector<std::uint8_t>{}, decoded));
}

TEST(Codec, KeysComeFromTheHostId) {
    ClientAnnounce announce;
    announce.host_id = "PC-001";
    EXPECT_EQ(key_of(announce), "PC-001");

    ResourceSampleMessage sample;
    sample.host_id = "PC-002";
    EXPECT_EQ(key_of(sample), "PC-002");

    ComplianceReportMessage report;
    report.report.host_id = "PC-003";
    EXPECT_EQ(key_of(report), "PC-003");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `lm/transport/codec.hpp` not found.

- [ ] **Step 3: Write `libs/transport/include/lm/transport/codec.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lm/transport/messages.hpp"

namespace lm::transport {

[[nodiscard]] std::vector<std::uint8_t> encode(const ClientAnnounce& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ResourceSampleMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const TemplateBundleMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ComplianceReportMessage& message);

/// Returns false on truncated or malformed input; never throws and never leaves
/// the output partially populated in a way the caller could mistake for success.
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ClientAnnounce& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ResourceSampleMessage& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, TemplateBundleMessage& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ComplianceReportMessage& out);

[[nodiscard]] std::string key_of(const ClientAnnounce& message);
[[nodiscard]] std::string key_of(const ResourceSampleMessage& message);
[[nodiscard]] std::string key_of(const ComplianceReportMessage& message);

}  // namespace lm::transport
```

- [ ] **Step 4: Write `libs/transport/src/codec.cpp`**

Uses FastCDR for CDR wire compatibility. The `Writer`/`Reader` helpers keep every
message body to a few lines, and confine all FastCDR API contact to one place.

> **Verify the FastCDR method names against the installed headers before building.**
> FastCDR 2.x renamed several methods from 1.x (`get_serialized_data_length()` versus
> `getSerializedDataLength()`). Open
> `build/windows/vcpkg_installed/x64-windows/include/fastcdr/Cdr.h` and confirm. The
> round-trip tests from Step 1 will fail loudly if anything is wrong, which is exactly
> what they are for.

```cpp
#include "lm/transport/codec.hpp"

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastcdr/exceptions/Exception.h>

namespace lm::transport {
namespace {

using eprosima::fastcdr::Cdr;
using eprosima::fastcdr::FastBuffer;

/// Serialises with a callable that writes into a Cdr, returning the bytes.
template <typename Body>
std::vector<std::uint8_t> serialise(Body&& body) {
    FastBuffer buffer;
    Cdr writer(buffer);
    std::forward<Body>(body)(writer);
    const auto length = writer.get_serialized_data_length();
    const auto* data = reinterpret_cast<const std::uint8_t*>(buffer.getBuffer());
    return std::vector<std::uint8_t>(data, data + length);
}

/// Deserialises with a callable that reads from a Cdr. Returns false if the
/// payload is empty or FastCDR reports it ran off the end.
template <typename Body>
bool deserialise(std::span<const std::uint8_t> bytes, Body&& body) {
    if (bytes.empty()) {
        return false;
    }
    try {
        FastBuffer buffer(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())),
                          bytes.size());
        Cdr reader(buffer);
        std::forward<Body>(body)(reader);
        return true;
    } catch (const eprosima::fastcdr::exception::Exception&) {
        return false;
    }
}

void write_disk(Cdr& writer, const core::DiskUsage& disk) {
    writer << disk.mount << disk.total_bytes << disk.free_bytes;
}

void read_disk(Cdr& reader, core::DiskUsage& disk) {
    reader >> disk.mount >> disk.total_bytes >> disk.free_bytes;
}

void write_result(Cdr& writer, const core::CheckResult& result) {
    writer << result.rule_id << static_cast<std::uint8_t>(result.status) << result.observed
           << result.message;
}

bool read_result(Cdr& reader, core::CheckResult& result) {
    std::uint8_t status = 0;
    reader >> result.rule_id >> status >> result.observed >> result.message;
    if (status > static_cast<std::uint8_t>(core::CheckStatus::Error)) {
        return false;
    }
    result.status = static_cast<core::CheckStatus>(status);
    return true;
}

}  // namespace

// --- ClientAnnounce --------------------------------------------------------

std::vector<std::uint8_t> encode(const ClientAnnounce& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.agent_version << message.capabilities;
    });
}

bool decode(std::span<const std::uint8_t> bytes, ClientAnnounce& out) {
    ClientAnnounce parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.host_id >> parsed.agent_version >> parsed.capabilities;
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- ResourceSampleMessage -------------------------------------------------

std::vector<std::uint8_t> encode(const ResourceSampleMessage& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.sample.cpu_percent << message.sample.mem_total_bytes
               << message.sample.mem_used_bytes
               << static_cast<std::uint32_t>(message.sample.disks.size());
        for (const core::DiskUsage& disk : message.sample.disks) {
            write_disk(writer, disk);
        }
    });
}

bool decode(std::span<const std::uint8_t> bytes, ResourceSampleMessage& out) {
    ResourceSampleMessage parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        std::uint32_t disk_count = 0;
        reader >> parsed.host_id >> parsed.sample.cpu_percent >> parsed.sample.mem_total_bytes >>
            parsed.sample.mem_used_bytes >> disk_count;
        parsed.sample.disks.resize(disk_count);
        for (core::DiskUsage& disk : parsed.sample.disks) {
            read_disk(reader, disk);
        }
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- TemplateBundleMessage -------------------------------------------------

std::vector<std::uint8_t> encode(const TemplateBundleMessage& message) {
    return serialise(
        [&](Cdr& writer) { writer << message.revision << message.hash << message.json; });
}

bool decode(std::span<const std::uint8_t> bytes, TemplateBundleMessage& out) {
    TemplateBundleMessage parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.revision >> parsed.hash >> parsed.json;
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- ComplianceReportMessage -----------------------------------------------

std::vector<std::uint8_t> encode(const ComplianceReportMessage& message) {
    return serialise([&](Cdr& writer) {
        writer << message.report.host_id << message.report.applied_revision
               << static_cast<std::uint32_t>(message.report.results.size());
        for (const core::CheckResult& result : message.report.results) {
            write_result(writer, result);
        }
    });
}

bool decode(std::span<const std::uint8_t> bytes, ComplianceReportMessage& out) {
    ComplianceReportMessage parsed;
    bool statuses_valid = true;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        std::uint32_t count = 0;
        reader >> parsed.report.host_id >> parsed.report.applied_revision >> count;
        parsed.report.results.resize(count);
        for (core::CheckResult& result : parsed.report.results) {
            if (!read_result(reader, result)) {
                statuses_valid = false;
            }
        }
    });
    if (ok && statuses_valid) {
        out = std::move(parsed);
        return true;
    }
    return false;
}

// --- keys ------------------------------------------------------------------

std::string key_of(const ClientAnnounce& message) { return message.host_id; }
std::string key_of(const ResourceSampleMessage& message) { return message.host_id; }
std::string key_of(const ComplianceReportMessage& message) { return message.report.host_id; }

}  // namespace lm::transport
```

- [ ] **Step 5: Update `libs/transport/CMakeLists.txt`**

```cmake
find_package(fastcdr CONFIG REQUIRED)

add_library(lm_transport STATIC
  src/in_memory_transport.cpp
  src/codec.cpp)

target_link_libraries(lm_transport PUBLIC lm_core PRIVATE fastcdr lm_warnings)

lm_add_test(lm_transport_tests
  SOURCES tests/test_in_memory_transport.cpp tests/test_codec.cpp
  LINK lm_transport)
```

- [ ] **Step 6: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass. If FastCDR method names differ, fix them per the note in
Step 4 and re-run.

- [ ] **Step 7: Commit**

```bash
git add libs/transport
git commit -m "feat: add FastCDR wire codecs for all four topics"
```

---

## Task 11: `FastDdsTransport`

**Files:**
- Create: `libs/transport/src/fastdds/topic_data_type.hpp`, `libs/transport/src/fastdds/fast_dds_transport.cpp`
- Create: `libs/transport/include/lm/transport/fast_dds_transport.hpp`
- Modify: `libs/transport/CMakeLists.txt`
- Test: `libs/transport/tests/test_fastdds_loopback.cpp` (gated by `LM_BUILD_INTEGRATION_TESTS`)

**Interfaces:**
- Consumes: `IClientTransport`, `IServerTransport` (Task 9); `encode`/`decode`/`key_of` (Task 10).
- Produces: `DdsConfig{domain_id, initial_peers, liveliness_lease}`, `make_dds_client(const DdsConfig&) -> std::unique_ptr<IClientTransport>`, `make_dds_server(const DdsConfig&) -> std::unique_ptr<IServerTransport>`.

**QoS, exactly as specified:**

| Topic | Reliability | Durability | History | Extra |
|---|---|---|---|---|
| `ClientAnnounce` | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_LAST(1)` | Liveliness `AUTOMATIC`, lease 10 s |
| `ResourceSample` | `BEST_EFFORT` | `VOLATILE` | `KEEP_LAST(1)` | — |
| `TemplateBundle` | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_LAST(1)` | — |
| `ComplianceReport` | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_LAST(1)` | — |

> **API-surface caveat, read this first.** Fast DDS 3.x reorganised namespaces and
> renamed several `TopicDataType` virtuals relative to 2.x. This plan does **not**
> reproduce those signatures from memory, because a wrong signature here costs more time
> than looking it up. Before writing `topic_data_type.hpp`, open the installed header:
>
> ```
> build/windows/vcpkg_installed/x64-windows/include/fastdds/dds/topic/TopicDataType.hpp
> ```
>
> and implement exactly the pure virtuals it declares. Every one of them is a thin
> forwarder to the Task 10 codec — `encode` for serialisation, `decode` for
> deserialisation, `encode(...).size()` for the size query, and `key_of` hashed into the
> instance handle for the keyed topics. The design work is already done; this is
> transcription against the real header.

- [ ] **Step 1: Read the installed Fast DDS headers**

```bash
ls build/windows/vcpkg_installed/x64-windows/include/fastdds/dds/topic/
```

Note the exact signatures of the pure virtuals on `TopicDataType`, and the
`DomainParticipantFactory` / `DataWriter` / `DataReader` API in
`fastdds/dds/domain/` and `fastdds/dds/publisher/`.

- [ ] **Step 2: Write `libs/transport/include/lm/transport/fast_dds_transport.hpp`**

```cpp
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "lm/transport/transport.hpp"

namespace lm::transport {

struct DdsConfig {
    int domain_id = 0;
    /// Explicit peers for networks where multicast discovery is blocked.
    /// Empty means rely on default multicast discovery.
    std::vector<std::string> initial_peers;
    std::chrono::milliseconds liveliness_lease = std::chrono::seconds{10};
};

[[nodiscard]] std::unique_ptr<IClientTransport> make_dds_client(const DdsConfig& config);
[[nodiscard]] std::unique_ptr<IServerTransport> make_dds_server(const DdsConfig& config);

}  // namespace lm::transport
```

- [ ] **Step 3: Write `libs/transport/src/fastdds/topic_data_type.hpp`**

A single class template parameterised on the message type, so all four topics share one
implementation. Each override forwards to the Task 10 codec. Match the virtuals to the
header you read in Step 1.

```cpp
#pragma once

#include <string>

#include "lm/transport/codec.hpp"

namespace lm::transport {

/// Adapts a message type to Fast DDS by forwarding to the free-function codec.
/// Keyed is true for the three topics keyed by host id.
template <typename Message, bool Keyed>
class MessageTopicDataType /* : public eprosima::fastdds::dds::TopicDataType */ {
    // Implement exactly the pure virtuals declared by the installed
    // TopicDataType.hpp. Each one is a forwarder:
    //
    //   serialise      -> encode(*static_cast<const Message*>(data))
    //   deserialise    -> decode({payload->data, payload->length}, *out)
    //   size query     -> encode(*static_cast<const Message*>(data)).size()
    //   create/delete  -> new Message / delete
    //   key            -> if constexpr (Keyed) hash key_of(message) into the handle
    //
    // No message-specific logic belongs here; all of it lives in codec.cpp.
};

}  // namespace lm::transport
```

- [ ] **Step 4: Write `libs/transport/src/fastdds/fast_dds_transport.cpp`**

Two classes, `DdsClientTransport` and `DdsServerTransport`, each owning a
`DomainParticipant`, its publisher/subscriber, and the topics for its role. Structure:

1. Create the participant on `config.domain_id`; if `initial_peers` is non-empty, add
   them as unicast locators in the participant QoS.
2. Register one `MessageTopicDataType` per topic.
3. Create writers and readers with the QoS from the table above.
4. `DataReaderListener::on_data_available` takes every sample, decodes it, and invokes
   the stored `std::function`. **Decode failures are logged and skipped, never fatal.**
5. `on_liveliness_changed` with a negative count drives `IServerTransport::on_client_lost`.
6. `on_publication_matched` / `on_subscription_matched` drive `on_connection_changed`:
   `Connected` when the matched count is above zero, `Disconnected` at zero.

> **Callbacks arrive on Fast DDS threads, not the GUI thread.** The applications must
> marshal into Qt via a queued connection — Task 13 and Task 14 handle this. Do not add
> Qt types here; `lm_transport` must stay Qt-free.

- [ ] **Step 5: Write the gated integration test `libs/transport/tests/test_fastdds_loopback.cpp`**

Publishes and subscribes within one process on a dedicated domain id, polling with a
timeout rather than a fixed sleep so it is not flaky.

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "lm/transport/fast_dds_transport.hpp"

using namespace lm::core;
using namespace lm::transport;
using namespace std::chrono_literals;

namespace {

/// Domain 42 keeps this test off the default domain other tooling might use.
DdsConfig loopback_config() {
    DdsConfig config;
    config.domain_id = 42;
    return config;
}

/// Polls until the predicate holds or the timeout expires. Returns whether it held.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return predicate();
}

}  // namespace

TEST(FastDdsLoopback, ServerDiscoversAnAnnouncingClient) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<bool> seen{false};
    server->on_announce([&](const ClientAnnounce& message) {
        if (message.host_id == "LOOPBACK-PC") {
            seen = true;
        }
    });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ClientAnnounce announce;
    announce.host_id = "LOOPBACK-PC";
    announce.agent_version = "0.1.0";
    announce.capabilities = platform_capabilities().raw();
    client->publish_announce(announce);

    EXPECT_TRUE(wait_for([&] { return seen.load(); }));
}

TEST(FastDdsLoopback, ResourceSamplesReachTheServer) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<int> received{0};
    server->on_resources([&](const ResourceSampleMessage&) { ++received; });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ResourceSampleMessage sample;
    sample.host_id = "LOOPBACK-PC";
    sample.sample.cpu_percent = 12.5;
    client->publish_resources(sample);

    EXPECT_TRUE(wait_for([&] { return received.load() > 0; }));
}

TEST(FastDdsLoopback, LateJoiningClientReceivesTheRetainedBundle) {
    const auto server = make_dds_server(loopback_config());

    TemplateBundleMessage bundle;
    bundle.revision = 5;
    bundle.hash = "abc";
    bundle.json = R"({"revision":5,"hash":"abc","baseline":{"name":"b","rules":[]},
                      "templates":[],"assignments":{}})";
    server->publish_bundle(bundle);

    // Client created after the publish — TransientLocal must still deliver it.
    const auto client = make_dds_client(loopback_config());

    std::atomic<std::uint64_t> revision{0};
    client->on_bundle([&](const TemplateBundleMessage& message) { revision = message.revision; });

    EXPECT_TRUE(wait_for([&] { return revision.load() == 5u; }));
}
```

- [ ] **Step 6: Update `libs/transport/CMakeLists.txt`**

```cmake
find_package(fastdds CONFIG REQUIRED)

target_sources(lm_transport PRIVATE src/fastdds/fast_dds_transport.cpp)
target_link_libraries(lm_transport PRIVATE fastdds fastcdr lm_warnings)

if(LM_BUILD_INTEGRATION_TESTS)
  lm_add_test(lm_transport_dds_tests
    SOURCES tests/test_fastdds_loopback.cpp
    LINK lm_transport)
endif()
```

- [ ] **Step 7: Build and run the unit tests**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all non-integration tests pass.

- [ ] **Step 8: Run the integration tests explicitly**

```bash
cmake --preset windows -DLM_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: the three loopback tests pass. If discovery fails, the machine's firewall is
blocking multicast — set `initial_peers` to `127.0.0.1` and re-run to confirm that is
the cause.

- [ ] **Step 9: Commit**

```bash
git add libs/transport
git commit -m "feat: add Fast DDS transport with spec QoS and loopback tests"
```

---

## Task 12: `lm_ui` — theme, widgets and the coalescing fleet model

**Files:**
- Create: `libs/ui/CMakeLists.txt`
- Create: `libs/ui/include/lm/ui/theme.hpp`, `libs/ui/src/theme.cpp`, `libs/ui/resources/theme.qss`, `libs/ui/resources/lm_ui.qrc`
- Create: `libs/ui/include/lm/ui/status_pill.hpp`, `libs/ui/src/status_pill.cpp`
- Create: `libs/ui/include/lm/ui/sparkline.hpp`, `libs/ui/src/sparkline.cpp`
- Create: `libs/ui/include/lm/ui/meter_bar.hpp`, `libs/ui/src/meter_bar.cpp`
- Create: `libs/ui/include/lm/ui/fleet_model.hpp`, `libs/ui/src/fleet_model.cpp`
- Create: `libs/ui/include/lm/ui/sample_coalescer.hpp`, `libs/ui/src/sample_coalescer.cpp`
- Modify: `CMakeLists.txt`, `cmake/LabMonitorTesting.cmake`
- Test: `libs/ui/tests/main.cpp`, `libs/ui/tests/test_fleet_model.cpp`, `libs/ui/tests/test_sample_coalescer.cpp`

**Interfaces:**
- Consumes: `core::FleetView`, `core::FleetEntry`, `core::HostState` (Task 6); `transport::ResourceSampleMessage` (Task 9).
- Produces: `Theme::apply(QApplication&)`, `Theme::color_for(core::HostState)`, `Theme::glyph_for(core::HostState)`; `StatusPill`, `Sparkline`, `MeterBar`; `FleetModel` with `apply(const core::FleetView&)` and `apply_sample(const transport::ResourceSampleMessage&)`, roles `HostIdRole`/`SeverityRole`/`StateRole`; `SampleCoalescer` with `push()` and the `flushed(QVector<transport::ResourceSampleMessage>)` signal.

**Three decisions that carry the "snappy" requirement:**

1. **`FleetModel` keeps rows in a stable order — sorted by host id — and never reorders
   itself.** Severity sorting is done by a `QSortFilterProxyModel` reading `SeverityRole`.
   This is the crucial one: if the model reordered on every state change, a host going
   offline would move rows and force the view to repaint everything. With a stable model,
   `apply()` only ever inserts, removes, or emits `dataChanged` on the cells that changed.
2. **`apply_sample()` emits `dataChanged` for the three resource columns only** — not the
   whole row, and never `beginResetModel()`.
3. **`SampleCoalescer` keeps the latest sample per host** behind a mutex and flushes on a
   100 ms timer, so a burst of DDS traffic produces one repaint rather than dozens.
   Latest-wins per host mirrors the `KEEP_LAST(1)` QoS exactly.

Widgets are verified by running the applications; the **tests here cover the model and
the coalescer**, which is where the logic is. They need only `QCoreApplication`, so they
run headless in CI.

- [ ] **Step 1: Add a `NO_GTEST_MAIN` option to `cmake/LabMonitorTesting.cmake`**

Qt tests supply their own `main()` so they can construct a `QCoreApplication` before the
tests run. Linking `GTest::gtest_main` as well would give two `main` symbols.

```cmake
function(lm_add_test target)
  cmake_parse_arguments(ARG "NO_GTEST_MAIN" "" "SOURCES;LINK" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE ${ARG_LINK} GTest::gtest GTest::gmock lm_warnings)
  if(NOT ARG_NO_GTEST_MAIN)
    target_link_libraries(${target} PRIVATE GTest::gtest_main)
  endif()
  add_test(NAME ${target} COMMAND ${target})
  copy_runtime_dependencies(${target})
endfunction()
```

- [ ] **Step 2: Write the failing test `libs/ui/tests/test_fleet_model.cpp`**

```cpp
#include <gtest/gtest.h>

#include <QSignalSpy>

#include "lm/ui/fleet_model.hpp"

using namespace lm::core;
using namespace lm::ui;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = Clock::time_point{} + 1'000'000s;

FleetView view_with(std::vector<std::pair<HostId, HostState>> hosts) {
    FleetView view;
    for (auto& [id, state] : hosts) {
        FleetEntry entry;
        entry.host_id = id;
        entry.state = state;
        entry.last_seen = kNow;
        view.entries.push_back(entry);
    }
    return view;
}

int row_of(const FleetModel& model, const QString& host_id) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.data(model.index(row, 0), FleetModel::HostIdRole).toString() == host_id) {
            return row;
        }
    }
    return -1;
}

}  // namespace

TEST(FleetModel, StartsEmpty) {
    FleetModel model;
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_GT(model.columnCount(), 0);
}

TEST(FleetModel, InsertsRowsWithoutResetting) {
    FleetModel model;
    QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insert_spy(&model, &QAbstractItemModel::rowsInserted);

    model.apply(view_with({{"PC-002", HostState::Online}, {"PC-001", HostState::Online}}));

    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(reset_spy.count(), 0);
    EXPECT_GT(insert_spy.count(), 0);
}

TEST(FleetModel, KeepsRowsInStableHostIdOrder) {
    FleetModel model;
    model.apply(view_with({{"PC-003", HostState::Online},
                           {"PC-001", HostState::Missing},
                           {"PC-002", HostState::Offline}}));

    EXPECT_EQ(row_of(model, "PC-001"), 0);
    EXPECT_EQ(row_of(model, "PC-002"), 1);
    EXPECT_EQ(row_of(model, "PC-003"), 2);
}

TEST(FleetModel, StateChangeDoesNotMoveTheRow) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Online}}));
    const int before = row_of(model, "PC-002");

    QSignalSpy move_spy(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Missing}}));

    EXPECT_EQ(row_of(model, "PC-002"), before);
    EXPECT_EQ(move_spy.count(), 0);
    EXPECT_EQ(reset_spy.count(), 0);
}

TEST(FleetModel, SeverityRoleDrivesProxySorting) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Missing}}));

    const int online = model.data(model.index(row_of(model, "PC-001"), 0),
                                  FleetModel::SeverityRole).toInt();
    const int missing = model.data(model.index(row_of(model, "PC-002"), 0),
                                   FleetModel::SeverityRole).toInt();
    EXPECT_LT(missing, online);  // lower sorts first: Missing is most urgent
}

TEST(FleetModel, RemovesHostsThatLeaveTheView) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Unexpected}}));

    QSignalSpy remove_spy(&model, &QAbstractItemModel::rowsRemoved);
    model.apply(view_with({{"PC-001", HostState::Online}}));

    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(row_of(model, "PC-002"), -1);
    EXPECT_GT(remove_spy.count(), 0);
}

TEST(FleetModel, UnchangedViewEmitsNoDataChanged) {
    FleetModel model;
    const FleetView view = view_with({{"PC-001", HostState::Online}});
    model.apply(view);

    QSignalSpy changed_spy(&model, &QAbstractItemModel::dataChanged);
    model.apply(view);

    EXPECT_EQ(changed_spy.count(), 0);
}

TEST(FleetModel, SampleUpdatesOnlyTheResourceColumns) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    QSignalSpy changed_spy(&model, &QAbstractItemModel::dataChanged);

    lm::transport::ResourceSampleMessage sample;
    sample.host_id = "PC-001";
    sample.sample.cpu_percent = 55.0;
    sample.sample.mem_total_bytes = 1000;
    sample.sample.mem_used_bytes = 500;
    model.apply_sample(sample);

    ASSERT_GT(changed_spy.count(), 0);
    const auto arguments = changed_spy.takeFirst();
    const auto top_left = arguments.at(0).toModelIndex();
    const auto bottom_right = arguments.at(1).toModelIndex();
    EXPECT_EQ(top_left.row(), bottom_right.row());
    // A contiguous resource block, not the whole row.
    EXPECT_LT(bottom_right.column() - top_left.column() + 1, model.columnCount());
}

TEST(FleetModel, SampleForAnUnknownHostIsIgnored) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage sample;
    sample.host_id = "GHOST";
    EXPECT_NO_THROW(model.apply_sample(sample));
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(FleetModel, ProvidesHeadersForEveryColumn) {
    FleetModel model;
    for (int column = 0; column < model.columnCount(); ++column) {
        EXPECT_FALSE(model.headerData(column, Qt::Horizontal, Qt::DisplayRole)
                         .toString()
                         .isEmpty());
    }
}
```

- [ ] **Step 3: Write the failing test `libs/ui/tests/test_sample_coalescer.cpp`**

```cpp
#include <gtest/gtest.h>

#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include "lm/ui/sample_coalescer.hpp"

using namespace lm::transport;
using namespace lm::ui;
using namespace std::chrono_literals;

namespace {

ResourceSampleMessage sample_for(const std::string& host, double cpu) {
    ResourceSampleMessage message;
    message.host_id = host;
    message.sample.cpu_percent = cpu;
    return message;
}

/// Spins the event loop until the spy sees a signal or the timeout expires.
bool wait_for_signal(QSignalSpy& spy, int milliseconds = 2000) {
    return spy.wait(milliseconds);
}

}  // namespace

TEST(SampleCoalescer, CollapsesABurstForOneHostIntoTheLatestSample) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    for (double cpu : {10.0, 20.0, 30.0, 99.5}) {
        coalescer.push(sample_for("PC-001", cpu));
    }

    ASSERT_TRUE(wait_for_signal(spy));
    const auto batch = spy.takeFirst().at(0).value<QVector<ResourceSampleMessage>>();
    ASSERT_EQ(batch.size(), 1);
    EXPECT_DOUBLE_EQ(batch.front().sample.cpu_percent, 99.5);
}

TEST(SampleCoalescer, KeepsOneEntryPerHost) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    coalescer.push(sample_for("PC-001", 10.0));
    coalescer.push(sample_for("PC-002", 20.0));
    coalescer.push(sample_for("PC-001", 30.0));

    ASSERT_TRUE(wait_for_signal(spy));
    const auto batch = spy.takeFirst().at(0).value<QVector<ResourceSampleMessage>>();
    EXPECT_EQ(batch.size(), 2);
}

TEST(SampleCoalescer, DoesNotEmitWhenNothingWasPushed) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(spy.count(), 0);
}

TEST(SampleCoalescer, ClearsItsBufferBetweenFlushes) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    coalescer.push(sample_for("PC-001", 10.0));
    ASSERT_TRUE(wait_for_signal(spy));
    spy.clear();

    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(spy.count(), 0);
}
```

- [ ] **Step 4: Write `libs/ui/tests/main.cpp`**

```cpp
#include <gtest/gtest.h>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 5: Run the tests to verify they fail**

Expected: FAIL — `lm/ui/fleet_model.hpp` not found.

- [ ] **Step 6: Write `libs/ui/include/lm/ui/theme.hpp` and `src/theme.cpp`**

```cpp
#pragma once

#include <QColor>
#include <QString>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"

class QApplication;

namespace lm::ui {

/// Dark slate with a single cyan accent. Status is always colour *and* glyph,
/// never colour alone, so it survives greyscale and colour-blindness.
namespace Theme {

inline constexpr const char* kAccent = "#22d3ee";
inline constexpr const char* kBackground = "#0f172a";
inline constexpr const char* kSurface = "#1e293b";
inline constexpr const char* kText = "#e2e8f0";
inline constexpr const char* kTextMuted = "#94a3b8";

inline constexpr const char* kOnline = "#34d399";
inline constexpr const char* kOffline = "#fbbf24";
inline constexpr const char* kMissing = "#f87171";
inline constexpr const char* kUnexpected = "#a78bfa";
inline constexpr const char* kNotApplicable = "#64748b";

void apply(QApplication& app);

[[nodiscard]] QColor color_for(core::HostState state);
[[nodiscard]] QColor color_for(core::CheckStatus status);

/// A distinct shape per state, so hue is never the only signal.
[[nodiscard]] QString glyph_for(core::HostState state);
[[nodiscard]] QString glyph_for(core::CheckStatus status);

}  // namespace Theme
}  // namespace lm::ui
```

`apply()` loads `:/lm_ui/theme.qss` from the Qt resource file and calls
`app.setStyleSheet()`. `glyph_for` returns `"✓"` for Online/Pass, `"!"` for
Offline/Fail, `"✕"` for Missing, `"?"` for Unexpected, and `"◌"` for
NotApplicable.

- [ ] **Step 7: Write the three widgets**

- **`StatusPill`** — a `QWidget` painting a rounded rect in `Theme::color_for()`, the
  glyph, and a caption. Setter `set_state(core::HostState)` calls `update()`.
- **`Sparkline`** — a `QWidget` holding a fixed-capacity ring buffer (default 60
  points). `push(double)` appends and calls `update()`. `paintEvent` draws a polyline
  scaled to the widget rect with antialiasing, plus a subtle filled area under the
  curve. **No QtCharts dependency.**
- **`MeterBar`** — a horizontal bar with `set_value(double percent)` animating through
  `QVariantAnimation` with `QEasingCurve::OutCubic` over 300 ms, so values glide rather
  than snap.

Each is under 100 lines and takes no dependency beyond `Qt5::Widgets` and `lm_core`.

- [ ] **Step 8: Write `libs/ui/include/lm/ui/fleet_model.hpp` and `src/fleet_model.cpp`**

```cpp
#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <vector>

#include "lm/core/fleet.hpp"
#include "lm/transport/messages.hpp"

namespace lm::ui {

/// Rows are held in a stable order (by host id) and are never reordered. Wrap
/// this in a QSortFilterProxyModel with sortRole == SeverityRole to present the
/// most-urgent-first order without churning rows in the view.
class FleetModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        HostColumn = 0,
        StateColumn,
        CpuColumn,
        MemoryColumn,
        DiskColumn,
        RevisionColumn,
        LastSeenColumn,
        ColumnCount
    };

    enum Role {
        HostIdRole = Qt::UserRole + 1,
        SeverityRole,  ///< lower sorts first
        StateRole,
        StaleRole,
    };

    explicit FleetModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    /// Merges a reconciled view: inserts new hosts, removes departed ones, and
    /// emits dataChanged only for cells that actually changed.
    void apply(const core::FleetView& view);

    /// Updates only the CPU, memory and disk columns for one host.
    void apply_sample(const transport::ResourceSampleMessage& sample);

private:
    struct Row {
        core::FleetEntry entry;
        core::ResourceSample resources;
        bool has_resources = false;
    };

    [[nodiscard]] int index_of(const core::HostId& host_id) const;

    std::vector<Row> rows_;  ///< always sorted by entry.host_id
};

}  // namespace lm::ui
```

Implementation notes for `apply()`: build a host-id-sorted copy of `view.entries`, then
walk both sequences in parallel. Hosts only in the new sequence trigger
`beginInsertRows`/`endInsertRows`; hosts only in the old trigger
`beginRemoveRows`/`endRemoveRows`; hosts in both compare `entry` field-by-field and emit
`dataChanged` for the affected column range only. `apply_sample()` looks up the row, and
if the sample differs, emits `dataChanged(index(row, CpuColumn), index(row, DiskColumn))`.

- [ ] **Step 9: Write `libs/ui/include/lm/ui/sample_coalescer.hpp` and `src/sample_coalescer.cpp`**

```cpp
#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "lm/transport/messages.hpp"

Q_DECLARE_METATYPE(lm::transport::ResourceSampleMessage)

namespace lm::ui {

/// Buffers incoming resource samples and emits them in batches, so a burst of
/// DDS traffic produces one repaint instead of dozens. push() is safe to call
/// from a Fast DDS callback thread; flushed() is emitted on the owning thread.
class SampleCoalescer : public QObject {
    Q_OBJECT

public:
    explicit SampleCoalescer(std::chrono::milliseconds interval = std::chrono::milliseconds{100},
                             QObject* parent = nullptr);

    /// Thread-safe. Later samples for the same host replace earlier ones,
    /// mirroring the KEEP_LAST(1) QoS on the ResourceSample topic.
    void push(transport::ResourceSampleMessage sample);

signals:
    void flushed(QVector<transport::ResourceSampleMessage> batch);

private:
    void flush();

    QTimer timer_;
    std::mutex mutex_;
    std::map<std::string, transport::ResourceSampleMessage> pending_;
};

}  // namespace lm::ui
```

`flush()` swaps `pending_` under the lock, returns early if it is empty (so no signal is
emitted when nothing arrived), and emits the batch outside the lock.

- [ ] **Step 10: Write `libs/ui/CMakeLists.txt`**

```cmake
find_package(Qt5 COMPONENTS Widgets Svg REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(lm_ui STATIC
  src/theme.cpp
  src/status_pill.cpp
  src/sparkline.cpp
  src/meter_bar.cpp
  src/fleet_model.cpp
  src/sample_coalescer.cpp
  resources/lm_ui.qrc)
add_library(lm::ui ALIAS lm_ui)

target_include_directories(lm_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_ui
  PUBLIC lm_core lm_transport Qt5::Widgets Qt5::Svg
  PRIVATE lm_warnings)

lm_add_test(lm_ui_tests
  NO_GTEST_MAIN
  SOURCES tests/main.cpp tests/test_fleet_model.cpp tests/test_sample_coalescer.cpp
  LINK lm_ui)
```

Add to the top-level `CMakeLists.txt`:

```cmake
if(LM_BUILD_GUI)
  add_subdirectory(libs/ui)
endif()
```

- [ ] **Step 11: Build and run**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass, including the model and coalescer suites.

- [ ] **Step 12: Commit**

```bash
git add libs/ui cmake/LabMonitorTesting.cmake CMakeLists.txt
git commit -m "feat: add lm_ui theme, widgets, stable fleet model and sample coalescer"
```

---

## Task 13: Client application

**Files:**
- Create: `apps/client/CMakeLists.txt`, `apps/client/main.cpp`
- Create: `apps/client/monitor_worker.hpp`, `apps/client/monitor_worker.cpp`
- Create: `apps/client/tray_controller.hpp`, `apps/client/tray_controller.cpp`
- Create: `apps/client/detail_window.hpp`, `apps/client/detail_window.cpp`
- Modify: `CMakeLists.txt`
- Test: `apps/client/tests/test_monitor_worker.cpp`

**Interfaces:**
- Consumes: `HostProbes`, `make_platform_probes()`, `local_host_name()` (Tasks 7–8); `IClientTransport`, `make_dds_client`, `make_in_memory_client` (Tasks 9, 11); `Theme`, `Sparkline`, `MeterBar`, `StatusPill` (Task 12); `evaluate()` (Task 5).
- Produces: `MonitorWorker` with signals `resources_sampled(core::ResourceSample)`, `report_ready(core::ComplianceReport)`, `connection_changed(transport::ConnectionState)`; `TrayController`; `DetailWindow`.

**Threading, restated because it is the whole point:** `MonitorWorker` is moved onto a
worker `QThread`. It owns the probes and the transport. It communicates with the GUI
purely through queued signals. **No registry read, service enumeration or DDS call ever
happens on the GUI thread.**

- [ ] **Step 1: Write the failing test `apps/client/tests/test_monitor_worker.cpp`**

Uses `InMemoryTransport` and the probe fakes, so it needs no DDS domain and no real OS
probing. Drives the worker synchronously via its slots rather than waiting on timers.

```cpp
#include <gtest/gtest.h>

#include "lm/core/json.hpp"  // serialise_bundle, content_hash
#include "lm/platform/fakes.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "monitor_worker.hpp"

using namespace lm::core;
using namespace lm::platform;
using namespace lm::transport;

namespace {

struct Fixture {
    MessageBus bus;
    FakeResourceProbe* resources = nullptr;
    FakeProcessProbe* processes = nullptr;
    std::unique_ptr<MonitorWorker> worker;

    Fixture() {
        auto resource_probe = std::make_unique<FakeResourceProbe>();
        auto process_probe = std::make_unique<FakeProcessProbe>();
        resources = resource_probe.get();
        processes = process_probe.get();

        ProbeSet set;
        set.resources = std::move(resource_probe);
        set.processes = std::move(process_probe);

        auto probes = std::make_unique<HostProbes>(
            "PC-001", std::move(set),
            Capabilities{}.add(Capability::Resources).add(Capability::Processes));

        worker = std::make_unique<MonitorWorker>(std::move(probes), make_in_memory_client(bus));
    }
};

TemplateBundleMessage bundle_message(std::uint64_t revision, std::vector<Rule> rules) {
    TemplateBundle bundle;
    bundle.revision = revision;
    Template tmpl;
    tmpl.name = "Lab Workstation";
    tmpl.rules = std::move(rules);
    bundle.templates = {tmpl};
    bundle.assignments["PC-001"] = {"Lab Workstation"};

    TemplateBundleMessage message;
    message.revision = revision;
    message.hash = content_hash(bundle);
    message.json = serialise_bundle(bundle);
    return message;
}

Rule process_rule(std::string exe) {
    Rule rule;
    rule.id = "p1";
    rule.expectation = Presence::MustBePresent;
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

}  // namespace

TEST(MonitorWorker, AnnouncesItselfOnStart) {
    Fixture fixture;

    std::vector<ClientAnnounce> announcements;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_announce([&](const ClientAnnounce& message) { announcements.push_back(message); });

    fixture.worker->start();

    ASSERT_EQ(announcements.size(), 1u);
    EXPECT_EQ(announcements.front().host_id, "PC-001");
    EXPECT_FALSE(announcements.front().agent_version.empty());
}

TEST(MonitorWorker, PublishesResourceSamplesOnTick) {
    Fixture fixture;
    fixture.resources->next.cpu_percent = 44.0;

    std::optional<ResourceSampleMessage> received;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_resources([&](const ResourceSampleMessage& message) { received = message; });

    fixture.worker->sample_resources();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->host_id, "PC-001");
    EXPECT_DOUBLE_EQ(received->sample.cpu_percent, 44.0);
}

TEST(MonitorWorker, WithNoTemplateReportsResourcesOnly) {
    Fixture fixture;

    std::optional<ComplianceReportMessage> received;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    fixture.worker->evaluate_compliance();

    ASSERT_TRUE(received.has_value());
    EXPECT_TRUE(received->report.results.empty());
    EXPECT_EQ(received->report.applied_revision, 0u);
    EXPECT_EQ(fixture.processes->calls, 0);  // nothing to probe
}

TEST(MonitorWorker, AppliesAPublishedTemplateAndReports) {
    Fixture fixture;
    fixture.processes->next = {ProcessInfo{"antivirus.exe", std::nullopt}};

    const auto server = make_in_memory_server(fixture.bus);
    std::optional<ComplianceReportMessage> received;
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    fixture.worker->start();
    server->publish_bundle(bundle_message(3, {process_rule("antivirus.exe")}));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->report.applied_revision, 3u);
    ASSERT_EQ(received->report.results.size(), 1u);
    EXPECT_EQ(received->report.results.front().status, CheckStatus::Pass);
}

TEST(MonitorWorker, ReevaluatesImmediatelyWhenTheTemplateChanges) {
    Fixture fixture;
    fixture.processes->next = {ProcessInfo{"antivirus.exe", std::nullopt}};

    const auto server = make_in_memory_server(fixture.bus);
    int reports = 0;
    server->on_report([&](const ComplianceReportMessage&) { ++reports; });

    fixture.worker->start();
    server->publish_bundle(bundle_message(1, {process_rule("antivirus.exe")}));
    server->publish_bundle(bundle_message(2, {process_rule("other.exe")}));

    EXPECT_EQ(reports, 2);
}

TEST(MonitorWorker, IgnoresARepublishedIdenticalRevision) {
    Fixture fixture;

    const auto server = make_in_memory_server(fixture.bus);
    int reports = 0;
    server->on_report([&](const ComplianceReportMessage&) { ++reports; });

    fixture.worker->start();
    const TemplateBundleMessage message = bundle_message(1, {process_rule("a.exe")});
    server->publish_bundle(message);
    server->publish_bundle(message);

    EXPECT_EQ(reports, 1);
}

TEST(MonitorWorker, RejectsAMalformedBundleWithoutCrashing) {
    Fixture fixture;
    const auto server = make_in_memory_server(fixture.bus);
    fixture.worker->start();

    TemplateBundleMessage broken;
    broken.revision = 9;
    broken.json = "{ not json";

    EXPECT_NO_THROW(server->publish_bundle(broken));
    EXPECT_EQ(fixture.worker->applied_revision(), 0u);  // last good state retained
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `monitor_worker.hpp` not found.

- [ ] **Step 3: Write `apps/client/monitor_worker.hpp`**

```cpp
#pragma once

#include <QObject>
#include <QTimer>

#include <memory>

#include "lm/platform/probes.hpp"
#include "lm/transport/transport.hpp"

/// Owns all probing and messaging. Lives on a worker thread; talks to the GUI
/// only through queued signals.
class MonitorWorker : public QObject {
    Q_OBJECT

public:
    MonitorWorker(std::unique_ptr<lm::platform::HostProbes> probes,
                  std::unique_ptr<lm::transport::IClientTransport> transport,
                  QObject* parent = nullptr);

    [[nodiscard]] std::uint64_t applied_revision() const { return bundle_.revision; }

public slots:
    /// Announces this client and subscribes to template updates.
    void start();
    /// Fast tick: samples resources and publishes them.
    void sample_resources();
    /// Slow tick: collects facts, evaluates the template, publishes the report.
    void evaluate_compliance();
    void set_reporting_paused(bool paused);

signals:
    void resources_sampled(lm::core::ResourceSample sample);
    void report_ready(lm::core::ComplianceReport report);
    void template_applied(quint64 revision);
    void connection_changed(int state);

private:
    void on_bundle(const lm::transport::TemplateBundleMessage& message);

    std::unique_ptr<lm::platform::HostProbes> probes_;
    std::unique_ptr<lm::transport::IClientTransport> transport_;
    lm::core::TemplateBundle bundle_;
    bool paused_ = false;
};
```

`on_bundle` parses the JSON; on failure it logs via `spdlog` and **keeps the last good
bundle**, then on success stores it and calls `evaluate_compliance()` immediately. It
returns early when `message.revision == bundle_.revision`, which is what makes the
republish test pass.

- [ ] **Step 4: Write `apps/client/tray_controller.hpp/.cpp`**

`QSystemTrayIcon` whose icon is rendered from an SVG template recoloured by state
(green / amber / red / grey). Context menu: **Open**, **Pause reporting** (checkable),
**Copy diagnostics**, **Quit**. Activation on `QSystemTrayIcon::Trigger` toggles the
detail window. Tooltip shows hostname plus current CPU and disk.

- [ ] **Step 5: Write `apps/client/detail_window.hpp/.cpp`**

Three bands as specified: header (hostname, `StatusPill` for connection, applied
template name and revision); resource strip (`Sparkline` for CPU, `MeterBar` for memory,
one `MeterBar` per volume); compliance list grouped into Applications / Services /
Registry with `NotApplicable` rows dimmed. `closeEvent` calls `hide()` and
`event->ignore()` so closing never quits the app.

- [ ] **Step 6: Write `apps/client/main.cpp`**

```cpp
// Structure (not the full file):
//   - QApplication app{argc, argv};
//   - app.setQuitOnLastWindowClosed(false);        // tray app: closing the window must not exit
//   - boost::program_options: --domain-id, --config, --offline, --log-level
//   - spdlog rotating file sink + console sink
//   - lm::ui::Theme::apply(app);
//   - auto probes   = std::make_unique<HostProbes>(local_host_name(), make_platform_probes(),
//                                                  platform_capabilities());
//   - auto transport = options.offline ? make_in_memory_client(bus) : make_dds_client(config);
//   - auto* worker = new MonitorWorker{std::move(probes), std::move(transport)};
//   - QThread* thread = new QThread; worker->moveToThread(thread);
//   - QTimer* fast = new QTimer{worker}; fast->setInterval(2000);
//   - QTimer* slow = new QTimer{worker}; slow->setInterval(30000);
//     (both created on the worker thread inside start(), so they tick there)
//   - connect worker signals to DetailWindow/TrayController slots (queued by default
//     across threads — do not use Qt::DirectConnection here)
//   - thread->start(); QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
//   - The window is NOT shown: the app starts hidden with only a tray icon.
```

- [ ] **Step 7: Write `apps/client/CMakeLists.txt`**

```cmake
find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(spdlog CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(lab_monitor_client WIN32
  main.cpp
  monitor_worker.cpp
  tray_controller.cpp
  detail_window.cpp)

target_include_directories(lab_monitor_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lab_monitor_client PRIVATE
  lm_core lm_platform lm_transport lm_ui
  Qt5::Widgets Boost::program_options spdlog::spdlog lm_warnings)

copy_runtime_dependencies(lab_monitor_client)

lm_add_test(lab_monitor_client_tests
  NO_GTEST_MAIN
  SOURCES ${CMAKE_SOURCE_DIR}/libs/ui/tests/main.cpp tests/test_monitor_worker.cpp
          monitor_worker.cpp
  LINK lm_core lm_platform lm_transport Qt5::Core)
target_include_directories(lab_monitor_client_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

The `WIN32` keyword makes it a GUI subsystem binary, so no console window appears.

Add to the top-level `CMakeLists.txt` inside `if(LM_BUILD_GUI)`:
`add_subdirectory(apps/client)`.

- [ ] **Step 8: Build and run the tests**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 9: Run the client manually**

```bash
./build/windows/Debug/lab_monitor_client.exe --offline
```

Expected: no window appears; a tray icon shows; clicking it opens the detail window
with live CPU, memory and disk; closing the window returns to the tray.

- [ ] **Step 10: Commit**

```bash
git add apps/client CMakeLists.txt
git commit -m "feat: add tray client with threaded monitor worker"
```

---

## Task 14: Server application

**Files:**
- Create: `libs/core/include/lm/core/client_registry.hpp`, `libs/core/src/client_registry.cpp`
- Create: `apps/server/CMakeLists.txt`, `apps/server/main.cpp`
- Create: `apps/server/server_controller.hpp`, `apps/server/server_controller.cpp`
- Create: `apps/server/fleet_window.hpp`, `apps/server/fleet_window.cpp`
- Create: `apps/server/status_ribbon.hpp`, `apps/server/status_ribbon.cpp`
- Modify: `libs/core/CMakeLists.txt`, `CMakeLists.txt`
- Test: `libs/core/tests/test_client_registry.cpp`

**Interfaces:**
- Consumes: `reconcile()`, `FleetView` (Task 6); `IServerTransport` (Tasks 9, 11); `FleetModel`, `SampleCoalescer`, `StatusRibbon` inputs (Task 12).
- Produces: `core::ClientRegistry` with `record_announce`, `record_sample`, `record_report`, `mark_lost`, `snapshot() -> std::vector<DiscoveredClient>`; `ServerController`; `FleetWindow`; `StatusRibbon`.

> **`ClientRegistry` goes in `lm_core`, not the app.** It is pure bookkeeping over
> `DiscoveredClient` records with no I/O, which makes it unit-testable and keeps the
> server application to wiring and widgets.

- [ ] **Step 1: Write the failing test `libs/core/tests/test_client_registry.cpp`**

```cpp
#include <gtest/gtest.h>

#include "lm/core/client_registry.hpp"

using namespace lm::core;
using namespace std::chrono_literals;

namespace {
const TimePoint kNow = Clock::time_point{} + 1'000'000s;
}

TEST(ClientRegistry, StartsEmpty) {
    const ClientRegistry registry;
    EXPECT_TRUE(registry.snapshot().empty());
}

TEST(ClientRegistry, RecordsAnAnnouncement) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}.add(Capability::Resources), kNow);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients.front().host_id, "PC-001");
    EXPECT_TRUE(clients.front().caps.has(Capability::Resources));
    EXPECT_EQ(clients.front().last_seen, kNow);
}

TEST(ClientRegistry, SamplesRefreshLastSeenWithoutDuplicating) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_sample("PC-001", kNow + 5s);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients.front().last_seen, kNow + 5s);
}

TEST(ClientRegistry, ASampleFromAnUnannouncedHostStillRegistersIt) {
    ClientRegistry registry;
    registry.record_sample("ROGUE", kNow);
    EXPECT_EQ(registry.snapshot().size(), 1u);
}

TEST(ClientRegistry, ReportsRecordTheAppliedRevision) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_report("PC-001", 7, kNow + 1s);

    EXPECT_EQ(registry.snapshot().front().applied_revision, 7u);
}

TEST(ClientRegistry, AnnouncementDoesNotResetAKnownRevision) {
    ClientRegistry registry;
    registry.record_report("PC-001", 7, kNow);
    registry.record_announce("PC-001", Capabilities{}, kNow + 1s);

    EXPECT_EQ(registry.snapshot().front().applied_revision, 7u);
}

TEST(ClientRegistry, MarkLostRemovesTheClient) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.mark_lost("PC-001");
    EXPECT_TRUE(registry.snapshot().empty());
}

TEST(ClientRegistry, MarkLostForAnUnknownHostIsHarmless) {
    ClientRegistry registry;
    EXPECT_NO_THROW(registry.mark_lost("GHOST"));
}

TEST(ClientRegistry, SnapshotIsOrderedByHostId) {
    ClientRegistry registry;
    registry.record_announce("PC-003", Capabilities{}, kNow);
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_announce("PC-002", Capabilities{}, kNow);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 3u);
    EXPECT_EQ(clients[0].host_id, "PC-001");
    EXPECT_EQ(clients[2].host_id, "PC-003");
}

TEST(ClientRegistry, FeedsReconcileDirectly) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);

    ReconcileOptions options;
    options.liveliness_lease = 10s;
    const FleetView view = reconcile({{"PC-001", ""}}, registry.snapshot(), kNow, options);

    EXPECT_EQ(view.counts.online, 1u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL — `lm/core/client_registry.hpp` not found.

- [ ] **Step 3: Write `libs/core/include/lm/core/client_registry.hpp` and `src/client_registry.cpp`**

```cpp
#pragma once

#include <map>
#include <vector>

#include "lm/core/fleet.hpp"

namespace lm::core {

/// Pure bookkeeping of clients the server has heard from. No I/O and no clock:
/// timestamps are supplied by the caller, so this is fully testable.
class ClientRegistry {
public:
    void record_announce(const HostId& host_id, Capabilities caps, TimePoint seen_at);
    void record_sample(const HostId& host_id, TimePoint seen_at);
    void record_report(const HostId& host_id, std::uint64_t applied_revision, TimePoint seen_at);
    void mark_lost(const HostId& host_id);

    /// Ordered by host id; feeds straight into reconcile().
    [[nodiscard]] std::vector<DiscoveredClient> snapshot() const;

private:
    DiscoveredClient& touch(const HostId& host_id, TimePoint seen_at);

    std::map<HostId, DiscoveredClient> clients_;
};

}  // namespace lm::core
```

`touch()` inserts the host if unknown and advances `last_seen` only forward, so an
out-of-order sample cannot make a live client look stale. `record_announce` updates
`caps` but must **not** touch `applied_revision`.

Add `src/client_registry.cpp` and `tests/test_client_registry.cpp` to
`libs/core/CMakeLists.txt`.

- [ ] **Step 4: Write `apps/server/server_controller.hpp/.cpp`**

Owns the `IServerTransport`, the `ClientRegistry`, the expected-host list, the published
bundle and the draft bundle. Responsibilities:

- Transport callbacks arrive on Fast DDS threads and are marshalled onto the GUI thread
  with `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)` before touching the
  registry. **This is mandatory** — the registry is not thread-safe by design.
- Resource samples go through `SampleCoalescer` rather than straight to the model.
- A 1 s `QTimer` calls `reconcile(expected_, registry_.snapshot(), Clock::now(), options_)`
  and hands the `FleetView` to `FleetModel::apply()`, then emits counts for the ribbon.
- `publish()` bumps `revision`, recomputes `content_hash`, persists, and calls
  `transport_->publish_bundle()`. It is only enabled when
  `content_hash(draft_) != content_hash(published_)`.
- Config load/save of expected hosts and the bundle as JSON under
  `QStandardPaths::AppConfigLocation`. A parse failure logs, surfaces the message, and
  keeps the last good bundle in memory.

- [ ] **Step 5: Write `apps/server/status_ribbon.hpp/.cpp`**

A horizontal row of clickable counters — Online, Offline, Missing, Unexpected, Stale.
`set_counts(const core::FleetCounts&)` updates the numbers, animating each through
`QVariantAnimation` so the value rolls rather than jumps. Clicking one emits
`filter_requested(std::optional<core::HostState>)`; clicking the active one again clears
the filter.

- [ ] **Step 6: Write `apps/server/fleet_window.hpp/.cpp`**

- A `QSortFilterProxyModel` over `FleetModel` with
  `setSortRole(FleetModel::SeverityRole)` and `sort(0)`, giving the
  most-urgent-first order without the model ever reordering.
- Left: the host sidebar (`QListView` or a single-column `QTableView` on the proxy) with
  a `QLineEdit` filter box wired to `setFilterFixedString` for instant filtering.
- Centre: detail for the selected host — `Sparkline`, `MeterBar`s, and its compliance
  list.
- Top: the `StatusRibbon`; its `filter_requested` signal sets a state filter on the proxy.
- A Templates tab hosting the rule editor and host→template assignments, with the
  Publish button described in Step 4.
- Geometry and splitter state saved and restored via `QSettings`.

- [ ] **Step 7: Write `apps/server/main.cpp`**

Same shape as the client's: `boost::program_options` for `--domain-id`, `--config`,
`--offline` and `--log-level`; `spdlog` sinks; `Theme::apply(app)`; construct
`ServerController` and `FleetWindow`; **show** the window (unlike the client).

- [ ] **Step 8: Write `apps/server/CMakeLists.txt`**

Mirrors the client's, with `add_executable(lab_monitor_server WIN32 ...)` and the same
link set. Add `add_subdirectory(apps/server)` inside the top-level `if(LM_BUILD_GUI)`.

- [ ] **Step 9: Build and run the tests**

```bash
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: all tests pass.

- [ ] **Step 10: Verify the vertical slice end to end**

In one terminal:

```bash
./build/windows/Debug/lab_monitor_server.exe
```

In another:

```bash
./build/windows/Debug/lab_monitor_client.exe
```

Expected: within a few seconds the server lists the client's hostname as **Unexpected**
(it is not yet in the expected list), showing live CPU, memory and disk that update
every 2 seconds. Adding that hostname to the expected list moves it to **Online**.
Stopping the client moves it to **Offline** within the 10 s liveliness lease.

**This is the vertical slice the plan set out to prove.**

- [ ] **Step 11: Commit**

```bash
git add libs/core apps/server CMakeLists.txt
git commit -m "feat: add server fleet console with client registry"
```

---

## Task 15: CI

**Files:**
- Create: `.github/workflows/ci.yml`

- [ ] **Step 1: Write the workflow**

Two jobs, matching the spec:

1. **Headless** — `windows-latest` and `ubuntu-latest`, configured with the
   `windows-headless` / `linux-headless` presets (`LM_BUILD_GUI=OFF`,
   `VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`). Builds `lm_core`, `lm_platform`,
   `lm_transport` and runs all of their tests. No Qt in the dependency graph, so this
   is the fast feedback loop on every push.
2. **Full** — the `windows` / `linux-debug` presets with the GUI, using vcpkg binary
   caching against the GitHub Actions cache backend
   (`VCPKG_BINARY_SOURCES=clear;x-gha,readwrite`).

Both jobs pin `VCPKG_ROOT` and use a CMake ≥ 3.28 from the runner image.

- [ ] **Step 2: Verify the headless preset locally first**

```bash
cmake --preset windows-headless
cmake --build --preset windows-headless
ctest --preset windows-headless
```

Expected: configures without Qt and all non-GUI tests pass.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "build: add headless and full CI workflows"
```
