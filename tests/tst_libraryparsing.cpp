#include "library/FilenameParser.h"
#include "library/Id3Reader.h"
#include "library/ZipDirectory.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void appendLe16(QByteArray& bytes, quint16 value)
{
    bytes.append(char(value & 0xffU));
    bytes.append(char((value >> 8) & 0xffU));
}

void appendLe32(QByteArray& bytes, quint32 value)
{
    appendLe16(bytes, quint16(value & 0xffffU));
    appendLe16(bytes, quint16(value >> 16));
}

void appendLe64(QByteArray& bytes, quint64 value)
{
    appendLe32(bytes, quint32(value & 0xffffffffU));
    appendLe32(bytes, quint32(value >> 32));
}

QByteArray syncSafe(quint32 value)
{
    QByteArray bytes;
    bytes.append(char((value >> 21) & 0x7fU));
    bytes.append(char((value >> 14) & 0x7fU));
    bytes.append(char((value >> 7) & 0x7fU));
    bytes.append(char(value & 0x7fU));
    return bytes;
}

struct ZipFixtureMember {
    QByteArray name;
    quint16 method = 0;
    quint16 flags = 0;
    QByteArray data = "x";
};

QByteArray makeZip(const QList<ZipFixtureMember>& members, const QByteArray& comment = {},
                   bool zip64 = false, int claimedEntryDelta = 0)
{
    QByteArray zip;
    struct CentralInfo {
        ZipFixtureMember member;
        quint64 offset = 0;
    };
    QList<CentralInfo> central;
    for (const ZipFixtureMember& member : members) {
        CentralInfo info{member, quint64(zip.size())};
        central.append(info);
        appendLe32(zip, 0x04034b50);
        appendLe16(zip, 20);
        appendLe16(zip, member.flags);
        appendLe16(zip, member.method);
        appendLe16(zip, 0);
        appendLe16(zip, 0);
        appendLe32(zip, 0x12345678);
        appendLe32(zip, quint32(member.data.size()));
        appendLe32(zip, quint32(member.data.size()));
        appendLe16(zip, quint16(member.name.size()));
        appendLe16(zip, 0);
        zip += member.name;
        zip += member.data;
    }

    const quint64 centralOffset = quint64(zip.size());
    for (const CentralInfo& info : central) {
        QByteArray extra;
        if (zip64) {
            appendLe16(extra, 0x0001);
            appendLe16(extra, 24);
            appendLe64(extra, quint64(info.member.data.size()));
            appendLe64(extra, quint64(info.member.data.size()));
            appendLe64(extra, info.offset);
        }
        appendLe32(zip, 0x02014b50);
        appendLe16(zip, 45);
        appendLe16(zip, 20);
        appendLe16(zip, info.member.flags);
        appendLe16(zip, info.member.method);
        appendLe16(zip, 0);
        appendLe16(zip, 0);
        appendLe32(zip, 0x12345678);
        appendLe32(zip, zip64 ? 0xffffffffU : quint32(info.member.data.size()));
        appendLe32(zip, zip64 ? 0xffffffffU : quint32(info.member.data.size()));
        appendLe16(zip, quint16(info.member.name.size()));
        appendLe16(zip, quint16(extra.size()));
        appendLe16(zip, 0);
        appendLe16(zip, 0);
        appendLe16(zip, 0);
        appendLe32(zip, 0);
        appendLe32(zip, zip64 ? 0xffffffffU : quint32(info.offset));
        zip += info.member.name;
        zip += extra;
    }
    const quint64 centralSize = quint64(zip.size()) - centralOffset;
    const quint64 actualCount = quint64(members.size());
    const quint64 claimedCount = quint64(qint64(actualCount) + claimedEntryDelta);

    if (zip64) {
        const quint64 zip64Offset = quint64(zip.size());
        appendLe32(zip, 0x06064b50);
        appendLe64(zip, 44);
        appendLe16(zip, 45);
        appendLe16(zip, 45);
        appendLe32(zip, 0);
        appendLe32(zip, 0);
        appendLe64(zip, claimedCount);
        appendLe64(zip, claimedCount);
        appendLe64(zip, centralSize);
        appendLe64(zip, centralOffset);
        appendLe32(zip, 0x07064b50);
        appendLe32(zip, 0);
        appendLe64(zip, zip64Offset);
        appendLe32(zip, 1);
    }

    appendLe32(zip, 0x06054b50);
    appendLe16(zip, 0);
    appendLe16(zip, 0);
    appendLe16(zip, zip64 ? 0xffffU : quint16(claimedCount));
    appendLe16(zip, zip64 ? 0xffffU : quint16(claimedCount));
    appendLe32(zip, zip64 ? 0xffffffffU : quint32(centralSize));
    appendLe32(zip, zip64 ? 0xffffffffU : quint32(centralOffset));
    appendLe16(zip, quint16(comment.size()));
    zip += comment;
    return zip;
}

