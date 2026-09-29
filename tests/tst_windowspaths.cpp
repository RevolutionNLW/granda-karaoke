// Music-folder safety with the path forms Windows produces: drive letters,
// backslashes, case differences, UNC shares, 8.3 short names, junctions and
// substituted drive letters. The rule under test: nothing the program writes
// (databases, SQLite journals, caches) may ever be opened inside a music
// folder, and a sibling folder such as C:/Karaoke2 is never mistaken for
// part of C:/Karaoke. Every other platform runs the portable part.

#include "AppStorage.h"
#include "library/Catalogue.h"
#include "library/LibraryScanner.h"
#include "library/MetadataOverrideStore.h"
#include "library/UserStateStore.h"
#include "playlist/PlaylistStore.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}

// Every entry under a folder, hidden and system ones included, with its size
// and modification time: two equal snapshots mean nothing was written there.
QStringList snapshot(const QString& root)
{
    QStringList entries;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        entries.append(QStringLiteral("%1|%2|%3").arg(
            QDir(root).relativeFilePath(info.absoluteFilePath()),
            info.isDir() ? QStringLiteral("dir") : QString::number(info.size()),
            info.isDir() ? QString() : QString::number(info.lastModified().toMSecsSinceEpoch())));
    }
    entries.sort();
    return entries;
}

// A small music folder the tests must leave exactly as it was.
void makeMusicFolder(const QString& root)
{
    writeFile(root + QStringLiteral("/Disc 1/SC001-01 - Artist - Song.mp3"), QByteArray("audio"));
    writeFile(root + QStringLiteral("/Disc 1/SC001-01 - Artist - Song.cdg"), QByteArray("lyrics"));
    writeFile(root + QStringLiteral("/Über Straße ♪/SC002-01 - Künstler - Lied.mp3"), QByteArray("audio"));
    writeFile(root + QStringLiteral("/Über Straße ♪/SC002-01 - Künstler - Lied.cdg"), QByteArray("lyrics"));
}

#ifdef Q_OS_WIN
QString shortPathName(const QString& path)
{
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    const DWORD length = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (length == 0)
        return {};
    std::wstring buffer(length, L'\0');
    const DWORD written = GetShortPathNameW(wide.c_str(), buffer.data(), length);
    if (written == 0 || written >= length)
        return {};
    buffer.resize(written);
    return QDir::fromNativeSeparators(QString::fromStdWString(buffer));
}

bool runCommand(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.start(program, arguments);
    return process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
}

// A drive letter nobody is using, for `subst`.
QString freeDriveLetter()
{
    const DWORD used = GetLogicalDrives();
    for (char letter = 'Z'; letter >= 'M'; --letter) {
        if (!(used & (1u << (letter - 'A'))))
            return QString(QLatin1Char(letter));
    }
    return {};
}
#endif

} // namespace

class TestWindowsPaths : public QObject {
    Q_OBJECT

private slots:
    void containment_data();
    void containment();
    void storageRefusedInsideRoot_data();
    void storageRefusedInsideRoot();
    void everyStoreRefusesWindowsSpellingsBeforeSqlite();
    void siblingFolderStorageIsAccepted();
    void shortNamesResolveToTheMusicFolder();
    void trailingDotsAndSpacesResolveToTheMusicFolder();
    void junctionIntoMusicFolderIsRefused();
    void substitutedDriveIsRecognised();
    void longPathsAreCompared();
    void uncShareContainment();
    void programDataFolderIsLocalAndOwnedByTheProgram();
    void linkToAPlaceNotYetMadeIsRefused();
};

