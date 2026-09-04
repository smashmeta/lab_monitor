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
    /// The walk ran out of its time budget and stopped, so `root` holds what
    /// was found up to that point and not the whole share. Separate from
    /// `reachable` because a partial listing is neither an unreachable share
    /// nor a complete one, and presenting a truncated tree as complete is the
    /// one thing that must not happen: an operator would read a missing
    /// script as a script that is not there.
    bool truncated = false;

    [[nodiscard]] bool empty() const;
};

/// Reads the tree under `root_path`. Never throws: an unreachable, missing or
/// empty-string root comes back with reachable == false and error set.
///
/// Read on demand, never watched -- a share can be slow or briefly
/// unavailable, and a watcher would put that on the console's GUI thread.
///
/// **Bounded in time.** This runs on the GUI thread, from the tab's
/// constructor and again when a persisted root loads at startup, so an
/// unbounded walk of a root somebody pointed at `C:\` would freeze the console
/// on every launch -- with the field that would fix the setting inside the
/// frozen window. The walk therefore gives up after a couple of seconds and
/// returns what it has with `truncated` set. It is still depth-capped and
/// still skips symlinks and junctions; the budget is the backstop for sheer
/// breadth, which nothing else here bounds.
[[nodiscard]] ScriptLibrary read_script_library(const QString& root_path);

/// Reads one script's bytes. nullopt when it cannot be read at all, which the
/// caller turns into a refusal to run rather than dispatching an empty body.
///
/// A file past a generous size cap (far larger than any real *.ps1) comes back
/// the same way. Merely clicking a row in the tree reads the file whole into
/// memory and into a QPlainTextEdit, and the share is a folder other people
/// write, so without a cap a selection alone is enough to exhaust the console.
/// nullopt rather than a truncated body deliberately: half a script in the
/// preview is a promise about what would run that nothing can keep.
[[nodiscard]] std::optional<QString> read_script_body(const QString& absolute_path);
