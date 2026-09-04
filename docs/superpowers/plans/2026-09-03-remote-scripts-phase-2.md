# Remote Scripts Phase 2 — The Share and History — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Scripts tab from a bare custom-script editor into the shipped arrangement — a folder tree of `*.ps1` read from a configured share, with a read-only preview, and a persisted history of every run with operator-driven cleanup.

**Architecture:** Two new plain units under `apps/server` keep the file work out of the widget. `script_library.{hpp,cpp}` reads a root path into a folder/script tree and reads one script's bytes; `run_store.{hpp,cpp}` converts a `ScriptRun` to and from JSON and owns the `runs/` directory. `ScriptsTab` gains a `QStackedWidget` with the library as page 0 and the existing editor as page 1, and `ServerController` gains the share root as a persisted setting plus run persistence and deletion. Nothing about dispatch changes: the share is only where a body came from.

**Tech Stack:** C++23, Qt 6 Widgets, nlohmann-json, gtest. Windows/MSVC via the VS 2026 bundled CMake.

**Spec:** `docs/superpowers/specs/2026-09-02-remote-scripts-design.md` — §8 (script picker, custom script mode, history and cleanup) and §11 (phasing).

## Global Constraints

- C++23; types `PascalCase`, functions `snake_case`, private members `trailing_`; include prefix `lm/<lib>/…`; `/W4` warnings-as-errors.
- `lm_core` depends on `nlohmann-json` and nothing else — no Qt, no DDS, no syscalls. Nothing in this plan goes in `libs/core`.
- Switches over `TargetState`, `ScriptStatus` and `HostState` stay exhaustive — no `default:`.
- Outcome colours come only from the existing `lm::ui::Theme` palette.
- Integration tests stay gated behind `LM_BUILD_INTEGRATION_TESTS`, default OFF.
- **The share is read on demand and refreshed on a button, never watched** (spec §8): a share can be slow or briefly unavailable and must never stall the console. Do not add a `QFileSystemWatcher`.
- **Only `*.ps1` is listed.** Folders are the categories; there is no metadata file and no separate taxonomy.
- **The root is a persisted setting, not a launch option** (spec §8). Do not add a CLI flag for it.
- **The body is re-read when Run is pressed, and a change since the preview stops the run** (spec §8).
- Adding a source file requires re-running **configure**, not just build:
  `"C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --preset windows`
- Build and test: `…/cmake.exe --build --preset windows-debug` then `…/ctest.exe --preset windows-debug`
- `lab_monitor_server_tests` already builds widgets, runs under a `QApplication`, and has `Theme::apply()` live, so `libs/ui/tests/pixel_probe.hpp` is available.

## File Structure

| File | Responsibility |
|---|---|
| `apps/server/script_library.hpp/.cpp` | **New.** Read a root path into `ScriptLibrary`; read one script's bytes. No widgets, no controller. |
| `apps/server/run_store.hpp/.cpp` | **New.** `ScriptRun` to/from JSON; save, load and delete files in a `runs/` directory. No widgets, no controller. |
| `apps/server/server_controller.hpp/.cpp` | Share root as a persisted setting; run persistence on completion; run deletion. |
| `apps/server/scripts_tab.hpp/.cpp` | The `QStackedWidget`, the tree, the preview, the re-read-at-Run check, the history list. |
| `apps/server/tests/test_script_library.cpp` | **New.** Tree reading against a `QTemporaryDir`. |
| `apps/server/tests/test_run_store.cpp` | **New.** JSON round-trip and directory operations. |
| `apps/server/tests/test_scripts_tab.cpp` | Extended for every UI change. |
| `apps/server/tests/test_script_dispatch.cpp` | Extended for the controller-side settings and persistence. |

---

### Task 1: Reading the share

**Files:**
- Create: `apps/server/script_library.hpp`, `apps/server/script_library.cpp`
- Create: `apps/server/tests/test_script_library.cpp`
- Modify: `apps/server/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `LibraryScript{QString relative_path; QString absolute_path;}`, `LibraryFolder{QString name; std::vector<LibraryFolder> folders; std::vector<LibraryScript> scripts;}`, `ScriptLibrary{LibraryFolder root; bool reachable; QString error; bool empty() const;}`, `ScriptLibrary read_script_library(const QString& root_path)`, `std::optional<QString> read_script_body(const QString& absolute_path)`.

- [ ] **Step 1: Write the failing test**

Create `apps/server/tests/test_script_library.cpp`:

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include "script_library.hpp"

namespace {

/// Writes `contents` to `relative` under `dir`, creating folders as needed.
void write_file(const QTemporaryDir& dir, const QString& relative, const QString& contents) {
    const QString path = dir.filePath(relative);
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << contents;
}

const LibraryFolder* folder_named(const LibraryFolder& parent, const QString& name) {
    for (const LibraryFolder& child : parent.folders) {
        if (child.name == name) {
            return &child;
        }
    }
    return nullptr;
}

}  // namespace

TEST(ScriptLibrary, ReadsPs1FilesAndIgnoresEverythingElse) {
    // Only *.ps1 is listed (spec section 8). A share is a folder somebody else
    // keeps, so it will hold readmes, logs and .bat files that are not ours.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("clear-temp.ps1"), QStringLiteral("exit 0"));
    write_file(dir, QStringLiteral("notes.txt"), QStringLiteral("not a script"));
    write_file(dir, QStringLiteral("legacy.bat"), QStringLiteral("echo no"));

    const ScriptLibrary library = read_script_library(dir.path());

    ASSERT_TRUE(library.reachable) << library.error.toStdString();
    ASSERT_EQ(library.root.scripts.size(), 1u);
    EXPECT_EQ(library.root.scripts.front().relative_path.toStdString(), "clear-temp.ps1");
}

TEST(ScriptLibrary, FoldersAreTheCategories) {
    // No metadata file and no taxonomy: the folder a script sits in is its
    // category, which is the whole reason the tree is the picker.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("Maintenance/clear-temp.ps1"), QStringLiteral("exit 0"));
    write_file(dir, QStringLiteral("Maintenance/Deep/purge.ps1"), QStringLiteral("exit 0"));
    write_file(dir, QStringLiteral("Software/install-7zip.ps1"), QStringLiteral("exit 0"));

    const ScriptLibrary library = read_script_library(dir.path());

    ASSERT_TRUE(library.reachable);
    EXPECT_TRUE(library.root.scripts.empty()) << "nothing sits at the root here";
    ASSERT_EQ(library.root.folders.size(), 2u);

    const LibraryFolder* maintenance = folder_named(library.root, QStringLiteral("Maintenance"));
    ASSERT_NE(maintenance, nullptr);
    ASSERT_EQ(maintenance->scripts.size(), 1u);
    ASSERT_EQ(maintenance->folders.size(), 1u) << "nesting is not flattened";
    ASSERT_EQ(maintenance->folders.front().scripts.size(), 1u);
    EXPECT_EQ(maintenance->folders.front().scripts.front().relative_path.toStdString(),
              "Maintenance/Deep/purge.ps1");
}

TEST(ScriptLibrary, RelativePathIsTheNameARunRecords) {
    // Two folders commonly hold an install.ps1. The relative path is what makes
    // the run history say which one was executed.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("Software/install.ps1"), QStringLiteral("exit 0"));

    const ScriptLibrary library = read_script_library(dir.path());

    ASSERT_TRUE(library.reachable);
    const LibraryFolder* software = folder_named(library.root, QStringLiteral("Software"));
    ASSERT_NE(software, nullptr);
    ASSERT_EQ(software->scripts.size(), 1u);
    EXPECT_EQ(software->scripts.front().relative_path.toStdString(), "Software/install.ps1")
        << "forward slashes, relative to the root, on every platform";
    EXPECT_TRUE(software->scripts.front().absolute_path.endsWith(QStringLiteral("install.ps1")));
}

TEST(ScriptLibrary, AnUnreachableRootSaysSoRatherThanReadingEmpty) {
    // An empty tree and an unreachable share look identical on screen, and the
    // difference is the whole of what an operator needs: one means "nobody has
    // put scripts here", the other "you are not seeing the scripts".
    const ScriptLibrary library =
        read_script_library(QStringLiteral("//no-such-host/no-such-share"));

    EXPECT_FALSE(library.reachable);
    EXPECT_FALSE(library.error.isEmpty()) << "a failure that does not say why is not actionable";
}

TEST(ScriptLibrary, AnEmptyButReachableRootIsNotAnError) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const ScriptLibrary library = read_script_library(dir.path());

    EXPECT_TRUE(library.reachable);
    EXPECT_TRUE(library.empty());
    EXPECT_TRUE(library.error.isEmpty());
}

TEST(ScriptLibrary, AnEmptyRootPathIsUnreachableRatherThanTheWorkingDirectory) {
    // QDir("") is the *current* directory, so an unconfigured share would
    // otherwise list whatever the console happened to be launched from.
    const ScriptLibrary library = read_script_library(QString());

    EXPECT_FALSE(library.reachable);
    EXPECT_FALSE(library.error.isEmpty());
}

TEST(ScriptLibrary, ReadsAScriptsBytesAndReportsOneItCannot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("a.ps1"), QStringLiteral("Write-Output 'hi'\n"));

    const auto body = read_script_body(dir.filePath(QStringLiteral("a.ps1")));
    ASSERT_TRUE(body.has_value());
    EXPECT_NE(body->indexOf(QStringLiteral("Write-Output 'hi'")), -1);

    EXPECT_FALSE(read_script_body(dir.filePath(QStringLiteral("gone.ps1"))).has_value())
        << "a missing file must be distinguishable from an empty one";
}
```