QByteArray textPayload(uchar encoding, const QByteArray& text)
{
    QByteArray result(1, char(encoding));
    result += text;
    return result;
}

QByteArray v23Frame(const QByteArray& id, const QByteArray& payload)
{
    QByteArray result = id;
    appendLe32(result, 0); // replaced below with big-endian size
    const quint32 size = quint32(payload.size());
    result[4] = char(size >> 24);
    result[5] = char(size >> 16);
    result[6] = char(size >> 8);
    result[7] = char(size);
    result.append("\0\0", 2);
    result += payload;
    return result;
}

QByteArray v24Frame(const QByteArray& id, const QByteArray& payload, quint16 flags = 0)
{
    QByteArray result = id + syncSafe(quint32(payload.size()));
    result.append(char(flags >> 8));
    result.append(char(flags));
    result += payload;
    return result;
}

QByteArray v22Frame(const QByteArray& id, const QByteArray& payload)
{
    QByteArray result = id;
    const quint32 size = quint32(payload.size());
    result.append(char(size >> 16));
    result.append(char(size >> 8));
    result.append(char(size));
    result += payload;
    return result;
}

QByteArray id3Tag(uchar version, const QByteArray& body, uchar flags = 0)
{
    QByteArray result("ID3", 3);
    result.append(char(version));
    result.append('\0');
    result.append(char(flags));
    result += syncSafe(quint32(body.size()));
    result += body;
    return result;
}

QByteArray utf16Be(const QString& value, bool bom = false)
{
    QByteArray result;
    if (bom)
        result.append("\xfe\xff", 2);
    for (const QChar c : value) {
        result.append(char(c.unicode() >> 8));
        result.append(char(c.unicode()));
    }
    return result;
}

QByteArray utf16Le(const QString& value, bool bom = true)
{
    QByteArray result;
    if (bom)
        result.append("\xff\xfe", 2);
    for (const QChar c : value) {
        result.append(char(c.unicode()));
        result.append(char(c.unicode() >> 8));
    }
    return result;
}

QByteArray makeV1(const QByteArray& title, const QByteArray& artist, const QByteArray& album,
                  int track = 0)
{
    QByteArray tag(128, '\0');
    tag.replace(0, 3, "TAG");
    tag.replace(3, qMin(30, title.size()), title.left(30));
    tag.replace(33, qMin(30, artist.size()), artist.left(30));
    tag.replace(63, qMin(30, album.size()), album.left(30));
    if (track > 0) {
        tag[125] = '\0';
        tag[126] = char(track);
    }
    return tag;
}

QString writeFixture(QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not write synthetic fixture");
    return path;
}

} // namespace

class TestLibraryParsing : public QObject {
    Q_OBJECT

private slots:
    void zipStoredPair();
    void zipMethodsAndNames();
    void zipDamageAndComments();
    void zip64();
    void zipShiftedOffsets();
    void id3VersionsAndEncodings();
    void id3UnsynchronisationAndExtendedHeader();
    void id3V1AndFileMerge();
    void id3DamageAndPlaceholders();
    void filenameExamples_data();
    void filenameExamples();
    void filenameEdges();
    void searchNormalizationAndFallback();
};