void TestWindowsPaths::containment_data()
{
    QTest::addColumn<QString>("candidate");
    QTest::addColumn<QString>("root");
    QTest::addColumn<bool>("inside");

    // Portable: '/'-separated folders that never exist, compared by name.
    QTest::newRow("root itself") << "/fks-nowhere/Karaoke" << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("child") << "/fks-nowhere/Karaoke/Songs" << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("deep file") << "/fks-nowhere/Karaoke/a/b/c/d/e/library.sqlite"
                               << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("sibling with suffix") << "/fks-nowhere/Karaoke2" << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("file in sibling") << "/fks-nowhere/Karaoke2/library.sqlite"
                                     << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("sibling with dot") << "/fks-nowhere/Karaoke.old/x" << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("sibling with space") << "/fks-nowhere/Karaoke Songs/x" << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("parent") << "/fks-nowhere" << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("dot-dot out") << "/fks-nowhere/Karaoke/../Karaoke2/x" << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("dot-dot in") << "/fks-nowhere/Karaoke2/../Karaoke/x" << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("dot and double slash") << "/fks-nowhere/Karaoke/.//Songs" << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("spaces and Unicode") << "/fks-nowhere/Frankie's Karaoke/Über Straße ♪/x.sqlite"
                                        << "/fks-nowhere/Frankie's Karaoke" << true;
    QTest::newRow("Unicode sibling") << "/fks-nowhere/Frankie's Karaoke ♪/x.sqlite"
                                     << "/fks-nowhere/Frankie's Karaoke" << false;

#ifdef Q_OS_WIN
    QTest::newRow("C: root itself") << "C:/Karaoke" << "C:/Karaoke" << true;
    QTest::newRow("C: child") << "C:/Karaoke/Songs" << "C:/Karaoke" << true;
    QTest::newRow("C: sibling Karaoke2") << "C:/Karaoke2" << "C:/Karaoke" << false;
    QTest::newRow("C: file in Karaoke2") << "C:/Karaoke2/library.sqlite" << "C:/Karaoke" << false;
    QTest::newRow("C: parent") << "C:/" << "C:/Karaoke" << false;
    QTest::newRow("whole drive as root") << "C:/Karaoke/x.sqlite" << "C:/" << true;
    QTest::newRow("D: file") << "D:/Music/Karaoke/Song.mp3" << "D:/Music/Karaoke" << true;
    QTest::newRow("D: root is not inside its file") << "D:/Music/Karaoke" << "D:/Music/Karaoke/Song.mp3" << false;
    QTest::newRow("D: parent") << "D:/Music" << "D:/Music/Karaoke" << false;
    QTest::newRow("D: sibling with space") << "D:/Music/Karaoke Songs/x" << "D:/Music/Karaoke" << false;
    QTest::newRow("other drive") << "E:/Karaoke/x.sqlite" << "C:/Karaoke" << false;
    QTest::newRow("backslashes") << "C:\\Karaoke\\Songs\\x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("backslash root") << "C:/Karaoke/Songs" << "C:\\Karaoke" << true;
    QTest::newRow("trailing backslash root") << "C:/Karaoke/Songs" << "C:\\Karaoke\\" << true;
    QTest::newRow("mixed separators") << "C:\\Karaoke/Songs\\x.sqlite" << "C:/Karaoke\\" << true;
    QTest::newRow("backslash sibling") << "C:\\Karaoke2\\x.sqlite" << "C:\\Karaoke" << false;
    QTest::newRow("case differs") << "c:/karaoke/songs/x.sqlite" << "C:/KARAOKE" << true;
    QTest::newRow("drive letter case") << "c:/Karaoke/x" << "C:/Karaoke" << true;
    QTest::newRow("Unicode case") << "C:/ÜBER/x" << "C:/über" << true;
    QTest::newRow("case differs, sibling") << "C:/KARAOKE2/x" << "c:/karaoke" << false;
    QTest::newRow("removable drive root") << "F:/Songs/x.sqlite" << "F:/" << true;
    QTest::newRow("removable other drive") << "G:/x.sqlite" << "F:/" << false;
    QTest::newRow("backslash dot-dot out") << "C:\\Karaoke\\..\\Karaoke2\\x" << "C:/Karaoke" << false;
    // Windows drops trailing dots and spaces, and names of dots alone.
    QTest::newRow("trailing dot") << "C:/Karaoke./x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("trailing space") << "C:/Karaoke /x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("trailing dots and spaces") << "C:/Karaoke. . /x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("name of dots") << "C:/.../Karaoke/x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("name of spaces") << "C:/   /Karaoke/x.sqlite" << "C:/Karaoke" << true;
    QTest::newRow("root with trailing dot") << "C:/Karaoke/x.sqlite" << "C:/Karaoke." << true;
    QTest::newRow("dotted sibling stays outside") << "C:/Karaoke.2/x.sqlite" << "C:/Karaoke" << false;
#endif
}

void TestWindowsPaths::containment()
{
    QFETCH(QString, candidate);
    QFETCH(QString, root);
    QFETCH(bool, inside);
    QCOMPARE(Catalogue::pathIsInsideOrEqual(candidate, root), inside);
    // The check used before writing agrees wherever Windows can say where
    // a path is (and errs towards "inside" where it cannot).
    QCOMPARE(Catalogue::mayBeInsideOrEqual(candidate, root), inside);
}