- [ ] **Step 2: Run it to verify it fails**

Run configure, then build `lab_monitor_server_tests`.
Expected: FAIL to compile — `script_library.hpp` does not exist.

- [ ] **Step 3: Write the header**

Create `apps/server/script_library.hpp`:

```cpp
#pragma once

#include <QString>

#include <optional>
#include <vector>

/// One *.ps1 found under the share root.
struct LibraryScript {
    /// Relative to the root, forward-slashed: "Maintenance/clear-temp.ps1".
    /// This is the name a run records -- the path, not the bare file name,
    /// because two folders commonly hold an install.ps1 and history has to say
    /// which one ran.
    QString relative_path;
    /// What to re-read at Run. Absolute, so a later change of root cannot
    /// silently repoint a script the operator already chose.
    QString absolute_path;
};

/// A folder in the share. Folders *are* the categories: no metadata file and no
/// separate taxonomy, so the tree on screen is the directory on disk.
struct LibraryFolder {
    QString name;  ///< empty for the root
    std::vector<LibraryFolder> folders;
    std::vector<LibraryScript> scripts;
};

/// What one read of the share produced.
///
/// `reachable` is not the same as "has scripts", and the tab shows something
/// different for each: an empty tree and an unreachable share look identical,
/// while the difference is exactly what an operator needs to know.
struct ScriptLibrary {
    LibraryFolder root;
    bool reachable = false;
    QString error;  ///< set only when !reachable

    [[nodiscard]] bool empty() const;
};

/// Reads the tree under `root_path`. Never throws: an unreachable, missing or
/// empty-string root comes back with reachable == false and error set.
///
/// Read on demand, never watched -- a share can be slow or briefly
/// unavailable, and a watcher would put that on the console's GUI thread.
[[nodiscard]] ScriptLibrary read_script_library(const QString& root_path);

/// Reads one script's bytes. nullopt when it cannot be read at all, which the
/// caller turns into a refusal to run rather than dispatching an empty body.
[[nodiscard]] std::optional<QString> read_script_body(const QString& absolute_path);
```

- [ ] **Step 4: Write the implementation**

Create `apps/server/script_library.cpp`:

```cpp
#include "script_library.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QTextStream>

#include <utility>

namespace {

/// How deep the walk goes. A share is somebody else's directory and may hold
/// anything -- a junction pointing at its own parent included -- and the
/// console must not hang on it. Eight is far past any real categorisation and
/// shallow enough that a pathological tree stays bounded.
constexpr int kMaxDepth = 8;

void read_into(LibraryFolder& folder, const QDir& dir, const QString& prefix, int depth) {
    if (depth > kMaxDepth) {
        return;
    }

    // NoSymLinks for the same reason as the depth cap: a junction on a share
    // can point anywhere, including back up this walk.
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);

    for (const QFileInfo& entry : entries) {
        const QString relative =
            prefix.isEmpty() ? entry.fileName() : prefix + QStringLiteral("/") + entry.fileName();
        if (entry.isDir()) {
            LibraryFolder child;
            child.name = entry.fileName();
            read_into(child, QDir(entry.absoluteFilePath()), relative, depth + 1);
            folder.folders.push_back(std::move(child));
        } else if (entry.suffix().compare(QStringLiteral("ps1"), Qt::CaseInsensitive) == 0) {
            folder.scripts.push_back(LibraryScript{relative, entry.absoluteFilePath()});
        }
    }
}

}  // namespace

bool ScriptLibrary::empty() const { return root.folders.empty() && root.scripts.empty(); }

ScriptLibrary read_script_library(const QString& root_path) {
    ScriptLibrary library;

    // QDir("") is the *current* directory, so without this an unconfigured
    // share would quietly list whatever the console was launched from.
    if (root_path.trimmed().isEmpty()) {
        library.error = QStringLiteral("No script share is configured.");
        return library;
    }

    const QDir dir(root_path);
    if (!dir.exists()) {
        library.error = QStringLiteral("The script share could not be reached: %1").arg(root_path);
        return library;
    }

    library.reachable = true;
    read_into(library.root, dir, QString(), 0);
    return library;
}

std::optional<QString> read_script_body(const QString& absolute_path) {
    QFile file(absolute_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    QTextStream stream(&file);
    return stream.readAll();
}
```

- [ ] **Step 5: Add to CMake and re-run configure**

In `apps/server/CMakeLists.txt` add `script_library.cpp` to the `lab_monitor_server` sources and to the `lab_monitor_server_tests` sources, and add `tests/test_script_library.cpp` to the test sources. Then run configure (a new source file needs it, not just a build).

- [ ] **Step 6: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptLibrary.*"`
Expected: PASS, 7 tests.

- [ ] **Step 7: Commit**

```bash
git add apps/server/script_library.hpp apps/server/script_library.cpp \
        apps/server/tests/test_script_library.cpp apps/server/CMakeLists.txt
git commit -m "feat: read the script share into a tree, folders as categories"
```

---

### Task 2: The share root as a persisted setting

