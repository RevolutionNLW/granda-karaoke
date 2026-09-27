#include "SongSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

SongPair writePair(const QString& folder, const QString& base,
                   char mp3Byte, char cdgByte)
{
    SongPair pair{QDir(folder).filePath(base + QStringLiteral(".mp3")),
                  QDir(folder).filePath(base + QStringLiteral(".cdg"))};
    if (!writeBytes(pair.mp3Path, QByteArray(140000, mp3Byte))
        || !writeBytes(pair.cdgPath, QByteArray(150000, cdgByte)))
        qFatal("Could not create identity fixture");
    return pair;
}

QByteArray id3v2Tag(int bodySize, char fill, bool footer)
{
    QByteArray tag(10, '\0');
    tag.replace(0, 3, "ID3");
    tag[3] = 4;
    tag[5] = footer ? char(0x10) : char(0);
    tag[6] = char((bodySize >> 21) & 0x7f);
    tag[7] = char((bodySize >> 14) & 0x7f);
    tag[8] = char((bodySize >> 7) & 0x7f);
    tag[9] = char(bodySize & 0x7f);
    tag.append(QByteArray(bodySize, fill));
    if (footer)
        tag.append(QByteArray("3DI\x04\0\x10\0\0\0\0", 10));
    return tag;
}

QByteArray id3v1Tag(char fill)
{
    QByteArray tag(128, fill);
    tag.replace(0, 3, "TAG");
    return tag;
}

QByteArray apev2Tag(int bodySize, char fill)
{
    QByteArray tag(bodySize, fill);
    QByteArray footer(32, '\0');
    footer.replace(0, 8, "APETAGEX");
    const quint32 declaredSize = static_cast<quint32>(bodySize + footer.size());
    for (int i = 0; i < 4; ++i)
        footer[12 + i] = char((declaredSize >> (8 * i)) & 0xff);
    tag.append(footer);
    return tag;
}

} // namespace

class TestSongSettings : public QObject {
    Q_OBJECT

private slots:
    void defaultsAndClamping();
    void identityUsesContentNotPathOrName();
    void identityIgnoresMp3Metadata();
    void identityHashesCompleteCdg();
    void identityValueIsStable();
    void bogusTagHeadersStillProduceIdentity();
    void storePersistsAndResets();
    void concurrentStoresMergeEntries();
    void busyLockDoesNotLoseEarlierChange();
    void invalidEntriesAreIsolatedAndClamped();
    void corruptDocumentIsPreserved_data();
    void corruptDocumentIsPreserved();
    void writeFailureKeepsMemoryValue();
    void newerVersionIsNeverOverwritten();
    void unreadableFileIsNeverOverwritten();
    void failedCorruptPreservationIsReadOnly();
};

void TestSongSettings::defaultsAndClamping()
{
    QCOMPARE(SongSettings{}.keySemitones, 0);
    QCOMPARE(SongSettings{}.tempoPercent, 100);
    const SongSettings low = SongSettings{-99, 2}.clamped();
    QCOMPARE(low.keySemitones, kMinKey);
    QCOMPARE(low.tempoPercent, kMinTempo);
    const SongSettings high = SongSettings{99, 999}.clamped();
    QCOMPARE(high.keySemitones, kMaxKey);
    QCOMPARE(high.tempoPercent, kMaxTempo);
}

void TestSongSettings::identityUsesContentNotPathOrName()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair first = writePair(dir.filePath("one"), "Same Name", 'a', 'b');
    const SongPair different = writePair(dir.filePath("two"), "Same Name", 'c', 'd');
    const SongPair moved = writePair(dir.filePath("renamed"), "A Different Name", 'a', 'b');
    const QString firstId = songIdentity(first);
    QVERIFY(firstId.startsWith(QStringLiteral("v1:")));
    QCOMPARE(firstId.size(), 67);
    QVERIFY(firstId != songIdentity(different));
    QCOMPARE(firstId, songIdentity(moved));

    SongSettingsStore store(dir.filePath("song-settings.json"));
    QVERIFY(store.store(firstId, {-2, 94}, first));
    QVERIFY(store.store(songIdentity(different), {3, 108}, different));
    QCOMPARE(store.settingsFor(firstId).keySemitones, -2);
    QCOMPARE(store.settingsFor(firstId).tempoPercent, 94);
    QCOMPARE(store.settingsFor(songIdentity(different)).keySemitones, 3);
    QCOMPARE(store.settingsFor(songIdentity(different)).tempoPercent, 108);

    QVERIFY(QFile::remove(moved.cdgPath));
    QVERIFY(songIdentity(moved).isEmpty());
}

