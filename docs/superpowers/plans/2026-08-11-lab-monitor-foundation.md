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

#include <cstdint>
#include <stdexcept>

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
    std::uint64_t hash = 1469598103934665603ull;
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

## Remaining tasks

Tasks 7–14 follow the same structure. They are being written in sequence:

| # | Task | Deliverable |
|---|---|---|
| 7 | `lm_platform` interfaces + fakes | Probe interfaces, `HostProbes`, test doubles |
| 8 | Resource probes | Windows (`GetSystemTimes`, `GlobalMemoryStatusEx`, `GetDiskFreeSpaceEx`) and Linux (`/proc`, `statvfs`) |
| 9 | `ITransport` + `InMemoryTransport` | Typed pub/sub over core DTOs, round-trip tests |
| 10 | FastCDR codecs | Hand-written `TopicDataType` per topic + round-trip tests |
| 11 | `FastDdsTransport` | Real DDS with the QoS table from the spec; gated integration test |
| 12 | `lm_ui` widgets | Theme, status pill, sparkline, meter bar, coalescing fleet model |
| 13 | Client application | Tray icon, hidden startup, detail window |
| 14 | Server application | Status ribbon, host sidebar, live detail |