**Files:**
- Modify: `apps/server/server_controller.hpp`, `apps/server/server_controller.cpp`
- Test: `apps/server/tests/test_script_dispatch.cpp` (already has a controller-only `Harness` with a `QTemporaryDir`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `QString ServerController::script_share_root() const`, `void ServerController::set_script_share_root(QString path)`, signal `void script_share_root_changed(QString path)`. Persisted at `<config_dir>/scripts.json` as `{"share_root": "..."}`.

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_script_dispatch.cpp` (add `#include <QSignalSpy>` if absent):

```cpp
TEST(ScriptShareRoot, PersistsAcrossARestart) {
    // A share moves, or an operator is handed a new one, far more often than
    // this console is restarted -- so it is a setting beside the bundle, not a
    // launch option (spec section 8).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString share = QStringLiteral("C:/scripts/share");

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        controller.set_script_share_root(share);
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    reopened.start();
    EXPECT_EQ(reopened.script_share_root(), share);
    reopened.stop();
}

TEST(ScriptShareRoot, AnnouncesAChangeSoTheTabCanReread) {
    Harness harness;
    QSignalSpy spy(harness.controller.get(), &ServerController::script_share_root_changed);
    ASSERT_TRUE(spy.isValid());

    harness.controller->set_script_share_root(QStringLiteral("C:/scripts"));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.front().at(0).toString(), QStringLiteral("C:/scripts"));

    harness.controller->set_script_share_root(QStringLiteral("C:/scripts"));
    EXPECT_EQ(spy.count(), 1) << "setting the same path again is not a change";
}

TEST(ScriptShareRoot, StartsEmptyOnAFreshConfigDirectory) {
    Harness harness;
    EXPECT_TRUE(harness.controller->script_share_root().isEmpty())
        << "an unconfigured share must not read as a configured one";
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptShareRoot.*"`
Expected: FAIL to compile — `script_share_root` is not a member.

- [ ] **Step 3: Declare the API**

In `apps/server/server_controller.hpp`, with the other config accessors:

```cpp
    /// Where the script picker reads from. Empty until an operator sets one.
    [[nodiscard]] QString script_share_root() const { return script_share_root_; }
    /// Persists immediately and emits script_share_root_changed(), so the tab
    /// re-reads the share without anyone having to remember to ask it to.
    void set_script_share_root(QString path);
```

with the other signals:

```cpp
    void script_share_root_changed(QString path);
```

and with the other private helpers and members:

```cpp
    void save_script_settings();
    [[nodiscard]] QString script_settings_path() const;

    QString script_share_root_;
```

- [ ] **Step 4: Implement**

In `apps/server/server_controller.cpp`, beside `bundle_path()`:

```cpp
QString ServerController::script_settings_path() const {
    return config_dir_ + QStringLiteral("/scripts.json");
}

void ServerController::set_script_share_root(QString path) {
    if (path == script_share_root_) {
        return;  // not a change, and re-reading a share for nothing costs a stall
    }
    script_share_root_ = std::move(path);
    save_script_settings();
    spdlog::info("script share root set to {}", script_share_root_.isEmpty()
                                                    ? std::string("(none)")
                                                    : script_share_root_.toStdString());
    emit script_share_root_changed(script_share_root_);
}

void ServerController::save_script_settings() {
    QDir().mkpath(config_dir_);
    nlohmann::json document;
    document["share_root"] = script_share_root_.toStdString();

    QFile file(script_settings_path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit config_error(
            QStringLiteral("Could not save the script settings to %1").arg(script_settings_path()));
        return;
    }
    const std::string text = document.dump(2);
    file.write(text.c_str(), static_cast<qint64>(text.size()));
}
```

and at the end of `load_config()`:

```cpp
    // Absent on a fresh config directory, and a missing file is not an error:
    // an unconfigured share is the starting state, not a failure.
    QFile settings_file(script_settings_path());
    if (settings_file.open(QIODevice::ReadOnly)) {
        const QByteArray text = settings_file.readAll();
        const nlohmann::json document =
            nlohmann::json::parse(text.constData(), nullptr, /*allow_exceptions=*/false);
        if (document.is_object() && document.contains("share_root") &&
            document["share_root"].is_string()) {
            script_share_root_ = QString::fromStdString(document["share_root"].get<std::string>());
        } else {
            emit config_error(QStringLiteral("The script settings in %1 could not be read; the "
                                             "share is unset until one is chosen again")
                                  .arg(script_settings_path()));
        }
    }
```

- [ ] **Step 5: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptShareRoot.*"`
Expected: PASS, 3 tests.

- [ ] **Step 6: Commit**

```bash
git add apps/server/server_controller.hpp apps/server/server_controller.cpp \
        apps/server/tests/test_script_dispatch.cpp
git commit -m "feat: the script share root is a setting, not a launch option"
```

---

### Task 3: The list is the default view

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: object names `ScriptModeStack` (`QStackedWidget`), `CustomScriptButton`, `BackToListButton`. Page 0 is the library, page 1 the editor. `ScriptsTab` gains `QStackedWidget* mode_stack_;` and `QVBoxLayout* library_page_layout_;` (the latter is where Tasks 4–5 add the tree and preview).

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_scripts_tab.cpp` (add `#include <QStackedWidget>`):

```cpp
namespace {

QStackedWidget* mode_stack(const Harness& harness) {
    return harness.window->findChild<QStackedWidget*>(QStringLiteral("ScriptModeStack"));
}

}  // namespace

TEST(ScriptsTab, OpensOnTheScriptListRatherThanTheEditor) {
    // The list is what an operator wants almost every time; the editor is the
    // escape hatch and should look like one, not an equal half of the tab.
    Harness harness;
    QStackedWidget* stack = mode_stack(harness);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0) << "the tab opened on the custom editor";
}

TEST(ScriptsTab, CustomScriptSwitchesToTheEditorAndBackReturns) {
    Harness harness;
    QStackedWidget* stack = mode_stack(harness);
    ASSERT_NE(stack, nullptr);

    button(harness, QStringLiteral("CustomScriptButton"))->click();
    EXPECT_EQ(stack->currentIndex(), 1);
    ASSERT_NE(scripts_editor(harness), nullptr) << "the editor is on the page it switched to";

    button(harness, QStringLiteral("BackToListButton"))->click();
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST(ScriptsTab, KeepsWhatWasTypedWhenSwitchingAway) {
    // The editor is a page, not a dialog: an operator who flicks to the list to
    // check a name must not come back to an empty box.
    Harness harness;
    button(harness, QStringLiteral("CustomScriptButton"))->click();
    scripts_editor(harness)->setPlainText(QStringLiteral("Write-Output 'mine'\nexit 0\n"));

    button(harness, QStringLiteral("BackToListButton"))->click();
    button(harness, QStringLiteral("CustomScriptButton"))->click();

    EXPECT_EQ(scripts_editor(harness)->toPlainText().toStdString(),
              "Write-Output 'mine'\nexit 0\n");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.OpensOnTheScriptList*"`
Expected: FAIL — `mode_stack` returns null.

- [ ] **Step 3: Build the stack**

In `scripts_tab.cpp`, where `editor_` is currently added straight to `script_layout`, build two pages instead. `ResetTemplateButton` keeps its existing construction but is added to `editor_layout` so it stays with the editor it resets.

```cpp
    mode_stack_ = new QStackedWidget(script_side);
    mode_stack_->setObjectName(QStringLiteral("ScriptModeStack"));

    // Page 0: the library. Tasks 4 and 5 fill library_page_layout_; the switch
    // button lives here from the start so the two pages work immediately.
    auto* library_page = new QWidget(mode_stack_);
    library_page_layout_ = new QVBoxLayout(library_page);
    library_page_layout_->setContentsMargins(0, 0, 0, 0);
    auto* to_custom = new QPushButton(QStringLiteral("Custom script…"), library_page);
    to_custom->setObjectName(QStringLiteral("CustomScriptButton"));
    library_page_layout_->addWidget(to_custom);
    mode_stack_->addWidget(library_page);

    // Page 1: the editor that was the whole tab in phase 1.
    auto* editor_page = new QWidget(mode_stack_);
    auto* editor_layout = new QVBoxLayout(editor_page);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    auto* back = new QPushButton(QStringLiteral("Back to script list"), editor_page);
    back->setObjectName(QStringLiteral("BackToListButton"));
    editor_layout->addWidget(back);
    editor_ = new QPlainTextEdit(editor_page);
    editor_->setObjectName(QStringLiteral("ScriptEditor"));
    editor_->setPlainText(starter_template());
    editor_layout->addWidget(editor_, 1);
    mode_stack_->addWidget(editor_page);

    mode_stack_->setCurrentIndex(0);  // the list is the default view
    script_layout->addWidget(mode_stack_, 1);

    connect(to_custom, &QPushButton::clicked, this, [this] { mode_stack_->setCurrentIndex(1); });
    connect(back, &QPushButton::clicked, this, [this] { mode_stack_->setCurrentIndex(0); });
```

Add to `scripts_tab.hpp`: forward declarations `class QStackedWidget;` and `class QVBoxLayout;`, and members `QStackedWidget* mode_stack_;`, `QVBoxLayout* library_page_layout_;`.

- [ ] **Step 4: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.*:ScriptRunView.*"`
Expected: PASS. The existing editor tests still pass — the editor still exists, on page 1.

- [ ] **Step 5: Commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp
git commit -m "feat: the script list is the default view, the editor an escape hatch"
```

---

### Task 4: The tree, the root field, Browse and Refresh

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: Task 1's `read_script_library()`/`ScriptLibrary`; Task 2's `script_share_root()`/`set_script_share_root()`/`script_share_root_changed`; Task 3's `library_page_layout_`.
- Produces: object names `ShareRootEdit` (`QLineEdit`), `BrowseShareButton`, `RefreshShareButton`, `ScriptTree` (`QTreeWidget`), `ShareMessage` (`QLabel`). Item role `ScriptsTab::kScriptIndexRole = Qt::UserRole + 3` holding the index into `scripts_`, absent on folder rows. Members `QLineEdit* share_root_edit_; QTreeWidget* script_tree_; QLabel* share_message_; std::vector<LibraryScript> scripts_;` and `void reload_library();`.

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_scripts_tab.cpp` (add `#include <QLineEdit>`, `#include <QTreeWidget>`, `#include <QTemporaryDir>`, `#include <QDir>`, `#include <QFile>`, `#include <QTextStream>`, `#include <QFileInfo>`):

```cpp
namespace {

/// Writes `contents` to `relative` under `dir`, creating folders as needed.
void write_script(const QTemporaryDir& dir, const QString& relative, const QString& contents) {
    const QString path = dir.filePath(relative);
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << contents;
}

QTreeWidget* script_tree(const Harness& harness) {
    return harness.window->findChild<QTreeWidget*>(QStringLiteral("ScriptTree"));
}

/// Depth-first search for the tree row whose text is `label`.
QTreeWidgetItem* tree_row(QTreeWidget* tree, const QString& label) {
    const auto matches =
        tree->findItems(label, Qt::MatchExactly | Qt::MatchRecursive, 0);
    return matches.isEmpty() ? nullptr : matches.front();
}

}  // namespace

TEST(ScriptsTab, ShowsTheShareAsATreeOfFoldersAndScripts) {
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("Maintenance/clear-temp.ps1"), QStringLiteral("exit 0"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    QApplication::processEvents();

    QTreeWidget* tree = script_tree(harness);
    ASSERT_NE(tree, nullptr);
    QTreeWidgetItem* folder = tree_row(tree, QStringLiteral("Maintenance"));
    ASSERT_NE(folder, nullptr) << "the folder is the category and must be a row";
    ASSERT_EQ(folder->childCount(), 1);
    EXPECT_EQ(folder->child(0)->text(0).toStdString(), "clear-temp.ps1")
        << "the row shows the file name; the relative path is the recorded name";
}

TEST(ScriptsTab, SettingTheRootThroughTheFieldPersistsIt) {
    // The field is the setting, so what it shows and what the console will read
    // next time must be the same thing.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());

    Harness harness;
    auto* edit = harness.window->findChild<QLineEdit*>(QStringLiteral("ShareRootEdit"));
    ASSERT_NE(edit, nullptr);

    edit->setText(share.path());
    edit->editingFinished();  // the field commits on focus-out or Enter
    QApplication::processEvents();

    EXPECT_EQ(harness.controller->script_share_root(), share.path());
}

TEST(ScriptsTab, ShowsTheRootTheControllerAlreadyHad) {
    // A restart must not present an empty field over a configured share.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    QApplication::processEvents();

    auto* edit = harness.window->findChild<QLineEdit*>(QStringLiteral("ShareRootEdit"));
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->text(), share.path());
}

TEST(ScriptsTab, RefreshPicksUpAScriptAddedSinceTheTreeWasRead) {
    // Read on demand, never watched (spec section 8): Refresh is the only thing
    // that makes the tree current, which is why it exists.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("first.ps1"), QStringLiteral("exit 0"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    QApplication::processEvents();
    ASSERT_NE(tree_row(script_tree(harness), QStringLiteral("first.ps1")), nullptr);
    EXPECT_EQ(tree_row(script_tree(harness), QStringLiteral("second.ps1")), nullptr);

    write_script(share, QStringLiteral("second.ps1"), QStringLiteral("exit 0"));
    button(harness, QStringLiteral("RefreshShareButton"))->click();
    QApplication::processEvents();

    EXPECT_NE(tree_row(script_tree(harness), QStringLiteral("second.ps1")), nullptr);
}

TEST(ScriptsTab, SaysTheShareIsUnreachableRatherThanShowingAnEmptyTree) {
    // An empty tree reads as "nobody has put scripts here". The operator needs
    // to know it is instead "you are not seeing the scripts".
    Harness harness;
    harness.controller->set_script_share_root(QStringLiteral("//no-such-host/no-such-share"));
    QApplication::processEvents();

    auto* message = harness.window->findChild<QLabel*>(QStringLiteral("ShareMessage"));
    ASSERT_NE(message, nullptr);
    EXPECT_FALSE(message->text().isEmpty());
    EXPECT_TRUE(message->isVisible() || !message->text().isEmpty());
}

TEST(ScriptsTab, StaysUsableForCustomScriptsWhenTheShareIsUnreachable) {
    // Spec section 8: an unreachable share must not take the tab down with it.
    Harness harness;
    harness.controller->set_script_share_root(QStringLiteral("//no-such-host/no-such-share"));
    QApplication::processEvents();

    button(harness, QStringLiteral("CustomScriptButton"))->click();
    ASSERT_NE(scripts_editor(harness), nullptr);
    EXPECT_TRUE(button(harness, QStringLiteral("CustomScriptButton"))->isEnabled());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.ShowsTheShare*"`
Expected: FAIL — `script_tree` returns null.

- [ ] **Step 3: Build the library page**

Into `library_page_layout_` (before the `CustomScriptButton` added in Task 3), add a root row, the tree and the message label:

```cpp
    auto* root_row = new QHBoxLayout();
    share_root_edit_ = new QLineEdit(library_page);
    share_root_edit_->setObjectName(QStringLiteral("ShareRootEdit"));
    share_root_edit_->setPlaceholderText(QStringLiteral("\\\\fileserver\\scripts"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), library_page);
    browse->setObjectName(QStringLiteral("BrowseShareButton"));
    auto* refresh = new QPushButton(QStringLiteral("Refresh"), library_page);
    refresh->setObjectName(QStringLiteral("RefreshShareButton"));
    root_row->addWidget(new QLabel(QStringLiteral("Share"), library_page));
    root_row->addWidget(share_root_edit_, 1);
    root_row->addWidget(browse);
    root_row->addWidget(refresh);
    library_page_layout_->addLayout(root_row);

    share_message_ = new QLabel(library_page);
    share_message_->setObjectName(QStringLiteral("ShareMessage"));
    share_message_->setWordWrap(true);
    library_page_layout_->addWidget(share_message_);

    script_tree_ = new QTreeWidget(library_page);
    script_tree_->setObjectName(QStringLiteral("ScriptTree"));
    script_tree_->setHeaderHidden(true);
    library_page_layout_->addWidget(script_tree_, 1);
```

Wiring:

```cpp
    // editingFinished rather than textChanged: re-reading a share on every
    // keystroke would hit the network for each character of a UNC path.
    connect(share_root_edit_, &QLineEdit::editingFinished, this,
            [this] { controller_->set_script_share_root(share_root_edit_->text().trimmed()); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the script share"), share_root_edit_->text());
        if (!chosen.isEmpty()) {
            share_root_edit_->setText(chosen);
            controller_->set_script_share_root(chosen);
        }
    });
    connect(refresh, &QPushButton::clicked, this, &ScriptsTab::reload_library);
    connect(controller_, &ServerController::script_share_root_changed, this,
            [this](const QString& path) {
                share_root_edit_->setText(path);
                reload_library();
            });

    share_root_edit_->setText(controller_->script_share_root());
    reload_library();
```

And the reader:

```cpp
void ScriptsTab::reload_library() {
    const ScriptLibrary library = read_script_library(controller_->script_share_root());

    script_tree_->clear();
    scripts_.clear();

    if (!library.reachable) {
        // The message is the whole point: an empty tree would claim the share
        // is empty, which is a different fact and a different action.
        share_message_->setText(library.error);
        share_message_->setVisible(true);
        return;
    }
    if (library.empty()) {
        share_message_->setText(QStringLiteral("No .ps1 scripts under %1")
                                    .arg(controller_->script_share_root()));
        share_message_->setVisible(true);
    } else {
        share_message_->clear();
        share_message_->setVisible(false);
    }

    // Folders first, then scripts, each alphabetical -- read_script_library()
    // already returns them in QDir::Name order within each kind.
    const std::function<void(const LibraryFolder&, QTreeWidgetItem*)> add =
        [&](const LibraryFolder& folder, QTreeWidgetItem* parent) {
            for (const LibraryFolder& child : folder.folders) {
                auto* row = parent == nullptr ? new QTreeWidgetItem(script_tree_)
                                              : new QTreeWidgetItem(parent);
                row->setText(0, child.name);
                add(child, row);
            }
            for (const LibraryScript& script : folder.scripts) {
                auto* row = parent == nullptr ? new QTreeWidgetItem(script_tree_)
                                              : new QTreeWidgetItem(parent);
                // The row shows the file name; the relative path is what a run
                // records, and it is reachable through kScriptIndexRole.
                row->setText(0, QFileInfo(script.relative_path).fileName());
                row->setData(0, kScriptIndexRole, static_cast<int>(scripts_.size()));
                scripts_.push_back(script);
            }
        };
    add(library.root, nullptr);
    script_tree_->expandAll();
}
```

Add `kScriptIndexRole` to the public roles in `scripts_tab.hpp`:

```cpp
    /// Index into the tab's flat list of scripts. Absent on folder rows, which
    /// is how "a script is selected" is decided -- a folder is a category and
    /// has nothing to run.
    static constexpr int kScriptIndexRole = Qt::UserRole + 3;
```

- [ ] **Step 4: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.*"`
Expected: PASS, including the six new cases.

- [ ] **Step 5: Commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp
git commit -m "feat: the script share, as a tree with a root you can point elsewhere"
```

---

### Task 5: The read-only preview

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: Task 4's `script_tree_`, `scripts_`, `kScriptIndexRole`; Task 1's `read_script_body()`.
- Produces: object name `ScriptPreview` (`QPlainTextEdit`, read-only, fixed-width). Members `QPlainTextEdit* preview_; std::optional<LibraryScript> selected_script_; QString previewed_body_;` and slot `void on_script_selection_changed();`.

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_scripts_tab.cpp`:

```cpp
TEST(ScriptsTab, ShowsTheContentOfTheSelectedScript) {
    // The operator is about to run this on a hundred machines. Seeing it first
    // is the entire reason the preview exists (spec section 8).
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("Maintenance/clear-temp.ps1"),
                 QStringLiteral("Remove-Item C:\\Temp\\* -Recurse\nexit 0\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    QApplication::processEvents();

    QTreeWidget* tree = script_tree(harness);
    QTreeWidgetItem* row = tree_row(tree, QStringLiteral("clear-temp.ps1"));
    ASSERT_NE(row, nullptr);
    tree->setCurrentItem(row);
    QApplication::processEvents();

    auto* preview = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("ScriptPreview"));
    ASSERT_NE(preview, nullptr);
    EXPECT_NE(preview->toPlainText().indexOf(QStringLiteral("Remove-Item")), -1)
        << preview->toPlainText().toStdString();
    EXPECT_TRUE(preview->isReadOnly()) << "editing here would be editing the share";
}

TEST(ScriptsTab, ClearsThePreviewWhenAFolderIsSelected) {
    // A folder is a category and has nothing to run. Leaving the last script's
    // body on screen would make it look like the folder's own content.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("Maintenance/clear-temp.ps1"),
                 QStringLiteral("Remove-Item C:\\Temp\\*\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    QApplication::processEvents();

    QTreeWidget* tree = script_tree(harness);
    tree->setCurrentItem(tree_row(tree, QStringLiteral("clear-temp.ps1")));
    QApplication::processEvents();
    tree->setCurrentItem(tree_row(tree, QStringLiteral("Maintenance")));
    QApplication::processEvents();

    auto* preview = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("ScriptPreview"));
    ASSERT_NE(preview, nullptr);
    EXPECT_TRUE(preview->toPlainText().isEmpty());
}

TEST(ScriptsTab, DisablesRunUntilAScriptIsSelected) {
    // Run with nothing chosen is a no-op that looks like a failure -- the same
    // reasoning as Run with no hosts ticked.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("exit 0"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    harness.announce("PC-001", enrolled());
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    EXPECT_FALSE(button(harness, QStringLiteral("RunButton"))->isEnabled())
        << "hosts are chosen but no script is";

    QTreeWidget* tree = script_tree(harness);
    tree->setCurrentItem(tree_row(tree, QStringLiteral("a.ps1")));
    QApplication::processEvents();

    EXPECT_TRUE(button(harness, QStringLiteral("RunButton"))->isEnabled());
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL — `ScriptPreview` does not exist.

- [ ] **Step 3: Add the preview and the selection handler**

Below the tree in `library_page_layout_`:

```cpp
    preview_ = new QPlainTextEdit(library_page);
    preview_->setObjectName(QStringLiteral("ScriptPreview"));
    preview_->setReadOnly(true);  // this is the share's content, not ours
    preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    library_page_layout_->addWidget(preview_, 1);

    connect(script_tree_, &QTreeWidget::currentItemChanged, this,
            &ScriptsTab::on_script_selection_changed);
```

```cpp
void ScriptsTab::on_script_selection_changed() {
    selected_script_.reset();
    previewed_body_.clear();
    preview_->clear();

    QTreeWidgetItem* row = script_tree_->currentItem();
    // A folder row carries no index: it is a category, and has nothing to show.
    if (row != nullptr && row->data(0, kScriptIndexRole).isValid()) {
        const int index = row->data(0, kScriptIndexRole).toInt();
        if (index >= 0 && index < static_cast<int>(scripts_.size())) {
            selected_script_ = scripts_[static_cast<std::size_t>(index)];
            const auto body = read_script_body(selected_script_->absolute_path);
            if (body.has_value()) {
                previewed_body_ = *body;
                preview_->setPlainText(previewed_body_);
            } else {
                // Named, not silent: a script that cannot be read is a thing to
                // fix on the share, and an empty pane would look like an empty
                // script that would happily "run".
                selected_script_.reset();
                preview_->setPlainText(
                    QStringLiteral("This script could not be read from the share."));
            }
        }
    }
    update_target_count();  // Run's enabled state depends on having a script too
}
```

Extend `update_target_count()`'s enable rule so Run needs both a target and something to run:

```cpp
    const bool has_body = mode_stack_->currentIndex() == 1 || selected_script_.has_value();
    run_button_->setEnabled(!checked_hosts().empty() && has_body);
```

and connect `mode_stack_`'s `currentChanged` to `update_target_count()` so switching pages re-evaluates it.

- [ ] **Step 4: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp
git commit -m "feat: see the script before it runs on a hundred machines"
```

---

### Task 6: Running from the library, re-reading at Run

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: Task 5's `selected_script_`, `previewed_body_`, `preview_`; Task 3's `mode_stack_`.
- Produces: object name `RunBlockedMessage` (`QLabel`). `on_run_clicked()` branches on `mode_stack_->currentIndex()`.

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_scripts_tab.cpp`:

```cpp
TEST(ScriptsTab, RunsTheSelectedScriptUnderItsRelativePath) {
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("Software/install.ps1"), QStringLiteral("exit 0\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    harness.announce("PC-001", enrolled());
    QApplication::processEvents();
    script_tree(harness)->setCurrentItem(
        tree_row(script_tree(harness), QStringLiteral("install.ps1")));
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    press_run(harness);

    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.script_name, "Software/install.ps1")
        << "history has to say which install.ps1 ran";
    EXPECT_EQ(run.script_body, "exit 0\n");
}

TEST(ScriptsTab, RefusesToRunAScriptThatChangedSinceThePreview) {
    // The preview's whole promise is that the operator sees what will execute.
    // A share is writable by other people while it is being read, so the file
    // is read again at Run and a difference stops the run (spec section 8).
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("exit 0\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    harness.announce("PC-001", enrolled());
    QApplication::processEvents();
    script_tree(harness)->setCurrentItem(tree_row(script_tree(harness), QStringLiteral("a.ps1")));
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    // Somebody else edits the share between the preview and the click.
    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("Remove-Item C:\\ -Recurse\n"));

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();

    EXPECT_TRUE(harness.controller->script_runs().empty()) << "nothing may be dispatched";

    auto* blocked = harness.window->findChild<QLabel*>(QStringLiteral("RunBlockedMessage"));
    ASSERT_NE(blocked, nullptr);
    EXPECT_FALSE(blocked->text().isEmpty()) << "silence would look like a broken button";

    auto* preview = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("ScriptPreview"));
    EXPECT_NE(preview->toPlainText().indexOf(QStringLiteral("Remove-Item")), -1)
        << "the operator must be shown what it changed to";
}

