#include "library/Id3Reader.h"

#include <QFile>
#include <QRegularExpression>

namespace {

constexpr qsizetype MaximumTagBytes = 256 * 1024;

quint32 be24(const QByteArray& bytes, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return (quint32(p[0]) << 16) | (quint32(p[1]) << 8) | quint32(p[2]);
}

quint32 be32(const QByteArray& bytes, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8)
        | quint32(p[3]);
}

bool syncSafe(const QByteArray& bytes, qsizetype offset, quint32& value)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80U)
        return false;
    value = (quint32(p[0]) << 21) | (quint32(p[1]) << 14) | (quint32(p[2]) << 7)
        | quint32(p[3]);
    return true;
}

QByteArray deUnsynchronise(const QByteArray& input)
{
    QByteArray output;
    output.reserve(input.size());
    for (qsizetype i = 0; i < input.size(); ++i) {
        output.append(input.at(i));
        if (uchar(input.at(i)) == 0xffU && i + 1 < input.size() && input.at(i + 1) == '\0')
            ++i;
    }
    return output;
}

QString decodeUtf16(const QByteArray& bytes, bool bigEndian)
{
    QString result;
    result.reserve(bytes.size() / 2);
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
        const uchar first = uchar(bytes.at(i));
        const uchar second = uchar(bytes.at(i + 1));
        result.append(QChar(bigEndian ? quint16((first << 8) | second)
                                     : quint16((second << 8) | first)));
    }
    return result;
}

QString decodeTextFrame(const QByteArray& payload)
{
    if (payload.isEmpty())
        return {};
    const uchar encoding = uchar(payload.at(0));
    QByteArray text = payload.mid(1);
    QString result;
    switch (encoding) {
    case 0:
        result = QString::fromLatin1(text);
        break;
    case 1:
        if (text.startsWith("\xff\xfe"))
            result = decodeUtf16(text.mid(2), false);
        else if (text.startsWith("\xfe\xff"))
            result = decodeUtf16(text.mid(2), true);
        else
            result = decodeUtf16(text, true);
        break;
    case 2:
        result = decodeUtf16(text, true);
        break;
    case 3:
        result = QString::fromUtf8(text);
        break;
    default:
        return {};
    }
    result.remove(QChar::Null);
    return result.trimmed();
}

bool plausibleFrameId(QByteArrayView id)
{
    if (id.isEmpty())
        return false;
    for (const char c : id) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

bool plausibleFrameBoundary(const QByteArray& body, qsizetype position, qsizetype idSize)
{
    if (position == body.size())
        return true;
    if (position < 0 || position > body.size() - idSize)
        return false;
    const QByteArray id = body.mid(position, idSize);
    return id == QByteArray(idSize, '\0') || plausibleFrameId(id);
}

void assignFrame(Id3Tags& tags, const QByteArray& id, const QString& value)
{
    if (id == "TIT2" || id == "TT2")
        tags.title = value;
    else if (id == "TPE1" || id == "TP1")
        tags.artist = value;
    else if (id == "TALB" || id == "TAL")
        tags.album = value;
    else if (id == "TPE2" || id == "TP2")
        tags.albumArtist = value;
    else if (id == "TRCK" || id == "TRK")
        tags.track = value;
}

QString latin1Field(const QByteArray& bytes, qsizetype offset, qsizetype length)
{
    QString value = QString::fromLatin1(bytes.constData() + offset, length);
    value.remove(QChar::Null);
    return value.trimmed();
}

} // namespace

bool Id3Tags::isEmpty() const
{
    return title.isEmpty() && artist.isEmpty() && album.isEmpty() && albumArtist.isEmpty()
        && track.isEmpty();
}