void TestWindowsPaths::storageRefusedInsideRoot_data()
{
    QTest::addColumn<QString>("database");
    QTest::addColumn<QString>("cache");
    QTest::addColumn<QString>("root");
    QTest::addColumn<bool>("safe");

    QTest::newRow("database in sibling") << "/fks-nowhere/Karaoke2/library.sqlite" << ""
                                         << "/fks-nowhere/Karaoke" << true;
    QTest::newRow("database inside") << "/fks-nowhere/Karaoke/library.sqlite" << ""
                                     << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("cache inside") << "/fks-nowhere/App/library.sqlite" << "/fks-nowhere/Karaoke/cache"
                                  << "/fks-nowhere/Karaoke" << false;
    QTest::newRow("cache is the root") << "/fks-nowhere/App/library.sqlite" << "/fks-nowhere/Karaoke"
                                       << "/fks-nowhere/Karaoke" << false;
#ifdef Q_OS_WIN
    QTest::newRow("C: database inside") << "C:\\Karaoke\\library.sqlite" << "" << "C:/Karaoke" << false;
    QTest::newRow("C: database in Karaoke2") << "C:/Karaoke2/library.sqlite" << "" << "C:/Karaoke" << true;
    QTest::newRow("C: case-changed database") << "c:/KARAOKE/Songs/library.sqlite" << "" << "C:/Karaoke" << false;
    QTest::newRow("C: cache inside by backslash") << "C:/App/library.sqlite" << "C:\\Karaoke\\cache"
                                                  << "C:/Karaoke" << false;
    QTest::newRow("whole removable drive") << "F:/library.sqlite" << "" << "F:/" << false;
    QTest::newRow("app data outside removable drive")
        << "C:/Users/Frankie/AppData/Local/Granda/FrankiesKaraokeStudio/library.sqlite"
        << "C:/Users/Frankie/AppData/Local/Granda/FrankiesKaraokeStudio" << "F:/" << true;
    QTest::newRow("music folder is the user profile")
        << "C:/Users/Frankie/AppData/Local/Granda/FrankiesKaraokeStudio/library.sqlite" << ""
        << "C:/Users/Frankie" << false;
#endif
}

void TestWindowsPaths::storageRefusedInsideRoot()
{
    QFETCH(QString, database);
    QFETCH(QString, cache);
    QFETCH(QString, root);
    QFETCH(bool, safe);
    QString error;
    QCOMPARE(Catalogue::storageIsSafe(database, cache, {root}, &error), safe);
    if (!safe)
        QVERIFY2(error.contains(QStringLiteral("must not be inside")), qPrintable(error));
}

void TestWindowsPaths::everyStoreRefusesWindowsSpellingsBeforeSqlite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Frankie's Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);

    // The same folder written the ways Windows users and APIs write it.
    QStringList spellings{root + QStringLiteral("/Disc 1")};
#ifdef Q_OS_WIN
    spellings << QDir::toNativeSeparators(root + QStringLiteral("/Disc 1"))
              << QDir::toNativeSeparators(root).toUpper() + QStringLiteral("\\DISC 1")
              << root.toLower() + QStringLiteral("\\disc 1");
#endif
    for (const QString& folder : std::as_const(spellings)) {
        const QString at = folder + QStringLiteral("/");
        QString error;

        Catalogue catalogue(at + QStringLiteral("library.sqlite"));
        QVERIFY2(!catalogue.open(&error, {root}), qPrintable(folder));
        QVERIFY2(error.contains(QStringLiteral("must not be inside")), qPrintable(error));

        Catalogue cached(temporary.filePath(QStringLiteral("app/library.sqlite")),
                         at + QStringLiteral("cache"));
        QVERIFY2(!cached.open(&error, {root}), qPrintable(folder));
        QVERIFY2(error.contains(QStringLiteral("Cache directory")), qPrintable(error));

        PlaylistStore playlists(at + QStringLiteral("playlists.sqlite"));
        QVERIFY2(!playlists.open(&error, {root}), qPrintable(folder));
        QVERIFY2(error.contains(QStringLiteral("inside library root")), qPrintable(error));

        MetadataOverrideStore overrides(at + QStringLiteral("metadata-overrides.sqlite"));
        QVERIFY2(!overrides.open(&error, {root}), qPrintable(folder));
        QVERIFY2(error.contains(QStringLiteral("must not be inside")), qPrintable(error));

        UserStateStore userState(at + QStringLiteral("user-state.sqlite"));
        QVERIFY2(!userState.open(&error, {root}), qPrintable(folder));

        LibraryScanner scanner(at + QStringLiteral("library.sqlite"));
        QString failure;
        QObject::connect(&scanner, &LibraryScanner::failed,
                         [&failure](const QString& value) { failure = value; });
        scanner.scan(root);
        QVERIFY2(failure.contains(QStringLiteral("must not be inside")), qPrintable(failure));
    }
    // Not one file (not even a journal, WAL or lock file) appeared.
    QCOMPARE(snapshot(root), before);
}

