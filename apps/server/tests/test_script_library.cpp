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