void TestLibraryParsing::zipStoredPair()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = writeFixture(directory, "pair.zip",
                                      makeZip({{"song.mp3", 0, 0, "audio"},
                                               {"song.cdg", 0, 0, "lyrics"}}));
    const ZipDirectoryResult result = readZipDirectory(path);
    QCOMPARE(result.status, ZipStatus::Ok);
    QCOMPARE(result.members.size(), 2);
    QCOMPARE(result.members.at(0).name, QStringLiteral("song.mp3"));
    QCOMPARE(result.members.at(0).uncompressedSize, quint64(5));
    QCOMPARE(result.members.at(1).localHeaderOffset, quint64(43));
    QVERIFY(zipMethodSupported(0));
    QVERIFY(zipMethodSupported(8));
    QVERIFY(!zipMethodSupported(9));
}

void TestLibraryParsing::zipMethodsAndNames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray cpName("caf");
    cpName.append(char(0x82));
    cpName += ".mp3";
    const QString path = writeFixture(
        directory, "names.zip",
        makeZip({{"folder/", 0, 0, {}},
                 {QByteArray("folder/\xc3\x85se.mp3"), 8, 0x0801, "compressed"},
                 {cpName, 9, 0, "deflate64"}}));
    const ZipDirectoryResult result = readZipDirectory(path);
    QCOMPARE(result.status, ZipStatus::Ok);
    QCOMPARE(result.members.at(0).name, QStringLiteral("folder/"));
    QVERIFY(result.members.at(0).isDirectory);
    QCOMPARE(result.members.at(1).name, QString::fromUtf8("folder/Åse.mp3"));
    QCOMPARE(result.members.at(1).method, quint16(8));
    QVERIFY(result.members.at(1).encrypted);
    QCOMPARE(result.members.at(2).name, QString::fromUtf8("café.mp3"));
    QCOMPARE(result.members.at(2).method, quint16(9));
}

void TestLibraryParsing::zipDamageAndComments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray complete = makeZip({{"song.mp3"}}, "a comment");
    QCOMPARE(readZipDirectory(writeFixture(directory, "comment.zip", complete)).status, ZipStatus::Ok);

    const QByteArray truncated = complete.left(35);
    QCOMPARE(readZipDirectory(writeFixture(directory, "truncated.zip", truncated)).status,
             ZipStatus::Truncated);
    QCOMPARE(readZipDirectory(writeFixture(directory, "empty.zip", {})).status, ZipStatus::NotZip);
    QCOMPARE(readZipDirectory(writeFixture(directory, "garbage.zip", "not a zip")).status,
             ZipStatus::NotZip);
    QCOMPARE(readZipDirectory(writeFixture(directory, "lie.zip", makeZip({{"one"}}, {}, false, 1))).status,
             ZipStatus::Corrupt);
    QCOMPARE(readZipDirectory(directory.filePath("missing.zip")).status, ZipStatus::Unreadable);
}

void TestLibraryParsing::zip64()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const ZipDirectoryResult result = readZipDirectory(
        writeFixture(directory, "zip64.zip", makeZip({{"nested/song.mp3", 0, 0, "abc"}}, {}, true)));
    QCOMPARE(result.status, ZipStatus::Ok);
    QCOMPARE(result.members.size(), 1);
    QCOMPARE(result.members.first().name, QStringLiteral("nested/song.mp3"));
    QCOMPARE(result.members.first().compressedSize, quint64(3));
    QCOMPARE(result.members.first().localHeaderOffset, quint64(0));
}