void TestWindowsPaths::siblingFolderStorageIsAccepted()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    const QString sibling = temporary.filePath(QStringLiteral("Karaoke2"));
    QVERIFY(QDir().mkpath(sibling));

    QString error;
    {
        Catalogue catalogue(sibling + QStringLiteral("/library.sqlite"));
        QVERIFY2(catalogue.open(&error, {root}), qPrintable(error));
    }
    {
        PlaylistStore playlists(sibling + QStringLiteral("/playlists.sqlite"));
        QVERIFY2(playlists.open(&error, {root}), qPrintable(error));
    }
    {
        MetadataOverrideStore overrides(sibling + QStringLiteral("/metadata-overrides.sqlite"));
        QVERIFY2(overrides.open(&error, {root}), qPrintable(error));
    }
    {
        UserStateStore userState(sibling + QStringLiteral("/user-state.sqlite"));
        QVERIFY2(userState.open(&error, {root}), qPrintable(error));
    }
    QVERIFY(QFileInfo::exists(sibling + QStringLiteral("/library.sqlite")));
    QCOMPARE(snapshot(root), before);
}

void TestWindowsPaths::shortNamesResolveToTheMusicFolder()
{
#ifndef Q_OS_WIN
    QSKIP("8.3 short names exist only on Windows");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Frankie Karaoke Collection"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    const QString shortRoot = shortPathName(root);
    qInfo().noquote() << "Long:" << root << "Short:" << shortRoot;
    if (shortRoot.isEmpty() || QFileInfo(shortRoot).fileName() == QFileInfo(root).fileName())
        QSKIP("8.3 short names are turned off on this volume");

    QVERIFY(Catalogue::pathIsInsideOrEqual(shortRoot + QStringLiteral("/new.sqlite"), root));
    QVERIFY(Catalogue::pathIsInsideOrEqual(root + QStringLiteral("/new.sqlite"), shortRoot));
    QVERIFY(Catalogue::pathIsInsideOrEqual(shortRoot + QStringLiteral("/not-yet/deeper/new.sqlite"), root));
    QString error;
    Catalogue catalogue(shortRoot + QStringLiteral("/library.sqlite"));
    QVERIFY(!catalogue.open(&error, {root}));
    PlaylistStore playlists(root + QStringLiteral("/playlists.sqlite"));
    QVERIFY(!playlists.open(&error, {shortRoot}));
    QCOMPARE(snapshot(root), before);
#endif
}

void TestWindowsPaths::trailingDotsAndSpacesResolveToTheMusicFolder()
{
#ifndef Q_OS_WIN
    QSKIP("Only Windows drops trailing dots and spaces from folder names");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    // Windows opens "Karaoke." and "Karaoke " as the folder "Karaoke".
    for (const QString& spelling : {root + QStringLiteral("./library.sqlite"),
                                    root + QStringLiteral(" /library.sqlite"),
                                    root + QStringLiteral("./Disc 1./library.sqlite")}) {
        QString error;
        Catalogue catalogue(spelling);
        QVERIFY2(!catalogue.open(&error, {root}), qPrintable(spelling));
        PlaylistStore playlists(spelling);
        QVERIFY2(!playlists.open(&error, {root}), qPrintable(spelling));
    }
    QCOMPARE(snapshot(root), before);
#endif
}

