#include "SongPair.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestSongPair : public QObject {
    Q_OBJECT

private slots:
    void init();
    void mp3FindsCdg();
    void cdgFindsMp3();
    void extensionCaseIsIgnored();
    void namesWithPunctuationWork();
    void missingCdgIsReported();
    void missingMp3IsReported();
    void unreadableFileIsReported();
    void emptyFileIsReported();
    void unsupportedExtensionIsReported();
    void missingSelectedFileIsReported();

private:
    QString touch(const QString& name, const QByteArray& content = "data");
    std::unique_ptr<QTemporaryDir> m_dir;
};

QString TestSongPair::touch(const QString& name, const QByteArray& content)
{
    const QString path = m_dir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot create test file");
    file.write(content);
    return path;
}

void TestSongPair::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestSongPair::mp3FindsCdg()
{
    const QString mp3 = touch("Song.mp3");
    const QString cdg = touch("Song.cdg");
    touch("Other.cdg");
    const auto result = resolveSongPair(mp3);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.pair.mp3Path, QFileInfo(mp3).absoluteFilePath());
    QCOMPARE(result.pair.cdgPath, QFileInfo(cdg).absoluteFilePath());
    QCOMPARE(result.pair.displayName(), QStringLiteral("Song"));
}

void TestSongPair::cdgFindsMp3()
{
    const QString mp3 = touch("Song.mp3");
    const QString cdg = touch("Song.cdg");
    const auto result = resolveSongPair(cdg);
    QVERIFY(result.pair.isValid());
    QCOMPARE(result.pair.mp3Path, QFileInfo(mp3).absoluteFilePath());
    QCOMPARE(result.pair.cdgPath, QFileInfo(cdg).absoluteFilePath());
}

void TestSongPair::extensionCaseIsIgnored()
{
    touch("Loud.MP3");
    touch("Loud.CDG");
    const auto result = resolveSongPair(m_dir->filePath("Loud.MP3"));
    QVERIFY(result.pair.isValid());
    QVERIFY(result.pair.cdgPath.endsWith("Loud.CDG"));
}

void TestSongPair::namesWithPunctuationWork()
{
    const QString name = "Disc-01 - Artist - Call Me, Beep Me (The Song).v2";
    touch(name + ".mp3");
    touch(name + ".cdg");
    const auto result = resolveSongPair(m_dir->filePath(name + ".cdg"));
    QVERIFY(result.pair.isValid());
    QCOMPARE(result.pair.displayName(), name);
}

void TestSongPair::missingCdgIsReported()
{
    touch("Alone.mp3");
    touch("Different.cdg");
    const auto result = resolveSongPair(m_dir->filePath("Alone.mp3"));
    QVERIFY(!result.pair.isValid());
    QVERIFY(result.error.contains(".cdg"));
    QVERIFY(result.error.contains("missing"));
}

void TestSongPair::missingMp3IsReported()
{
    touch("Alone.cdg");
    const auto result = resolveSongPair(m_dir->filePath("Alone.cdg"));
    QVERIFY(!result.pair.isValid());
    QVERIFY(result.error.contains(".mp3"));
}

void TestSongPair::unreadableFileIsReported()
{
#ifdef Q_OS_WIN
    QSKIP("File permissions cannot make a file unreadable here");
#endif
    touch("Locked.mp3");
    const QString cdg = touch("Locked.cdg");
    QVERIFY(QFile::setPermissions(cdg, QFileDevice::Permissions{}));
    QFile probe(cdg);
    if (probe.open(QIODevice::ReadOnly))
        QSKIP("Running with privileges that ignore file permissions");
    const auto result = resolveSongPair(m_dir->filePath("Locked.mp3"));
    QFile::setPermissions(cdg, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QVERIFY(!result.pair.isValid());
    QVERIFY2(result.error.contains("could not be opened"), qPrintable(result.error));
}

void TestSongPair::emptyFileIsReported()
{
    touch("Empty.mp3");
    touch("Empty.cdg", QByteArray());
    const auto result = resolveSongPair(m_dir->filePath("Empty.mp3"));
    QVERIFY(!result.pair.isValid());
    QVERIFY(result.error.contains("empty"));
}

void TestSongPair::unsupportedExtensionIsReported()
{
    const auto result = resolveSongPair(touch("notes.txt"));
    QVERIFY(!result.pair.isValid());
    QVERIFY(result.error.contains(".mp3 or .cdg"));
}

void TestSongPair::missingSelectedFileIsReported()
{
    const auto result = resolveSongPair(m_dir->filePath("Ghost.mp3"));
    QVERIFY(!result.pair.isValid());
    QVERIFY(result.error.contains("could not be found"));
}

QTEST_GUILESS_MAIN(TestSongPair)
#include "tst_songpair.moc"