void TestLibraryParsing::zipShiftedOffsets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray original = makeZip({{"first.mp3", 0, 0, "audio"},
                                         {"first.cdg", 0, 0, "lyrics"}});

    QByteArray prepended("prefix-data");
    prepended += original;
    ZipDirectoryResult result = readZipDirectory(
        writeFixture(directory, QStringLiteral("prepended.zip"), prepended));
    QCOMPARE(result.status, ZipStatus::Corrupt);
    QCOMPARE(result.members.size(), 2);
    QVERIFY(!result.members.at(0).damaged);
    QVERIFY(!result.members.at(1).damaged);
    QCOMPARE(result.members.at(0).localHeaderOffset, quint64(11));

    QByteArray missing = original.mid(10);
    result = readZipDirectory(writeFixture(directory, QStringLiteral("missing-prefix.zip"), missing));
    QCOMPARE(result.status, ZipStatus::Corrupt);
    QCOMPARE(result.members.size(), 2);
    QVERIFY(result.members.at(0).damaged);
    QVERIFY(!result.members.at(1).damaged);
    QVERIFY(result.detail.contains(QStringLiteral("shifted by -10")));
}

void TestLibraryParsing::id3VersionsAndEncodings()
{
    QByteArray body = v22Frame("TT2", textPayload(0, QByteArray("Caf\xe9")));
    body += v22Frame("TP1", textPayload(1, utf16Le(QStringLiteral("Björk"))));
    Id3Tags tags = parseId3v2(id3Tag(2, body));
    QCOMPARE(tags.version, QStringLiteral("2.2"));
    QCOMPARE(tags.title, QStringLiteral("Café"));
    QCOMPARE(tags.artist, QStringLiteral("Björk"));

    body = v23Frame("TIT2", textPayload(2, utf16Be(QStringLiteral("東京"))));
    body += v23Frame("TPE1", textPayload(0, "Artist"));
    body += v23Frame("TALB", textPayload(3, "Album"));
    body += v23Frame("TPE2", textPayload(3, "Album Artist"));
    body += v23Frame("TRCK", textPayload(0, "07/20"));
    tags = parseId3v2(id3Tag(3, body));
    QCOMPARE(tags.version, QStringLiteral("2.3"));
    QCOMPARE(tags.title, QStringLiteral("東京"));
    QCOMPARE(tags.artist, QStringLiteral("Artist"));
    QCOMPARE(tags.album, QStringLiteral("Album"));
    QCOMPARE(tags.albumArtist, QStringLiteral("Album Artist"));
    QCOMPARE(tags.track, QStringLiteral("07/20"));

    tags = parseId3v2(id3Tag(4, v24Frame("TIT2", textPayload(3, "Grüße"))));
    QCOMPARE(tags.version, QStringLiteral("2.4"));
    QCOMPARE(tags.title, QString::fromUtf8("Grüße"));

    QByteArray v24Extended = syncSafe(6);
    v24Extended.append('\1');
    v24Extended.append('\0');
    v24Extended += v24Frame("TIT2", textPayload(3, "Extended v2.4"));
    tags = parseId3v2(id3Tag(4, v24Extended, 0x40));
    QCOMPARE(tags.title, QStringLiteral("Extended v2.4"));

    QByteArray buggyFrame("TIT2", 4);
    buggyFrame.append("\0\0\1\0", 4); // raw 256, incorrectly not syncsafe
    buggyFrame.append("\0\0", 2);
    buggyFrame.append('\0');
    buggyFrame.append(QByteArray(255, 'a'));
    tags = parseId3v2(id3Tag(4, buggyFrame));
    QCOMPARE(tags.title, QString(255, QLatin1Char('a')));
}