void TestSongSettings::identityIgnoresMp3Metadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray audio(150000, 'p');
    const QByteArray cdg(150000, 'c');
    const SongPair first{dir.filePath("first.mp3"), dir.filePath("first.cdg")};
    const SongPair second{dir.filePath("second.mp3"), dir.filePath("second.cdg")};
    QVERIFY(writeBytes(first.mp3Path,
        id3v2Tag(37, 'a', false) + audio + apev2Tag(41, 'x') + id3v1Tag('1')));
    QVERIFY(writeBytes(second.mp3Path,
        id3v2Tag(311, 'b', true) + audio + apev2Tag(79, 'y') + id3v1Tag('2')));
    QVERIFY(writeBytes(first.cdgPath, cdg));
    QVERIFY(writeBytes(second.cdgPath, cdg));

    const QString firstId = songIdentity(first);
    QVERIFY(!firstId.isEmpty());
    QCOMPARE(firstId, songIdentity(second));
}

void TestSongSettings::identityValueIsStable()
{
    // Remembered Key/Tempo settings are keyed by this exact value. It must never
    // change for the same content, whatever code computes it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair pair = writePair(dir.filePath("golden"), "Golden", 'g', 'h');
    QByteArray tagged = QByteArray("ID3\x03\0\0\0\0\0\x0a", 10) + QByteArray(10, 't')
        + QByteArray(140000, 'g') + QByteArray("TAG", 3) + QByteArray(125, 'v');
    QVERIFY(writeBytes(pair.mp3Path, tagged));
    QCOMPARE(songIdentity(pair), QStringLiteral(
                 "v1:5f7c7698cf5a2a3713d706014cab600d0f8684a667d30b46ba9b3166cf333b61"));
}

void TestSongSettings::identityHashesCompleteCdg()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair first = writePair(dir.path(), "first", 'm', 'c');
    const SongPair second = writePair(dir.path(), "second", 'm', 'c');
    QFile cdg(second.cdgPath);
    QVERIFY(cdg.open(QIODevice::ReadWrite));
    QVERIFY(cdg.seek(cdg.size() / 2));
    QCOMPARE(cdg.write("different-in-the-middle"), qint64(23));
    cdg.close();

    QVERIFY(songIdentity(first) != songIdentity(second));
}

void TestSongSettings::bogusTagHeadersStillProduceIdentity()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair truncated{dir.filePath("truncated.mp3"), dir.filePath("truncated.cdg")};
    const SongPair bogus{dir.filePath("bogus.mp3"), dir.filePath("bogus.cdg")};
    QVERIFY(writeBytes(truncated.mp3Path, QByteArray("ID3\x04", 4)));
    QByteArray bogusBytes("ID3\x04\0\0", 6);
    bogusBytes.append(QByteArray::fromHex("80808080"));
    bogusBytes.append(QByteArray(200, 'a'));
    QVERIFY(writeBytes(bogus.mp3Path, bogusBytes));
    QVERIFY(writeBytes(truncated.cdgPath, QByteArray(300, 'c')));
    QVERIFY(writeBytes(bogus.cdgPath, QByteArray(300, 'c')));

    QVERIFY(songIdentity(truncated).startsWith(QStringLiteral("v1:")));
    QVERIFY(songIdentity(bogus).startsWith(QStringLiteral("v1:")));
}

void TestSongSettings::storePersistsAndResets()
{
    QTemporaryDir dir;
    const SongPair pair = writePair(dir.filePath("media"), "Singer - Song", 'm', 'g');
    const QString id = songIdentity(pair);
    const QString path = dir.filePath("settings/song-settings.json");

    {
        SongSettingsStore store(path);
        QCOMPARE(store.settingsFor(id).keySemitones, 0);
        QVERIFY(store.store(id, {-2, 94}, pair));
        QCOMPARE(store.settingsFor(id).keySemitones, -2);
        QCOMPARE(store.settingsFor(id).tempoPercent, 94);
    }
    {
        SongSettingsStore store(path);
        QCOMPARE(store.settingsFor(id).keySemitones, -2);
        QCOMPARE(store.settingsFor(id).tempoPercent, 94);
        QVERIFY(store.store(id, {}, pair));
    }
    SongSettingsStore reopened(path);
    QCOMPARE(reopened.settingsFor(id).keySemitones, 0);
    QCOMPARE(reopened.settingsFor(id).tempoPercent, 100);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(root.value("format").toString(), QStringLiteral("fks-song-settings"));
    QCOMPARE(root.value("version").toInt(), 1);
    const QJsonObject entry = root.value("songs").toObject().value(id).toObject();
    QCOMPARE(entry.value("lastPath").toString(), QFileInfo(pair.mp3Path).absoluteFilePath());
    QCOMPARE(entry.value("displayName").toString(), pair.displayName());
    QVERIFY(entry.value("updated").toString().endsWith(QLatin1Char('Z')));
}