TEST(ScriptsTab, RunsAfterTheChangedScriptHasBeenSeenAndAccepted) {
    // The block is one-shot: having been shown the new content, pressing Run
    // again runs what is now on screen. Otherwise the script could never run.
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("exit 0\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    harness.announce("PC-001", enrolled());
    QApplication::processEvents();
    script_tree(harness)->setCurrentItem(tree_row(script_tree(harness), QStringLiteral("a.ps1")));
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("exit 1\n"));
    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();
    ASSERT_TRUE(harness.controller->script_runs().empty());

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();

    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    EXPECT_EQ(harness.controller->script_runs().back().script_body, "exit 1\n");
}

TEST(ScriptsTab, RefusesToRunAScriptThatHasVanishedFromTheShare) {
    QTemporaryDir share;
    ASSERT_TRUE(share.isValid());
    write_script(share, QStringLiteral("a.ps1"), QStringLiteral("exit 0\n"));

    Harness harness;
    harness.controller->set_script_share_root(share.path());
    harness.announce("PC-001", enrolled());
    QApplication::processEvents();
    script_tree(harness)->setCurrentItem(tree_row(script_tree(harness), QStringLiteral("a.ps1")));
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    ASSERT_TRUE(QFile::remove(share.filePath(QStringLiteral("a.ps1"))));

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();

    EXPECT_TRUE(harness.controller->script_runs().empty());
    auto* blocked = harness.window->findChild<QLabel*>(QStringLiteral("RunBlockedMessage"));
    ASSERT_NE(blocked, nullptr);
    EXPECT_FALSE(blocked->text().isEmpty());
}