void TestLibraryParsing::id3UnsynchronisationAndExtendedHeader()
{
    QByteArray unsynchronised;
    unsynchronised.append('\0');
    unsynchronised.append('A');
    unsynchronised.append(char(0xff));
    unsynchronised.append('B');
    QByteArray unsynchronisedFrame = v23Frame("TIT2", unsynchronised);
    unsynchronisedFrame.insert(unsynchronisedFrame.indexOf(char(0xff)) + 1, '\0');
    Id3Tags tags = parseId3v2(id3Tag(3, unsynchronisedFrame, 0x80));
    QCOMPARE(tags.title.size(), 3);
    QCOMPARE(tags.title.at(0), QChar('A'));
    QCOMPARE(tags.title.at(1), QChar(0x00ff));
    QCOMPARE(tags.title.at(2), QChar('B'));

    QByteArray extended;
    extended.append("\0\0\0\6", 4);
    extended.append(QByteArray(6, '\0'));
    extended += v23Frame("TIT2", textPayload(0, "After header"));
    tags = parseId3v2(id3Tag(3, extended, 0x40));
    QCOMPARE(tags.title, QStringLiteral("After header"));

    QByteArray perFrame;
    perFrame.append('\0');
    perFrame.append('X');
    perFrame.append(char(0xff));
    perFrame.append('\0');
    perFrame.append('Y');
    tags = parseId3v2(id3Tag(4, v24Frame("TIT2", perFrame, 0x0002)));
    QCOMPARE(tags.title.size(), 3);
    QCOMPARE(tags.title.at(1), QChar(0x00ff));
}

void TestLibraryParsing::id3V1AndFileMerge()
{
    const Id3Tags v1 = parseId3v1(makeV1(" Old title  ", QByteArray("Beyonc\xe9"), "Album", 12));
    QVERIFY(v1.hasV1);
    QCOMPARE(v1.version, QStringLiteral("1.1"));
    QCOMPARE(v1.title, QStringLiteral("Old title"));
    QCOMPARE(v1.artist, QStringLiteral("Beyoncé"));
    QCOMPARE(v1.track, QStringLiteral("12"));
    const Id3Tags plainV1 = parseId3v1(makeV1("Title", "Artist", "Album"));
    QCOMPARE(plainV1.version, QStringLiteral("1"));
    QVERIFY(plainV1.track.isEmpty());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray file = id3Tag(4, v24Frame("TIT2", textPayload(3, "V2 title")));
    file += QByteArray(300, 'm');
    file += makeV1("V1 title", "V1 artist", "V1 album", 4);
    const Id3Tags merged = readId3Tags(writeFixture(directory, "song.mp3", file));
    QVERIFY(merged.hasV1);
    QVERIFY(merged.hasV2);
    QCOMPARE(merged.version, QStringLiteral("2.4"));
    QCOMPARE(merged.title, QStringLiteral("V2 title"));
    QCOMPARE(merged.artist, QStringLiteral("V1 artist"));
    QCOMPARE(merged.album, QStringLiteral("V1 album"));
    QCOMPARE(merged.track, QStringLiteral("4"));
}

void TestLibraryParsing::id3DamageAndPlaceholders()
{
    QVERIFY(parseId3v2("garbage").isEmpty());
    QByteArray invalidHeader("ID3\4\0\0\0\0\0\0", 10);
    invalidHeader[6] = char(0x80);
    QVERIFY(!parseId3v2(invalidHeader).hasV2);
    const Id3Tags truncated = parseId3v2(id3Tag(3, v23Frame("TIT2", textPayload(0, "title"))).chopped(3));
    QVERIFY(truncated.hasV2);
    QVERIFY(truncated.title.isEmpty());
    QVERIFY(parseId3v1(QByteArray(127, '\0')).isEmpty());
    QVERIFY(isPlaceholderTagValue(QStringLiteral(" No Artist ")));
    QVERIFY(isPlaceholderTagValue(QStringLiteral("Track 10")));
    QVERIFY(isPlaceholderTagValue(QString()));
    QVERIFY(!isPlaceholderTagValue(QStringLiteral("Sparky001")));
}