Id3Tags parseId3v2(const QByteArray& head)
{
    Id3Tags tags;
    if (head.size() < 10 || !head.startsWith("ID3"))
        return tags;

    const uchar major = uchar(head.at(3));
    if (major < 2 || major > 4)
        return tags;
    quint32 declaredSize = 0;
    if (!syncSafe(head, 6, declaredSize))
        return tags;

    tags.hasV2 = true;
    tags.version = QStringLiteral("2.%1").arg(major);
    const qsizetype available = qMin<qsizetype>(declaredSize, head.size() - 10);
    QByteArray body = head.mid(10, available);
    const uchar headerFlags = uchar(head.at(5));
    const bool tagUnsynchronised = (headerFlags & 0x80U) != 0;
    if (tagUnsynchronised && major <= 3)
        body = deUnsynchronise(body);

    qsizetype position = 0;
    if ((headerFlags & 0x40U) && major >= 3) {
        if (body.size() < 4)
            return tags;
        quint32 extendedSize = 0;
        if (major == 3) {
            extendedSize = be32(body, 0);
            extendedSize += 4; // v2.3 excludes its own size field.
        } else if (!syncSafe(body, 0, extendedSize)) {
            return tags;
        }
        if (extendedSize < 4 || extendedSize > quint32(body.size()))
            return tags;
        position = qsizetype(extendedSize);
    }

    const qsizetype headerSize = major == 2 ? 6 : 10;
    const qsizetype idSize = major == 2 ? 3 : 4;
    while (position <= body.size() - headerSize) {
        const QByteArray id = body.mid(position, idSize);
        if (id == QByteArray(idSize, '\0'))
            break;
        if (!plausibleFrameId(id))
            break;

        quint32 frameSize = 0;
        quint16 frameFlags = 0;
        if (major == 2) {
            frameSize = be24(body, position + 3);
        } else if (major == 3) {
            frameSize = be32(body, position + 4);
            frameFlags = quint16(uchar(body.at(position + 8)) << 8)
                | uchar(body.at(position + 9));
        } else {
            quint32 syncSize = 0;
            const bool validSync = syncSafe(body, position + 4, syncSize);
            const quint32 rawSize = be32(body, position + 4);
            const qsizetype remaining = body.size() - position - headerSize;
            frameSize = validSync ? syncSize : rawSize;
            // Some v2.4 writers emit v2.3-style sizes. Prefer that value only
            // when the syncsafe interpretation cannot fit, or when only the
            // raw-size boundary lands on a plausible next frame/padding.
            if (frameSize > quint32(remaining) && rawSize <= quint32(remaining))
                frameSize = rawSize;
            else if (validSync && rawSize != syncSize && rawSize <= quint32(remaining)
                     && !plausibleFrameBoundary(body, position + headerSize + syncSize, idSize)
                     && plausibleFrameBoundary(body, position + headerSize + rawSize, idSize))
                frameSize = rawSize;
            frameFlags = quint16(uchar(body.at(position + 8)) << 8)
                | uchar(body.at(position + 9));
        }

        position += headerSize;
        if (frameSize > quint32(body.size() - position))
            break;
        QByteArray payload = body.mid(position, frameSize);
        position += frameSize;

        if (major == 3 && (frameFlags & 0x00c0U))
            continue; // Compression or encryption is outside this small reader.
        if (major == 4) {
            if (frameFlags & 0x000cU)
                continue;
            if ((frameFlags & 0x0040U) && !payload.isEmpty())
                payload.remove(0, 1);
            if ((frameFlags & 0x0001U) && payload.size() >= 4)
                payload.remove(0, 4);
            if (tagUnsynchronised || (frameFlags & 0x0002U))
                payload = deUnsynchronise(payload);
        }
        assignFrame(tags, id, decodeTextFrame(payload));
    }
    return tags;
}

Id3Tags parseId3v1(const QByteArray& tail128)
{
    Id3Tags tags;
    if (tail128.size() != 128 || !tail128.startsWith("TAG"))
        return tags;
    tags.hasV1 = true;
    tags.title = latin1Field(tail128, 3, 30);
    tags.artist = latin1Field(tail128, 33, 30);
    tags.album = latin1Field(tail128, 63, 30);
    if (tail128.at(125) == '\0' && uchar(tail128.at(126)) != 0) {
        tags.version = QStringLiteral("1.1");
        tags.track = QString::number(uchar(tail128.at(126)));
    } else {
        tags.version = QStringLiteral("1");
    }
    return tags;
}

Id3Tags readId3Tags(const QString& mp3Path)
{
    QFile file(mp3Path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray head = file.read(10);
    if (head.size() == 10 && head.startsWith("ID3")) {
        quint32 tagSize = 0;
        if (syncSafe(head, 6, tagSize)) {
            const qsizetype wanted = qMin<qsizetype>(tagSize, MaximumTagBytes - 10);
            head += file.read(wanted);
        }
    }
    Id3Tags v2 = parseId3v2(head);

    Id3Tags v1;
    if (file.size() >= 128 && file.seek(file.size() - 128))
        v1 = parseId3v1(file.read(128));
    if (!v2.hasV2)
        return v1;

    v2.hasV1 = v1.hasV1;
    if (v2.title.isEmpty())
        v2.title = v1.title;
    if (v2.artist.isEmpty())
        v2.artist = v1.artist;
    if (v2.album.isEmpty())
        v2.album = v1.album;
    if (v2.track.isEmpty())
        v2.track = v1.track;
    return v2;
}

bool isPlaceholderTagValue(const QString& value)
{
    const QString folded = value.trimmed().toLower();
    if (folded.isEmpty())
        return true;
    static const QRegularExpression trackNumber(QStringLiteral(R"(^track\s+\d+$)"));
    return folded == QLatin1String("artist") || folded == QLatin1String("no artist")
        || folded == QLatin1String("unknown") || folded == QLatin1String("unknown artist")
        || folded == QLatin1String("title") || folded == QLatin1String("untitled")
        || trackNumber.match(folded).hasMatch();
}