TEST(ScriptsTab, StillRunsTheEditorsBodyInCustomMode) {
    // The share is only where a body came from; dispatch is identical.
    Harness harness;
    harness.announce("PC-001", enrolled());
    button(harness, QStringLiteral("CustomScriptButton"))->click();
    scripts_editor(harness)->setPlainText(QStringLiteral("Write-Output 'hi'\nexit 0\n"));
    check_hosts(harness, {"PC-001"});
    QApplication::processEvents();

    press_run(harness);

    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.script_name, "(custom script)");
    EXPECT_EQ(run.script_body, "Write-Output 'hi'\nexit 0\n");
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL — runs are started from the library with no re-read, and `RunBlockedMessage` does not exist.

- [ ] **Step 3: Add the blocked message and branch Run**

Beside the Run button:

```cpp
    run_blocked_message_ = new QLabel(host_side);
    run_blocked_message_->setObjectName(QStringLiteral("RunBlockedMessage"));
    run_blocked_message_->setWordWrap(true);
```

An inline label, deliberately, rather than a modal: a `QMessageBox` here would block the console for something the operator has to *read and compare*, and a modal cannot show them the new body behind it.

```cpp
void ScriptsTab::on_run_clicked() {
    run_blocked_message_->clear();

    std::string script_name;
    std::string script_body;

    if (mode_stack_->currentIndex() == 1) {
        script_name = kCustomScriptName.toStdString();
        script_body = editor_->toPlainText().toStdString();
    } else {
        if (!selected_script_.has_value()) {
            return;  // Run is disabled in this state; belt and braces
        }
        // Re-read at Run. The preview promised the operator this is what would
        // execute, and a share is writable by other people while they read it.
        const auto current = read_script_body(selected_script_->absolute_path);
        if (!current.has_value()) {
            run_blocked_message_->setText(
                QStringLiteral("%1 could not be read from the share. Nothing was run.")
                    .arg(selected_script_->relative_path));
            return;
        }
        if (*current != previewed_body_) {
            // Shown, not just reported: the operator has to see what it became
            // before deciding. Pressing Run again dispatches what is now shown.
            previewed_body_ = *current;
            preview_->setPlainText(previewed_body_);
            run_blocked_message_->setText(
                QStringLiteral("%1 changed on the share since you previewed it. Nothing was "
                               "run — the new content is shown; press Run again to use it.")
                    .arg(selected_script_->relative_path));
            return;
        }
        script_name = selected_script_->relative_path.toStdString();
        script_body = current->toStdString();
    }

    const QString run_id = controller_->start_script_run(script_name, script_body,
                                                         checked_hosts(), kDefaultTimeoutSeconds);
    displayed_run_id_ = run_id.toStdString();
    refresh_run_view();
}
```

