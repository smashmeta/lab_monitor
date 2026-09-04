#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
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

#ifdef _WIN32
/// Counts `LibraryScript` entries anywhere in the tree whose file name
/// matches -- a junction the walk failed to skip would make this 2 instead
/// of 1, under two different folder names.
int count_scripts_named(const LibraryFolder& folder, const QString& file_name) {
    int count = 0;
    for (const LibraryScript& script : folder.scripts) {
        if (QFileInfo(script.relative_path).fileName() == file_name) {
            ++count;
        }
    }
    for (const LibraryFolder& child : folder.folders) {
        count += count_scripts_named(child, file_name);
    }
    return count;
}
#endif

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
    //
    // Deliberately a local path rather than a bogus UNC one: a nonexistent host
    // name sends this through SMB name resolution, which costs on the order of
    // a second and varies with the machine's resolver and firewall policy --
    // slow and a latent flake. A path under a real QTemporaryDir that was never
    // created exercises the identical !dir.exists() branch this function
    // actually takes, in under a millisecond.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const ScriptLibrary library =
        read_script_library(dir.filePath(QStringLiteral("does-not-exist")));

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

TEST(ScriptLibrary, AWhitespaceOnlyRootPathIsUnreachableToo) {
    // The guard is `.trimmed().isEmpty()`, not `.isEmpty()`. Pin the trimmed
    // half separately: a regression to a plain isEmpty() check would let " "
    // through to QDir(" ") and nothing else in this suite would catch it.
    const ScriptLibrary library = read_script_library(QStringLiteral("   "));

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

#ifdef _WIN32
TEST(ScriptLibrary, RefusesToReadAFileFarLargerThanAnyScript) {
    // Merely clicking a row in the tree reads the file whole into memory and
    // into a QPlainTextEdit, and the share is a folder other people write --
    // so without a cap, a selection alone is enough to exhaust the console, no
    // Run required. The same unbounded string would then become
    // ScriptCommand::script_body and go out to every target.
    //
    // nullopt, the same outcome a missing file already produces, so the tab's
    // existing "could not be read" handling covers it -- and deliberately not
    // a truncated body, since the preview's whole promise is that what is on
    // screen is what would execute.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString big = dir.filePath(QStringLiteral("huge.ps1"));
    {
        QFile file(big);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray block(64 * 1024, 'x');
        for (int written = 0; written < 24; ++written) {  // 1.5 MB, past the cap
            ASSERT_EQ(file.write(block), static_cast<qint64>(block.size()));
        }
    }
    EXPECT_FALSE(read_script_body(big).has_value());

    // And an ordinary script still reads, so the cap is not simply refusing
    // everything -- a version that returned nullopt unconditionally would pass
    // the assertion above on its own.
    write_file(dir, QStringLiteral("small.ps1"), QStringLiteral("exit 0\n"));
    EXPECT_TRUE(read_script_body(dir.filePath(QStringLiteral("small.ps1"))).has_value());
}

TEST(ScriptLibrary, AnOrdinaryShareIsNotReportedAsPartial) {
    // The walk's time budget must not fire on a share that read in
    // microseconds. Worth its own case because the failure is silent and
    // total: QElapsedTimer::hasExpired() answers true for any timeout on a
    // timer nobody started, which would mark every listing partial -- and the
    // partial message takes precedence over "No .ps1 scripts under X", so an
    // empty share would stop being explainable too.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("Maintenance/clear-temp.ps1"), QStringLiteral("exit 0"));

    const ScriptLibrary library = read_script_library(dir.path());

    EXPECT_TRUE(library.reachable);
    EXPECT_FALSE(library.empty());
    EXPECT_FALSE(library.truncated)
        << "a share that read instantly must be presented as the whole share";
}

TEST(ScriptLibrary, DoesNotDescendIntoAJunction) {
    // NoSymLinks does not exclude an NTFS junction: QFileInfo::isSymLink() is
    // false and isJunction() true for one, and entryInfoList(NoSymLinks)
    // still lists it -- read_into() has to skip isJunction() explicitly. This
    // is the only test that can see that check regress: drop it and the
    // picker quietly shows every script under a junction's target twice,
    // under two folder names, while the rest of this suite stays green.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    write_file(dir, QStringLiteral("real_target/target.ps1"), QStringLiteral("exit 0"));

    const QString target = dir.filePath(QStringLiteral("real_target"));
    const QString link = dir.filePath(QStringLiteral("link_to_target"));

    // Junctions need no elevation (unlike symlinks), so this runs from an
    // ordinary, unprivileged test invocation -- no admin prompt, no special
    // CI setup.
    QProcess mklink;
    mklink.start(QStringLiteral("cmd.exe"),
                 {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                  QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    if (!mklink.waitForFinished(5000) || mklink.exitCode() != 0) {
        GTEST_SKIP() << "could not create a junction, so this environment cannot exercise the "
                        "case: "
                     << mklink.readAllStandardError().toStdString();
    }

    const ScriptLibrary library = read_script_library(dir.path());
    ASSERT_TRUE(library.reachable);

    EXPECT_EQ(count_scripts_named(library.root, QStringLiteral("target.ps1")), 1)
        << "target.ps1 must appear once, not once per name the junction makes it reachable under";
    EXPECT_EQ(folder_named(library.root, QStringLiteral("link_to_target")), nullptr)
        << "the junction itself must not be descended into";

    // Remove the link, not what it points at: QDir::rmdir on the junction
    // path removes the reparse point itself, the same way Explorer's Delete
    // on a junction does not touch its target.
    EXPECT_TRUE(QDir().rmdir(link));
    EXPECT_TRUE(QFileInfo(dir.filePath(QStringLiteral("real_target/target.ps1"))).exists())
        << "removing the junction must not have deleted through it";
}
#endif