void TestSongSettings::concurrentStoresMergeEntries()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair firstPair = writePair(dir.filePath("media"), "First", 'a', 'b');
    const SongPair secondPair = writePair(dir.filePath("media"), "Second", 'c', 'd');
    const QString path = dir.filePath("song-settings.json");
    SongSettingsStore first(path);
    SongSettingsStore second(path);

    QVERIFY(first.store("first", {-2, 94}, firstPair));
    QVERIFY(second.store("second", {3, 108}, secondPair));

    SongSettingsStore reopened(path);
    QCOMPARE(reopened.settingsFor("first").keySemitones, -2);
    QCOMPARE(reopened.settingsFor("first").tempoPercent, 94);
    QCOMPARE(reopened.settingsFor("second").keySemitones, 3);
    QCOMPARE(reopened.settingsFor("second").tempoPercent, 108);
}

void TestSongSettings::invalidEntriesAreIsolatedAndClamped()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("song-settings.json");
    QJsonObject songs;
    songs.insert("clamped", QJsonObject{{"key", -40}, {"tempo", 900}});
    songs.insert("bad-type", QJsonObject{{"key", "two"}, {"tempo", 90}});
    songs.insert("bad-fraction", QJsonObject{{"key", 1.5}, {"tempo", 90}});
    songs.insert("good", QJsonObject{{"key", 3}, {"tempo", 108}});
    const QJsonObject root{{"format", "fks-song-settings"}, {"version", 1}, {"songs", songs}};
    QVERIFY(writeBytes(path, QJsonDocument(root).toJson()));

    SongSettingsStore store(path);
    QCOMPARE(store.settingsFor("clamped").keySemitones, kMinKey);
    QCOMPARE(store.settingsFor("clamped").tempoPercent, kMaxTempo);
    QCOMPARE(store.settingsFor("bad-type").keySemitones, 0);
    QCOMPARE(store.settingsFor("bad-fraction").tempoPercent, 100);
    QCOMPARE(store.settingsFor("good").keySemitones, 3);
    QCOMPARE(store.settingsFor("good").tempoPercent, 108);
}

void TestSongSettings::corruptDocumentIsPreserved_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::newRow("garbage") << QByteArray("not json");
    QTest::newRow("wrong root type") << QByteArray("[]");
    QTest::newRow("wrong format") << QJsonDocument(QJsonObject{
        {"format", "something-else"}, {"version", 1}, {"songs", QJsonObject{}}}).toJson();
    QTest::newRow("invalid version") << QJsonDocument(QJsonObject{
        {"format", "fks-song-settings"}, {"version", 0}, {"songs", QJsonObject{}}}).toJson();
    QTest::newRow("songs not an object") << QJsonDocument(QJsonObject{
        {"format", "fks-song-settings"}, {"version", 1}, {"songs", 5}}).toJson();
}