void TestWindowsPaths::junctionIntoMusicFolderIsRefused()
{
#ifndef Q_OS_WIN
    QSKIP("Directory junctions are a Windows feature (symbolic links are covered elsewhere)");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    // An innocent-looking app folder that is really a junction into the music.
    const QString junction = temporary.filePath(QStringLiteral("AppData"));
    QVERIFY(runCommand(QStringLiteral("cmd.exe"),
                       {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                        QDir::toNativeSeparators(junction),
                        QDir::toNativeSeparators(root + QStringLiteral("/Disc 1"))}));
    QVERIFY(QFileInfo(junction).isDir());

    QVERIFY(Catalogue::pathIsInsideOrEqual(junction + QStringLiteral("/library.sqlite"), root));
    QString error;
    Catalogue catalogue(junction + QStringLiteral("/library.sqlite"));
    QVERIFY(!catalogue.open(&error, {root}));
    PlaylistStore playlists(junction + QStringLiteral("/playlists.sqlite"));
    QVERIFY(!playlists.open(&error, {root}));
    UserStateStore userState(junction + QStringLiteral("/user-state.sqlite"));
    QVERIFY(!userState.open(&error, {root}));

    // The other way round: the music folder is chosen through a junction.
    const QString musicLink = temporary.filePath(QStringLiteral("MusicLink"));
    QVERIFY(runCommand(QStringLiteral("cmd.exe"),
                       {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                        QDir::toNativeSeparators(musicLink), QDir::toNativeSeparators(root)}));
    MetadataOverrideStore overrides(root + QStringLiteral("/metadata-overrides.sqlite"));
    QVERIFY(!overrides.open(&error, {musicLink}));
    QCOMPARE(snapshot(root), before);
    QVERIFY(QDir().rmdir(junction));
    QVERIFY(QDir().rmdir(musicLink));
#endif
}

void TestWindowsPaths::substitutedDriveIsRecognised()
{
#ifndef Q_OS_WIN
    QSKIP("Drive-letter substitution is a Windows feature");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    const QString letter = freeDriveLetter();
    if (letter.isEmpty())
        QSKIP("No free drive letter");
    const QString drive = letter + QStringLiteral(":");
    // The music folder appears as a whole drive, as a USB drive would.
    if (!runCommand(QStringLiteral("subst.exe"), {drive, QDir::toNativeSeparators(root)}))
        QSKIP("subst is not available here");
    const auto cleanup = qScopeGuard([&drive] {
        runCommand(QStringLiteral("subst.exe"), {drive, QStringLiteral("/d")});
    });
    QVERIFY(QFileInfo::exists(drive + QStringLiteral("/Disc 1")));
    qInfo().noquote() << "Substituted" << drive << "canonical:" << Catalogue::canonicalPath(drive + QStringLiteral("/"));

    QString error;
    // Music folder chosen as the drive; storage given by its real folder.
    Catalogue catalogue(root + QStringLiteral("/library.sqlite"));
    QVERIFY(!catalogue.open(&error, {drive + QStringLiteral("/")}));
    // Music folder chosen by its real folder; storage given on the drive.
    PlaylistStore playlists(drive + QStringLiteral("/playlists.sqlite"));
    QVERIFY(!playlists.open(&error, {root}));
    PlaylistStore nested(drive + QStringLiteral("\\Disc 1\\playlists.sqlite"));
    QVERIFY(!nested.open(&error, {root}));
    // Storage elsewhere is fine.
    Catalogue elsewhere(temporary.filePath(QStringLiteral("app/library.sqlite")));
    QVERIFY2(elsewhere.open(&error, {drive + QStringLiteral("/")}), qPrintable(error));
    elsewhere.close();
    QCOMPARE(snapshot(root), before);
#endif
}

void TestWindowsPaths::longPathsAreCompared()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    QVERIFY(QDir().mkpath(root));
    QString deep = root;
    while (deep.size() < 400)
        deep += QStringLiteral("/A Rather Long Folder Name For Karaoke Discs");
    // Compared by name even where the folders do not (or cannot) exist.
    QVERIFY(Catalogue::pathIsInsideOrEqual(deep + QStringLiteral("/library.sqlite"), root));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(deep + QStringLiteral("/library.sqlite"),
                                            root + QStringLiteral("/A Rather Long")));
    QString error;
    QVERIFY(!Catalogue::storageIsSafe(deep + QStringLiteral("/library.sqlite"), {}, {root}, &error));
    // Where the system allows long folders, real ones compare the same way.
    if (QDir().mkpath(deep)) {
        QVERIFY(Catalogue::pathIsInsideOrEqual(deep, root));
        PlaylistStore playlists(deep + QStringLiteral("/playlists.sqlite"));
        QVERIFY(!playlists.open(&error, {root}));
        QVERIFY(!QFileInfo::exists(deep + QStringLiteral("/playlists.sqlite")));
    } else {
        qInfo() << "Folders longer than" << deep.size() << "characters are not allowed here";
    }
}

