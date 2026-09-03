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