- [ ] **Step 4: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.*:ScriptRunView.*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp
git commit -m "feat: run what was previewed, or refuse and say what changed"
```

---

### Task 7: A run as JSON

**Files:**
- Create: `apps/server/run_store.hpp`, `apps/server/run_store.cpp`
- Create: `apps/server/tests/test_run_store.cpp`
- Modify: `apps/server/script_run.hpp`, `apps/server/script_run.cpp` (add `target_state_from_string`)
- Modify: `apps/server/CMakeLists.txt`

**Interfaces:**
- Consumes: `ScriptRun`, `RunTarget`, `TargetState`, `to_string(TargetState)`, `lm::transport::ScriptResultMessage`, `lm::core::ScriptStatus`.
- Produces: `std::optional<TargetState> target_state_from_string(std::string_view)`; `nlohmann::json run_to_json(const ScriptRun&)`; `std::expected<ScriptRun, std::string> run_from_json(const nlohmann::json&)`; `bool save_run(const QString& runs_dir, const ScriptRun&, QString* error)`; `std::vector<ScriptRun> load_runs(const QString& runs_dir, std::vector<QString>* errors)`; `bool delete_run(const QString& runs_dir, const std::string& run_id, QString* error)`.

- [ ] **Step 1: Write the failing test**

Create `apps/server/tests/test_run_store.cpp`:

```cpp
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

#include "run_store.hpp"

namespace {

ScriptRun sample_run(std::string run_id) {
    ScriptRun run;
    run.run_id = std::move(run_id);
    run.script_name = "Software/install.ps1";
    run.script_body = "exit 0\n";
    run.issued_at = std::chrono::system_clock::now();
    run.timeout_seconds = 120;

    RunTarget completed;
    completed.host_id = "PC-001";
    completed.state = TargetState::Completed;
    lm::transport::ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = run.run_id;
    result.status = lm::core::ScriptStatus::Completed;
    result.exit_code = 0;
    result.stdout_text = "done";
    result.duration_ms = 240;
    completed.result = result;
    run.targets.push_back(completed);

    RunTarget refused;
    refused.host_id = "PC-002";
    refused.state = TargetState::Refused;
    refused.detail = "not enrolled for script execution";
    run.targets.push_back(refused);

    return run;
}

}  // namespace

TEST(RunStore, RoundTripsEveryFieldARunViewReadsBack) {
    // A field the writer forgets is invisible on screen -- the history just
    // looks empty -- and a run is written once and never rewritten, so a loss
    // here is permanent.
    const ScriptRun original = sample_run("run-1");

    const auto reloaded = run_from_json(run_to_json(original));
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error();

    EXPECT_EQ(reloaded->run_id, original.run_id);
    EXPECT_EQ(reloaded->script_name, original.script_name);
    EXPECT_EQ(reloaded->script_body, original.script_body);
    EXPECT_EQ(reloaded->timeout_seconds, original.timeout_seconds);
    ASSERT_EQ(reloaded->targets.size(), 2u);

    EXPECT_EQ(reloaded->targets[0].host_id, "PC-001");
    EXPECT_EQ(reloaded->targets[0].state, TargetState::Completed);
    ASSERT_TRUE(reloaded->targets[0].result.has_value());
    EXPECT_EQ(reloaded->targets[0].result->exit_code, 0);
    EXPECT_EQ(reloaded->targets[0].result->stdout_text, "done");
    EXPECT_EQ(reloaded->targets[0].result->duration_ms, 240u);

    EXPECT_EQ(reloaded->targets[1].state, TargetState::Refused);
    EXPECT_EQ(reloaded->targets[1].detail, "not enrolled for script execution");
    EXPECT_FALSE(reloaded->targets[1].result.has_value())
        << "a refused target never had a result and must not gain an empty one";
}

TEST(RunStore, KeepsIssuedAtToTheSecond) {
    // "Delete runs older than" compares against this, so a lost timestamp is a
    // run that cleanup can never reach.
    const ScriptRun original = sample_run("run-2");
    const auto reloaded = run_from_json(run_to_json(original));
    ASSERT_TRUE(reloaded.has_value());

    const auto original_s =
        std::chrono::duration_cast<std::chrono::seconds>(original.issued_at.time_since_epoch());
    const auto reloaded_s =
        std::chrono::duration_cast<std::chrono::seconds>(reloaded->issued_at.time_since_epoch());
    EXPECT_EQ(original_s.count(), reloaded_s.count());
}

TEST(RunStore, RejectsADocumentThatIsNotARunRatherThanInventingOne) {
    EXPECT_FALSE(run_from_json(nlohmann::json::parse("[]")).has_value());
    EXPECT_FALSE(run_from_json(nlohmann::json::parse(R"({"script_name":"a"})")).has_value())
        << "no run_id means nothing can be looked up or deleted";
}

TEST(RunStore, SavesOneFilePerRunAndLoadsThemAllBack) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;

    ASSERT_TRUE(save_run(dir.path(), sample_run("run-a"), &error)) << error.toStdString();
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-b"), &error)) << error.toStdString();

    std::vector<QString> errors;
    std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);

    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(runs.size(), 2u);
    std::ranges::sort(runs, {}, &ScriptRun::run_id);
    EXPECT_EQ(runs[0].run_id, "run-a");
    EXPECT_EQ(runs[1].run_id, "run-b");
}

TEST(RunStore, ACorruptFileCostsOneRunRatherThanAllOfThem) {
    // The whole reason for one file per run (spec section 8).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(save_run(dir.path(), sample_run("good"), &error));

    QFile bad(QDir(dir.path()).filePath(QStringLiteral("corrupt.json")));
    ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
    bad.write("{ this is not json");
    bad.close();

    std::vector<QString> errors;
    const std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs.front().run_id, "good");
    EXPECT_EQ(errors.size(), 1u) << "the loss is reported, not silent";
}

TEST(RunStore, DeletesOneRunsFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-a"), &error));
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-b"), &error));

    ASSERT_TRUE(delete_run(dir.path(), "run-a", &error)) << error.toStdString();

    std::vector<QString> errors;
    const std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs.front().run_id, "run-b");
}

TEST(RunStore, DeletingARunThatIsNotThereIsAFailureThatSaysSo) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(delete_run(dir.path(), "never-existed", &error));
    EXPECT_FALSE(error.isEmpty());
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL to compile — `run_store.hpp` does not exist.

- [ ] **Step 3: Add `target_state_from_string`**

In `apps/server/script_run.hpp`, beside `to_string(TargetState)`:

```cpp
/// The inverse of to_string(TargetState). nullopt for anything else, so a
/// hand-edited or future file is rejected rather than silently read as Pending.
[[nodiscard]] std::optional<TargetState> target_state_from_string(std::string_view text);
```

Implement it in `script_run.cpp` as an exact match against the same six strings `to_string` produces, in one place so the two cannot drift.

- [ ] **Step 4: Write `run_store.hpp`**