void TestLibraryParsing::filenameExamples_data()
{
    QTest::addColumn<QString>("directory");
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("disc");
    QTest::addColumn<int>("track");
    QTest::addColumn<QStringList>("fields");
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("source");

    using K = ParsedName::Kind;
    auto row = [](const char* name, const QString& dir, const QString& file, const QString& disc,
                  int track, const QStringList& fields, K kind, const QString& source) {
        QTest::newRow(name) << dir << file << disc << track << fields << int(kind) << source;
    };
    row("mh", {}, "Mh1131-05 - Kroeger, Chad & Josey Scott - Hero.mp3", "MH1131", 5,
        {"Kroeger, Chad & Josey Scott", "Hero"}, K::DiscTrackFields, "filename");
    row("dk", {}, "DK022-18 - He'll Have To Go - Reeves, Jim .mp3", "DK022", 18,
        {"He'll Have To Go", "Reeves, Jim"}, K::DiscTrackFields, "filename");
    row("zkh", {}, "ZKH 005-11 - McCartney, Paul & Wings - Live And Let Die.mp3", "ZKH005", 11,
        {"McCartney, Paul & Wings", "Live And Let Die"}, K::DiscTrackFields, "filename");
    row("underscores", {}, "SF273-15_-_Cliff_Richard_-_Thank_You_For_A_Lifetime.mp3", "SF273", 15,
        {"Cliff Richard", "Thank You For A Lifetime"}, K::DiscTrackFields, "filename");
    row("sunfly", {}, "sunfly-069-04.mp3", "SUNFLY069", 4, {}, K::DiscTrackOnly, "filename");
    row("us2", {}, "Us2-001-06 - Tenth Avenue Freezeout - Springsteen, Bruce.mp3", "US2001", 6,
        {"Tenth Avenue Freezeout", "Springsteen, Bruce"}, K::DiscTrackFields, "filename");
    row("three part then space", {}, "Abcd10-1-07 Singer, Some - A Song.mp3", "ABCD101", 7,
        {"Singer, Some", "A Song"}, K::DiscTrackFields, "filename");
    row("dotted prefix", {}, "B.Name04-03-A Song Title - Some Singer.mp3", "BNAME04", 3,
        {"A Song Title", "Some Singer"}, K::DiscTrackFields, "filename");
    row("sfmw", {}, "Sfmw-852-15-Alanis Morrissette - Everything.mp3", "SFMW852", 15,
        {"Alanis Morrissette", "Everything"}, K::DiscTrackFields, "filename");
    row("zmp", {}, "zmp017-02-HELLO AGAIN - NEIL DIAMOND.mp3", "ZMP017", 2,
        {"HELLO AGAIN", "NEIL DIAMOND"}, K::DiscTrackFields, "filename");
    row("thmc", {}, "thmc0506-19 dalley, amy - i would cry.mp3", "THMC0506", 19,
        {"dalley, amy", "i would cry"}, K::DiscTrackFields, "filename");
    row("sfg", {}, "Sfg012 14 - Gabrielle - Out Of Reach.mp3", "SFG012", 14,
        {"Gabrielle", "Out Of Reach"}, K::DiscTrackFields, "filename");
    row("leading-catalog-digits", {}, "11EZH044-11 - Return to Sender - Elvis Presley.mp3", "11EZH044", 11,
        {"Return to Sender", "Elvis Presley"}, K::DiscTrackFields, "filename");
    row("ck", {}, "ck04-06 - mental as anything - nips are getting bigger.mp3", "CK04", 6,
        {"mental as anything", "nips are getting bigger"}, K::DiscTrackFields, "filename");
    row("cbse", {}, "CBSE5-0391_-_Nitty_Gritty_Dirt_Band,_The_-_Fishing_In_The_Dark.mp3", "CBSE5", 391,
        {"Nitty Gritty Dirt Band, The", "Fishing In The Dark"}, K::DiscTrackFields, "filename");
    row("kpp", {}, "Kpp01-01 Martin, Ricky - Livin' La Vida Loca.mp3", "KPP01", 1,
        {"Martin, Ricky", "Livin' La Vida Loca"}, K::DiscTrackFields, "filename");
    row("folder-disc", "Legends/LEG 099", "14 - Use Me - Bill Withers.mp3", "LEG099", 14,
        {"Use Me", "Bill Withers"}, K::DiscTrackFields, "folder");
    row("dot-track", "SF Gold 19", "11. Saw Doctors - Joyce Country Ceili Band.mp3", "SFGOLD19", 11,
        {"Saw Doctors", "Joyce Country Ceili Band"}, K::DiscTrackFields, "folder");
    row("et", {}, "ET26_04.mp3", "ET26", 4, {}, K::DiscTrackOnly, "filename");
    row("ezh-space", {}, "ezh37 -01.mp3", "EZH37", 1, {}, K::DiscTrackOnly, "filename");
    row("track-label", "SF 087", "Track 15.mp3", "SF087", 15, {}, K::TrackOnly, "folder");
    row("track-tight", "824", "Track11.mp3", "824", 11, {}, K::TrackOnly, "folder");
    row("track-minus", "Vol 22", "Track -03.mp3", "VOL22", 3, {}, K::TrackOnly, "folder");
    row("vol-track", "Vol 22", "vol 22 - track -02.mp3", "VOL22", 2, {}, K::TrackOnly, "folder");
    row("packed", "ezh 8", "EZH00807.mp3", "EZH008", 7, {}, K::DiscTrackOnly, "filename");
    row("ambiguous-packed", {}, "pm00411.mp3", {}, 0, {"pm00411"}, K::FreeText, {});
    row("sav", {}, "sav 518.mp3", {}, 0, {"sav 518"}, K::FreeText, {});
    row("numeric-prefixed", "Sweet Georgia Brown/sgb39", "3902.mp3", "SGB39", 2, {}, K::NumericOnly, "folder");
    row("numeric-folder", "sunfly most wanted/833", "13.mp3", "833", 13, {}, K::NumericOnly, "folder");
    row("numeric-short", "sgb4", "10.mp3", "SGB4", 10, {}, K::NumericOnly, "folder");
    row("two-fields", {}, "Artist - Title.mp3", {}, 0, {"Artist", "Title"}, K::Fields, {});
    row("tight-hyphen", {}, "lonestar-amazed.mp3", {}, 0, {"lonestar-amazed"}, K::FreeText, {});
    row("double-hyphen", {}, "8--3.mp3", {}, 0, {"8--3"}, K::FreeText, {});
    row("suffix", {}, "Singer - Song (Pro) Wvocal (Kararadio).mp3", {}, 0,
        {"Singer", "Song (Pro) Wvocal (Kararadio)"}, K::Fields, {});
}