void TestSongSettings::corruptDocumentIsPreserved()
{
    QFETCH(QByteArray, contents);
    QTemporaryDir dir;
    const QString path = dir.filePath("song-settings.json");
    QVERIFY(writeBytes(path, contents));
    SongSettingsStore store(path);
    QCOMPARE(store.settingsFor("anything").tempoPercent, 100);
    QVERIFY(!QFileInfo::exists(path));
    const QStringList preserved = QDir(dir.path()).entryList(
        {QStringLiteral("song-settings.json.corrupt-*")}, QDir::Files);
    QCOMPARE(preserved.size(), 1);
    QFile file(dir.filePath(preserved.first()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
}

void TestSongSettings::writeFailureKeepsMemoryValue()
{
    QTemporaryDir dir;
    const SongPair pair = writePair(dir.filePath("media"), "Song", 'x', 'y');
    const QString blocker = dir.filePath("not-a-folder");
    QVERIFY(writeBytes(blocker, "block"));
    SongSettingsStore store(blocker + QStringLiteral("/song-settings.json"));
    QVERIFY(!store.store("id", {-3, 88}, pair));
    QCOMPARE(store.settingsFor("id").keySemitones, -3);
    QCOMPARE(store.settingsFor("id").tempoPercent, 88);
}

void TestSongSettings::newerVersionIsNeverOverwritten()
{
    QTemporaryDir dir;
    const SongPair pair = writePair(dir.filePath("media"), "Song", 'x', 'y');
    const QString path = dir.filePath("song-settings.json");
    const QByteArray contents = QJsonDocument(QJsonObject{
        {"format", "fks-song-settings"}, {"version", 2}, {"songs", QJsonObject{}}}).toJson();
    QVERIFY(writeBytes(path, contents));

    SongSettingsStore store(path);
    QVERIFY(store.isReadOnly());
    QVERIFY(!store.store("id", {-3, 88}, pair));
    QCOMPARE(store.settingsFor("id").keySemitones, -3);  // Still applied this session.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
    QVERIFY(QDir(dir.path()).entryList({QStringLiteral("song-settings.json.corrupt-*")},
                                       QDir::Files).isEmpty());
}

void TestSongSettings::unreadableFileIsNeverOverwritten()
{
#ifdef Q_OS_WIN
    QSKIP("File permissions cannot make a file unreadable here");
#else
    QTemporaryDir dir;
    const SongPair pair = writePair(dir.filePath("media"), "Song", 'x', 'y');
    const QString path = dir.filePath("song-settings.json");
    const QByteArray contents = QJsonDocument(QJsonObject{
        {"format", "fks-song-settings"}, {"version", 1},
        {"songs", QJsonObject{{"other", QJsonObject{{"key", 2}, {"tempo", 96}}}}}}).toJson();
    QVERIFY(writeBytes(path, contents));
    QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
    {
        QFile probe(path);
        if (probe.open(QIODevice::ReadOnly))
            QSKIP("Running with privileges that ignore file permissions");
    }

    SongSettingsStore store(path);
    QVERIFY(store.isReadOnly());
    QVERIFY(!store.store("id", {-3, 88}, pair));
    QCOMPARE(store.settingsFor("id").tempoPercent, 88);
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
#endif
}

void TestSongSettings::failedCorruptPreservationIsReadOnly()
{
#ifdef Q_OS_WIN
    QSKIP("Directory permissions cannot reliably deny rename here");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const SongPair pair = writePair(dir.filePath("media"), "Song", 'x', 'y');
    const QString settingsDir = dir.filePath("settings");
    const QString path = QDir(settingsDir).filePath("song-settings.json");
    const QByteArray contents("corrupt settings that must survive");
    QVERIFY(writeBytes(path, contents));
    const QFileDevice::Permissions writablePermissions = QFileInfo(settingsDir).permissions();
    const QFileDevice::Permissions readOnlyPermissions = QFileDevice::ReadOwner
        | QFileDevice::ExeOwner;
    QVERIFY(QFile::setPermissions(settingsDir, readOnlyPermissions));

    SongSettingsStore store(path);
    if (!QFileInfo::exists(path)) {
        QVERIFY(QFile::setPermissions(settingsDir, writablePermissions));
        QSKIP("Running with privileges that ignore directory permissions");
    }
    QVERIFY(store.isReadOnly());
    QVERIFY(QFile::setPermissions(settingsDir, writablePermissions));
    QVERIFY(!store.store("id", {-3, 88}, pair));
    QCOMPARE(store.settingsFor("id").tempoPercent, 88);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
#endif
}

void TestSongSettings::busyLockDoesNotLoseEarlierChange()
{
    QTemporaryDir dir;
    const SongPair pair = writePair(dir.filePath("media"), "Song", 'x', 'y');
    const QString path = dir.filePath("song-settings.json");
    SongSettingsStore store(path);
    {
        QLockFile other(path + QStringLiteral(".lock"));
        QVERIFY(other.tryLock(0));
        QVERIFY(!store.store("first", {-2, 94}, pair));   // Busy: kept in memory only.
        QCOMPARE(store.settingsFor("first").keySemitones, -2);
    }
    QVERIFY(store.store("second", {1, 102}, pair));
    SongSettingsStore reopened(path);
    QCOMPARE(reopened.settingsFor("first").keySemitones, -2);
    QCOMPARE(reopened.settingsFor("first").tempoPercent, 94);
    QCOMPARE(reopened.settingsFor("second").keySemitones, 1);
}

QTEST_GUILESS_MAIN(TestSongSettings)
#include "tst_songsettings.moc"
