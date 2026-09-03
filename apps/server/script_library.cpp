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