void TestLibraryParsing::filenameExamples()
{
    QFETCH(QString, directory);
    QFETCH(QString, fileName);
    QFETCH(QString, disc);
    QFETCH(int, track);
    QFETCH(QStringList, fields);
    QFETCH(int, kind);
    QFETCH(QString, source);
    const ParsedName parsed = parseSongName(directory, fileName);
    QCOMPARE(parsed.discId, disc);
    QString expectedPrefix;
    for (const QChar c : disc) {
        if (c.isLetter())
            expectedPrefix.append(c);
    }
    QCOMPARE(parsed.discPrefix, expectedPrefix);
    QCOMPARE(parsed.track, track);
    QCOMPARE(parsed.fields, fields);
    QCOMPARE(int(parsed.kind), kind);
    QCOMPARE(parsed.discSource, source);
}

void TestLibraryParsing::filenameEdges()
{
    ParsedName parsed = parseSongName({}, {});
    QVERIFY(parsed.stem.isEmpty());
    QVERIFY(parsed.cleaned.isEmpty());
    QCOMPARE(parsed.kind, ParsedName::Kind::FreeText);

    parsed = parseSongName({}, QStringLiteral("   .mp3"));
    QVERIFY(parsed.cleaned.isEmpty());
    parsed = parseSongName({}, QStringLiteral(".mp3"));
    QVERIFY(parsed.cleaned.isEmpty() || parsed.cleaned == QLatin1String(".mp3"));

    parsed = parseSongName({}, QStringLiteral("Utils-95 applause.mp3"));
    QVERIFY(parsed.discId.isEmpty());
    QCOMPARE(parsed.track, 0);
    parsed = parseSongName({}, QStringLiteral("Vol 14 - Track -12.mp3"));
    QVERIFY(parsed.discId.isEmpty());
    QCOMPARE(parsed.track, 12);
    QCOMPARE(parsed.kind, ParsedName::Kind::TrackOnly);

    const QString nonAscii = QString::fromUtf8("Blåbær\u00a0– Don’t Stop ÿ.mp3");
    parsed = parseSongName({}, nonAscii);
    QVERIFY(parsed.cleaned.contains(QStringLiteral("Blåbær")));
    QVERIFY(parsed.cleaned.contains(QStringLiteral("ÿ")));

    const QString longName(20'000, QLatin1Char('a'));
    parsed = parseSongName({}, longName + QStringLiteral(".mp3"));
    QCOMPARE(parsed.cleaned.size(), longName.size());
    parsed = parseSongName({}, QStringLiteral("123.mp3"));
    QCOMPARE(parsed.kind, ParsedName::Kind::NumericOnly);
    QCOMPARE(parsed.track, 123);

    parsed = parseSongName({}, QStringLiteral("SINP05SPR-15 - Gentle Song - Example Band.mp3"));
    QCOMPARE(parsed.discId, QStringLiteral("SINP05SPR"));
    QCOMPARE(parsed.track, 15);
    QCOMPARE(parsed.fields, QStringList({QStringLiteral("Gentle Song"),
                                        QStringLiteral("Example Band")}));

    parsed = parseSongName(QStringLiteral("sunfly most wanted/818/Track14"),
                           QStringLiteral("Track14.mp3"));
    QCOMPARE(parsed.discId, QStringLiteral("818"));
    QCOMPARE(parsed.track, 14);

    parsed = parseSongName({}, QStringLiteral("H1B-10 - 7 To 9 - Surname, Given.mp3"));
    QCOMPARE(parsed.discId, QStringLiteral("H1B"));
    QCOMPARE(parsed.track, 10);
    QCOMPARE(parsed.fields.first(), QStringLiteral("7 To 9"));

    parsed = parseSongName({}, QStringLiteral("1999 - Prince.mp3"));
    QCOMPARE(parsed.track, 0);
    QCOMPARE(parsed.fields, QStringList({QStringLiteral("1999"), QStringLiteral("Prince")}));

    parsed = parseSongName({}, QStringLiteral("SC8759-03 - 40,000 Stories - Surname, Given.mp3"));
    QCOMPARE(parsed.discId, QStringLiteral("SC8759"));
    QCOMPARE(parsed.track, 3);
    QCOMPARE(parsed.fields.first(), QStringLiteral("40,000 Stories"));

    parsed = parseSongName({}, QStringLiteral("H1B-10 - Island Song - Coastal Group.mp3"));
    QCOMPARE(parsed.fields, QStringList({QStringLiteral("Island Song"),
                                        QStringLiteral("Coastal Group")}));
}

void TestLibraryParsing::searchNormalizationAndFallback()
{
    QCOMPARE(normalizeForSearch(QString::fromUtf8("Beyoncé & Mötley Crüe — Don’t_Stop!")),
             QStringLiteral("beyonce and motley crue don t stop"));
    QCOMPARE(normalizeForSearch(QStringLiteral("  A...B  ")), QStringLiteral("a b"));

    ParsedName parsed = parseSongName({}, QStringLiteral("SGB39-02.mp3"));
    QCOMPARE(fallbackTitle(parsed), QStringLiteral("Disc SGB39 - Track 02"));
    parsed = parseSongName({}, QStringLiteral("Track 7.mp3"));
    QCOMPARE(fallbackTitle(parsed), QStringLiteral("Track 07"));
    parsed = parseSongName(QStringLiteral("Packs/EK TEEN PACK 02/CD1"),
                           QStringLiteral("Track 02.mp3"));
    QVERIFY(parsed.discId.isEmpty());
    QCOMPARE(fallbackTitle(parsed), QStringLiteral("EK TEEN PACK 02 - Track 02"));
    parsed = parseSongName({}, QStringLiteral("It's My Life.mp3"));
    QCOMPARE(fallbackTitle(parsed), QStringLiteral("It's My Life"));
}

QTEST_GUILESS_MAIN(TestLibraryParsing)
#include "tst_libraryparsing.moc"