```cpp
#pragma once

#include <QString>

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "script_run.hpp"

/// A run as stored. One file per run, written once when the run finishes and
/// never mutated: deletion is a file delete, and a corrupt file costs one run
/// rather than the whole history.
[[nodiscard]] nlohmann::json run_to_json(const ScriptRun& run);

/// The inverse. An error string rather than a partially-filled run, because a
/// half-read run in the history is worse than a named gap.
[[nodiscard]] std::expected<ScriptRun, std::string> run_from_json(const nlohmann::json& document);

/// Writes `<runs_dir>/<run_id>.json`, creating the directory. False on failure
/// with `error` set when it is not null.
bool save_run(const QString& runs_dir, const ScriptRun& run, QString* error);

/// Every readable run in the directory, in no particular order. Unreadable
/// files are skipped and appended to `errors` when it is not null -- one bad
/// file must not cost the rest.
[[nodiscard]] std::vector<ScriptRun> load_runs(const QString& runs_dir,
                                               std::vector<QString>* errors);

/// Removes one run's file. False when it was not there.
bool delete_run(const QString& runs_dir, const std::string& run_id, QString* error);
```

- [ ] **Step 5: Write `run_store.cpp`**

`run_to_json` writes `run_id`, `script_name`, `script_body`, `issued_at` (seconds since epoch as an integer), `timeout_seconds`, and `targets` as an array of `{host_id, state (to_string), detail, result?}`. `result` carries `status` (via the existing `lm::core::to_string(ScriptStatus)` and its inverse — add one if absent, same exact-match rule as `target_state_from_string`), `refusal_reason`, `exit_code`, `has_reported`, `reported_ok`, `reported_message`, `stdout_text`, `stderr_text`, `duration_ms`.

`run_from_json` validates `run_id` is a non-empty string and `targets` is an array before touching anything, and returns `std::unexpected` naming the first problem. File names are `<run_id>.json`; `run_id` is generated by the server (`20260903-103053-844-204513d4`) so it is already filesystem-safe, but reject a `run_id` containing `/`, `\` or `..` in `save_run` and `delete_run` rather than trusting that.

- [ ] **Step 6: CMake, configure, run**

Add `run_store.cpp` to both targets and `tests/test_run_store.cpp` to the test sources, re-run configure, build, then
`build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="RunStore.*"`
Expected: PASS, 7 tests.

- [ ] **Step 7: Commit**

```bash
git add apps/server/run_store.hpp apps/server/run_store.cpp apps/server/script_run.hpp \
        apps/server/script_run.cpp apps/server/tests/test_run_store.cpp apps/server/CMakeLists.txt
git commit -m "feat: a run, as one file that outlives the console"
```

---

### Task 8: Persisting runs

**Files:**
- Modify: `apps/server/server_controller.hpp`, `apps/server/server_controller.cpp`
- Test: `apps/server/tests/test_script_dispatch.cpp`

**Interfaces:**
- Consumes: Task 7's `save_run`, `load_runs`, `delete_run`.
- Produces: `QString ServerController::runs_dir() const`; signal `void script_runs_changed()`; `bool delete_script_run(const std::string& run_id)`; `std::size_t delete_script_runs_before(std::chrono::system_clock::time_point cutoff)`. Runs are loaded in `load_config()` and saved once, when a run first reaches `is_finished()`.

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_script_dispatch.cpp`:

```cpp
TEST(RunHistory, KeepsAFinishedRunAcrossARestart) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string run_id;

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        const auto client = make_in_memory_client(bus);
        ClientAnnounce announce;
        announce.host_id = "PC-001";
        announce.capabilities = enrolled().raw();
        client->publish_announce(announce);
        controller.add_expected_host("PC-001", "");
        QApplication::processEvents();

        run_id = controller.start_script_run("a.ps1", "exit 0", {"PC-001"}, 60).toStdString();
        ScriptResultMessage result;
        result.host_id = "PC-001";
        result.run_id = run_id;
        result.status = ScriptStatus::Completed;
        client->publish_script_result(result);
        QApplication::processEvents();
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    reopened.start();

    ASSERT_EQ(reopened.script_runs().size(), 1u) << "the audit trail did not survive";
    EXPECT_EQ(reopened.script_runs().front().run_id, run_id);
    EXPECT_EQ(reopened.script_runs().front().targets.front().state, TargetState::Completed);
    reopened.stop();
}

TEST(RunHistory, DeletesOneRunAndSaysItChanged) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_runs_changed);
    ASSERT_TRUE(spy.isValid());

    EXPECT_TRUE(harness.controller->delete_script_run(run_id.toStdString()));
    EXPECT_TRUE(harness.controller->script_runs().empty());
    EXPECT_GT(spy.count(), 0) << "the history view has no other way to know";
}

TEST(RunHistory, DeletesOnlyRunsOlderThanTheCutoff) {
    // No automatic pruning anywhere (spec section 8): this runs when an
    // operator asks, and takes exactly what they asked for.
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString old_run =
        harness.controller->start_script_run("old.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", old_run.toStdString(), ScriptStatus::Completed);

    const auto cutoff = std::chrono::system_clock::now() + std::chrono::hours(1);
    const QString recent =
        harness.controller->start_script_run("new.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", recent.toStdString(), ScriptStatus::Completed);

    EXPECT_EQ(harness.controller->delete_script_runs_before(cutoff), 2u)
        << "both are older than an hour from now";
    EXPECT_TRUE(harness.controller->script_runs().empty());

    EXPECT_EQ(harness.controller->delete_script_runs_before(
                  std::chrono::system_clock::now() - std::chrono::hours(24)),
              0u)
        << "nothing is older than yesterday, and nothing may be taken";
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL to compile — `script_runs_changed`, `delete_script_run` and `delete_script_runs_before` do not exist.

- [ ] **Step 3: Implement**

```cpp
QString ServerController::runs_dir() const { return config_dir_ + QStringLiteral("/runs"); }
```

In `on_script_result()` and `on_run_deadline()`, after the run has been updated, save it exactly once:

```cpp
    // Written once, when the run reaches a state it will never leave. A run
    // still waiting is not an audit record yet, and rewriting a file per result
    // would turn a hundred-host run into a hundred writes.
    if (run->is_finished() && !saved_runs_.contains(run->run_id)) {
        QString error;
        if (save_run(runs_dir(), *run, &error)) {
            saved_runs_.insert(run->run_id);
        } else {
            emit config_error(error);
        }
    }
```

with `std::set<std::string> saved_runs_;` as a member. In `load_config()`:

```cpp
    std::vector<QString> run_errors;
    script_runs_ = load_runs(runs_dir(), &run_errors);
    // Loaded runs are finished by construction, so nothing here may be saved
    // again or given a deadline.
    for (const ScriptRun& run : script_runs_) {
        saved_runs_.insert(run.run_id);
    }
    std::ranges::sort(script_runs_, {}, &ScriptRun::issued_at);
    for (const QString& message : run_errors) {
        emit config_error(message);
    }
```

Deletion:

```cpp
bool ServerController::delete_script_run(const std::string& run_id) {
    QString error;
    const bool removed = delete_run(runs_dir(), run_id, &error);
    if (!removed) {
        emit config_error(error);
    }
    // The in-memory entry goes either way: an operator who asked for it gone
    // and still sees it will simply ask again, and the file is already absent.
    std::erase_if(script_runs_, [&](const ScriptRun& run) { return run.run_id == run_id; });
    saved_runs_.erase(run_id);
    spdlog::info("script run {} deleted", run_id);
    emit script_runs_changed();
    return removed;
}

std::size_t ServerController::delete_script_runs_before(
    std::chrono::system_clock::time_point cutoff) {
    std::vector<std::string> doomed;
    for (const ScriptRun& run : script_runs_) {
        if (run.issued_at < cutoff) {
            doomed.push_back(run.run_id);
        }
    }
    for (const std::string& run_id : doomed) {
        QString error;
        if (!delete_run(runs_dir(), run_id, &error)) {
            emit config_error(error);
        }
        saved_runs_.erase(run_id);
    }
    std::erase_if(script_runs_, [&](const ScriptRun& run) { return run.issued_at < cutoff; });
    spdlog::info("deleted {} script run(s) older than the chosen date", doomed.size());
    emit script_runs_changed();
    return doomed.size();
}
```

- [ ] **Step 4: Run the tests**

Run: `build/windows/bin/Debug/lab_monitor_server_tests.exe --gtest_filter="RunHistory.*:ScriptDispatch.*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add apps/server/server_controller.hpp apps/server/server_controller.cpp \
        apps/server/tests/test_script_dispatch.cpp