void TestWindowsPaths::uncShareContainment()
{
#ifndef Q_OS_WIN
    QSKIP("UNC network paths are a Windows feature");
#else
    // A share on this machine that does not exist: the check must still
    // compare by name, and must not hang on the network.
    const QString share = QStringLiteral("//127.0.0.1/FksNoSuchShare");
    QElapsedTimer timer;
    timer.start();
    QVERIFY(Catalogue::pathIsInsideOrEqual(share + QStringLiteral("/Karaoke/x.sqlite"),
                                           share + QStringLiteral("/Karaoke")));
    QVERIFY(Catalogue::pathIsInsideOrEqual(QStringLiteral("\\\\127.0.0.1\\FksNoSuchShare\\Karaoke\\x.sqlite"),
                                           share + QStringLiteral("/karaoke")));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(share + QStringLiteral("/Karaoke2/x.sqlite"),
                                            share + QStringLiteral("/Karaoke")));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(QStringLiteral("C:/Karaoke/x.sqlite"),
                                            share + QStringLiteral("/Karaoke")));
    qInfo() << "UNC comparisons took" << timer.elapsed() << "ms";
#endif
}

void TestWindowsPaths::programDataFolderIsLocalAndOwnedByTheProgram()
{
    // The names main() sets; everything the program writes lives here.
    QCoreApplication::setOrganizationName(QStringLiteral("Granda"));
    QCoreApplication::setApplicationName(QStringLiteral("FrankiesKaraokeStudio"));
    const auto restore = qScopeGuard([] {
        QCoreApplication::setOrganizationName({});
        QCoreApplication::setApplicationName(QStringLiteral("tst_windowspaths"));
    });
    const QString folder = appstorage::folder();
    qInfo().noquote() << "Program data folder:" << folder;
    QVERIFY(!folder.isEmpty());
    QVERIFY(folder.endsWith(QStringLiteral("/Granda/FrankiesKaraokeStudio")));
    const QString programFolder = QCoreApplication::applicationDirPath();
    QVERIFY(!Catalogue::pathIsInsideOrEqual(folder, programFolder));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(
        folder, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(
        folder, QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)));
#ifdef Q_OS_WIN
    // %LOCALAPPDATA%, not the roaming profile.
    const QString local = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    QVERIFY(!local.isEmpty());
    QCOMPARE(folder.toCaseFolded(), (local + QStringLiteral("/Granda/FrankiesKaraokeStudio")).toCaseFolded());
    const QString roaming = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(folder, roaming));
#elif defined(Q_OS_MACOS)
    // Unchanged on macOS: both locations are the same folder there.
    QCOMPARE(folder, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
#endif
}

void TestWindowsPaths::linkToAPlaceNotYetMadeIsRefused()
{
    // A link whose target does not exist yet looks like a missing name, but
    // creating the database through it would put it in the music folder.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("Karaoke"));
    makeMusicFolder(root);
    const QStringList before = snapshot(root);
    const QString app = temporary.filePath(QStringLiteral("app"));
    QVERIFY(QDir().mkpath(app));
#ifdef Q_OS_WIN
    // Qt makes shortcut files, not links, on Windows; a junction does the same.
    const QString folder = app + QStringLiteral("/linked");
    QVERIFY(runCommand(QStringLiteral("cmd.exe"),
                       {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                        QDir::toNativeSeparators(folder),
                        QDir::toNativeSeparators(root + QStringLiteral("/Not Yet"))}));
    const QString at = folder + QStringLiteral("/");
#else
    for (const char* name : {"library.sqlite", "playlists.sqlite", "metadata-overrides.sqlite",
                             "user-state.sqlite"}) {
        QVERIFY(QFile::link(QDir(root).filePath(QLatin1String(name)),
                            QDir(app).filePath(QLatin1String(name))));
    }
    const QString at = app + QStringLiteral("/");
#endif
    QString error;
    Catalogue catalogue(at + QStringLiteral("library.sqlite"));
    QVERIFY(!catalogue.open(&error, {root}));
    PlaylistStore playlists(at + QStringLiteral("playlists.sqlite"));
    QVERIFY(!playlists.open(&error, {root}));
    MetadataOverrideStore overrides(at + QStringLiteral("metadata-overrides.sqlite"));
    QVERIFY(!overrides.open(&error, {root}));
    UserStateStore userState(at + QStringLiteral("user-state.sqlite"));
    QVERIFY(!userState.open(&error, {root}));
    QVERIFY(Catalogue::mayBeInsideOrEqual(at + QStringLiteral("library.sqlite"), root));
    QCOMPARE(snapshot(root), before);
}

QTEST_GUILESS_MAIN(TestWindowsPaths)
#include "tst_windowspaths.moc"
