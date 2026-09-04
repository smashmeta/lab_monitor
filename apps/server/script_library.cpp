#include "script_library.hpp"

#include <QDir>
#include <QElapsedTimer>
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

/// How long the whole walk may take before it stops and returns what it has.
///
/// The depth cap bounds a pathological *shape*; nothing bounded the sheer
/// *size* of a share, and this read happens on the GUI thread -- from the
/// tab's constructor and again when a persisted root loads, i.e. before the
/// window is shown. A root of `C:\` therefore used to freeze the console on
/// every single launch, with no cancel and no way to reach the field that
/// would fix the setting: one bad value locked the operator out permanently.
/// Two seconds is far past a real script share and short enough that the
/// worst case is a stutter rather than a lock-out.
///
/// This is a bound, not the fix; moving the walk off the GUI thread is -- see
/// the Known gaps note in CLAUDE.md.
constexpr qint64 kWalkBudgetMs = 2000;

/// The largest *.ps1 that will be read into the preview and, from there, onto
/// the wire. A megabyte is orders of magnitude past any real script.
constexpr qint64 kMaxScriptBytes = 1024 * 1024;

void read_into(LibraryFolder& folder, const QDir& dir, const QString& prefix, int depth,
               const QElapsedTimer& clock, bool& truncated) {
    if (depth > kMaxDepth) {
        return;
    }
    if (clock.hasExpired(kWalkBudgetMs)) {
        truncated = true;
        return;
    }

    // NoSymLinks excludes genuine symlinks, but on Windows an NTFS junction is
    // a different thing: QFileInfo::isSymLink() is false for one and
    // isJunction() true, and entryInfoList(NoSymLinks) still lists it. A
    // junction can point anywhere, including back up this walk, so it is
    // skipped explicitly below. The depth cap is the backstop for anything
    // neither catches.
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);

    for (const QFileInfo& entry : entries) {
        // Checked per entry, not just per directory: one directory holding a
        // hundred thousand files is as effective a freeze as a deep tree, and
        // entryInfoList() has already been paid for by the time we get here.
        if (clock.hasExpired(kWalkBudgetMs)) {
            truncated = true;
            return;
        }
        if (entry.isSymLink() || entry.isJunction()) {
            continue;
        }
        const QString relative =
            prefix.isEmpty() ? entry.fileName() : prefix + QStringLiteral("/") + entry.fileName();
        if (entry.isDir()) {
            LibraryFolder child;
            child.name = entry.fileName();
            read_into(child, QDir(entry.absoluteFilePath()), relative, depth + 1, clock,
                      truncated);
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
    // Started here rather than around the whole function: dir.exists() on a
    // dead UNC path blocks in SMB/DNS resolution and no budget kept in this
    // process can shorten that, so the clock measures what the walk itself
    // costs and nothing it cannot control.
    QElapsedTimer clock;
    clock.start();
    read_into(library.root, dir, QString(), 0, clock, library.truncated);
    return library;
}

std::optional<QString> read_script_body(const QString& absolute_path) {
    QFile file(absolute_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    if (file.size() > kMaxScriptBytes) {
        // Deliberately the same "could not be read" outcome as a missing file,
        // so every caller's existing handling covers it: the preview says so
        // and Run refuses. Reading it and showing the first megabyte would be
        // worse than refusing -- the preview's whole promise is that what is on
        // screen is what will execute.
        return std::nullopt;
    }
    QTextStream stream(&file);
    return stream.readAll();
}