git commit -m "feat: runs outlive the console, one file each"
```

---

### Task 9: The history list, and Delete

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: Task 8's `script_runs()`, `script_runs_changed`, `delete_script_run`.
- Produces: object names `RunHistoryList` (`QListWidget`), `DeleteRunButton`. Item role `ScriptsTab::kRunIdRole = Qt::UserRole + 4`. Slot `void rebuild_run_history();`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(ScriptsTab, ListsPastRunsNewestFirst) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString first =
        harness.controller->start_script_run("first.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", first.toStdString(), ScriptStatus::Completed);
    const QString second =
        harness.controller->start_script_run("second.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", second.toStdString(), ScriptStatus::Completed);
    QApplication::processEvents();

    auto* history = harness.window->findChild<QListWidget*>(QStringLiteral("RunHistoryList"));
    ASSERT_NE(history, nullptr);
    ASSERT_EQ(history->count(), 2);
    EXPECT_EQ(history->item(0)->data(ScriptsTab::kRunIdRole).toString(), second)
        << "the run somebody just made is the one they are looking for";
    EXPECT_NE(history->item(0)->text().indexOf(QStringLiteral("second.ps1")), -1)
        << history->item(0)->text().toStdString();
}

TEST(ScriptsTab, SelectingAPastRunShowsItInTheRunView) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString first =
        harness.controller->start_script_run("first.ps1", "exit 0", {"PC-001"}, 60);
    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = first.toStdString();
    result.status = ScriptStatus::Completed;
    result.stdout_text = "output of the first run";
    harness.publish_result_message(result);
    harness.controller->start_script_run("second.ps1", "exit 0", {"PC-001"}, 60);
    QApplication::processEvents();

    auto* history = harness.window->findChild<QListWidget*>(QStringLiteral("RunHistoryList"));
    ASSERT_NE(history, nullptr);
    for (int row = 0; row < history->count(); ++row) {
        if (history->item(row)->data(ScriptsTab::kRunIdRole).toString() == first) {
            history->setCurrentRow(row);
        }
    }
    QApplication::processEvents();
    run_targets(harness)->selectRow(0);
    QApplication::processEvents();

    auto* output = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("RunOutput"));
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output->toPlainText().indexOf(QStringLiteral("output of the first run")), -1)
        << output->toPlainText().toStdString();
}

TEST(ScriptsTab, DeleteRemovesTheSelectedRunFromTheHistory) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    QApplication::processEvents();

    auto* history = harness.window->findChild<QListWidget*>(QStringLiteral("RunHistoryList"));
    ASSERT_NE(history, nullptr);
    ASSERT_EQ(history->count(), 1);
    history->setCurrentRow(0);

    button(harness, QStringLiteral("DeleteRunButton"))->click();
    QApplication::processEvents();

    EXPECT_EQ(history->count(), 0);
    EXPECT_TRUE(harness.controller->script_runs().empty());
}

TEST(ScriptsTab, DisablesDeleteUntilARunIsSelected) {
    Harness harness;
    EXPECT_FALSE(button(harness, QStringLiteral("DeleteRunButton"))->isEnabled());
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL — `RunHistoryList` does not exist.

- [ ] **Step 3: Build the history list**

A `QListWidget` beside the run view, each row reading `<script name> — <local time> — <tally>`, built newest-first from `controller_->script_runs()` (which `load_config()` sorts oldest-first, so iterate it in reverse). Selecting a row sets `displayed_run_id_` and calls `refresh_run_view()`. `DeleteRunButton` calls `controller_->delete_script_run(...)` for the selected row's `kRunIdRole`, and is enabled only while a row is selected. Connect `ServerController::script_runs_changed` to `rebuild_run_history()`.

`rebuild_run_history()` must **not** be called from inside a slot of the widget it rebuilds — the same trap the Templates tab's assignment column documents. Deleting is safe because the button is the sender, not a row.

- [ ] **Step 4: Run the tests, then commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp
git commit -m "feat: past runs are a list you can open and delete"
```

---

### Task 10: Delete runs older than…

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: Task 8's `delete_script_runs_before`; Task 9's `rebuild_run_history()`.
- Produces: object names `DeleteOlderDate` (`QDateEdit`), `DeleteOlderButton`, `CleanupMessage` (`QLabel`).

- [ ] **Step 1: Write the failing test**

Add `#include <QDateEdit>` and `#include <QDate>` to `apps/server/tests/test_scripts_tab.cpp`.

```cpp
TEST(ScriptsTab, DeletesRunsOlderThanTheChosenDate) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    QApplication::processEvents();

    auto* date = harness.window->findChild<QDateEdit*>(QStringLiteral("DeleteOlderDate"));
    ASSERT_NE(date, nullptr);
    date->setDate(QDate::currentDate().addDays(1));  // everything is older than tomorrow

    button(harness, QStringLiteral("DeleteOlderButton"))->click();
    QApplication::processEvents();

    EXPECT_TRUE(harness.controller->script_runs().empty());
    auto* message = harness.window->findChild<QLabel*>(QStringLiteral("CleanupMessage"));
    ASSERT_NE(message, nullptr);
    EXPECT_NE(message->text().indexOf(QStringLiteral("1")), -1)
        << "the operator is told how much was taken: " << message->text().toStdString();
}

TEST(ScriptsTab, KeepsRunsNewerThanTheChosenDate) {
    // Silently discarding an audit trail is worse than a large directory
    // (spec section 8), so this must take exactly what was asked for.
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    QApplication::processEvents();

    auto* date = harness.window->findChild<QDateEdit*>(QStringLiteral("DeleteOlderDate"));
    ASSERT_NE(date, nullptr);
    date->setDate(QDate::currentDate().addDays(-7));

    button(harness, QStringLiteral("DeleteOlderButton"))->click();
    QApplication::processEvents();

    EXPECT_EQ(harness.controller->script_runs().size(), 1u) << "a run from today was taken";
}
```

- [ ] **Step 2: Run it to verify it fails**

Expected: FAIL — `DeleteOlderDate` does not exist.

- [ ] **Step 3: Implement**

A `QDateEdit` defaulting to 30 days ago with a calendar popup, a **Delete runs older than…** button, and a `CleanupMessage` label reporting `Deleted N run(s).` The cutoff is the start of the chosen day in local time, converted to `std::chrono::system_clock::time_point`:

```cpp
    const QDateTime cutoff_local(date_->date(), QTime(0, 0), QTimeZone::LocalTime);
    const auto cutoff = std::chrono::system_clock::from_time_t(
        static_cast<std::time_t>(cutoff_local.toSecsSinceEpoch()));
    const std::size_t removed = controller_->delete_script_runs_before(cutoff);
    cleanup_message_->setText(QStringLiteral("Deleted %1 run(s).").arg(removed));
```

There is deliberately no confirmation dialog for a cutoff that matches nothing — the message saying `Deleted 0 run(s).` is the feedback, and a modal per click on an explicit, operator-driven action is noise.

- [ ] **Step 4: Run the full suite**

Run: `…/ctest.exe --preset windows-debug`
Expected: 100% passed.

- [ ] **Step 5: Update CLAUDE.md**

Add to the Scripts section: the share is read on demand and never watched; the root is a persisted setting with no CLI equivalent; the body is re-read at Run and a change blocks the run; runs are one file each under `<config>/runs/`, written once when finished, and cleanup is explicit.

- [ ] **Step 6: Commit**

```bash
git add apps/server/scripts_tab.hpp apps/server/scripts_tab.cpp \
        apps/server/tests/test_scripts_tab.cpp CLAUDE.md
git commit -m "feat: cleanup an operator asks for, never one that happens quietly"
```

---

## Self-review notes

**Spec coverage.** §8 script picker → Tasks 1, 4, 5. §8 custom script mode → Task 3 (list default, both switch controls; the starter template and dispatch already exist from phase 1). §8 unreachable share → Tasks 1, 4. §8 re-read at Run → Task 6. §8 history and cleanup → Tasks 7, 8, 9, 10. §11 phasing → the whole plan is piece 2. Host selection and the live run view are phase 1 and unchanged.

**Known gaps, deliberately.** The share is read on the GUI thread: a genuinely dead UNC path can block on `QDir::exists()` for the OS's timeout. Read-on-demand keeps that to the moment somebody presses Refresh or changes the root rather than every few seconds, which is what the spec's "never watched" rule buys. Moving the read to a worker is the obvious follow-up if a lab's share is slow, and it needs no interface change here — `read_script_library()` takes a path and returns a value, with no Qt widget or controller in its signature, precisely so it can be called from another thread later.

**Ordering.** Tasks 4–6 depend on Task 3's `library_page_layout_` and on Task 1. Task 9 depends on Task 8's signal. Task 10 depends on Task 9's rebuild. Tasks 7 and 8 are independent of 3–6 and could run in parallel with them if that were ever useful.

