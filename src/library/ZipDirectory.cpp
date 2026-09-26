#include "library/ZipDirectory.h"

#include <QFile>

#include <array>
#include <limits>
#include <utility>

namespace {

constexpr quint32 LocalHeaderSignature = 0x04034b50;
constexpr quint32 CentralHeaderSignature = 0x02014b50;
constexpr quint32 EocdSignature = 0x06054b50;
constexpr quint32 Zip64EocdSignature = 0x06064b50;
constexpr quint32 Zip64LocatorSignature = 0x07064b50;
constexpr quint32 CentralDigitalSignature = 0x05054b50;
constexpr quint64 MaximumEntries = 100'000;
constexpr quint64 MaximumDirectorySize = 64 * 1024 * 1024;
constexpr qint64 MaximumTailSize = 65'535 + 22;

bool hasBytes(const QByteArray& bytes, qsizetype offset, qsizetype count)
{
    return offset >= 0 && count >= 0 && offset <= bytes.size() && count <= bytes.size() - offset;
}

quint16 le16(const QByteArray& bytes, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& bytes, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16)
        | (quint32(p[3]) << 24);
}

quint64 le64(const QByteArray& bytes, qsizetype offset)
{
    return quint64(le32(bytes, offset)) | (quint64(le32(bytes, offset + 4)) << 32);
}

bool readAt(QFile& file, quint64 offset, qsizetype count, QByteArray& output)
{
    if (offset > quint64(std::numeric_limits<qint64>::max())
        || count < 0
        || offset + quint64(count) < offset
        || offset + quint64(count) > quint64(file.size()))
        return false;
    if (!file.seek(qint64(offset)))
        return false;
    output = file.read(count);
    return output.size() == count;
}

QString decodeCp437(const QByteArray& bytes)
{
    // Unicode mappings for CP437 bytes 0x80..0xff.  Keeping the table local
    // avoids relying on an optional platform text codec.
    static constexpr std::array<char16_t, 128> high = {
        u'\u00c7', u'\u00fc', u'\u00e9', u'\u00e2', u'\u00e4', u'\u00e0', u'\u00e5', u'\u00e7',
        u'\u00ea', u'\u00eb', u'\u00e8', u'\u00ef', u'\u00ee', u'\u00ec', u'\u00c4', u'\u00c5',
        u'\u00c9', u'\u00e6', u'\u00c6', u'\u00f4', u'\u00f6', u'\u00f2', u'\u00fb', u'\u00f9',
        u'\u00ff', u'\u00d6', u'\u00dc', u'\u00a2', u'\u00a3', u'\u00a5', u'\u20a7', u'\u0192',
        u'\u00e1', u'\u00ed', u'\u00f3', u'\u00fa', u'\u00f1', u'\u00d1', u'\u00aa', u'\u00ba',
        u'\u00bf', u'\u2310', u'\u00ac', u'\u00bd', u'\u00bc', u'\u00a1', u'\u00ab', u'\u00bb',
        u'\u2591', u'\u2592', u'\u2593', u'\u2502', u'\u2524', u'\u2561', u'\u2562', u'\u2556',
        u'\u2555', u'\u2563', u'\u2551', u'\u2557', u'\u255d', u'\u255c', u'\u255b', u'\u2510',
        u'\u2514', u'\u2534', u'\u252c', u'\u251c', u'\u2500', u'\u253c', u'\u255e', u'\u255f',
        u'\u255a', u'\u2554', u'\u2569', u'\u2566', u'\u2560', u'\u2550', u'\u256c', u'\u2567',
        u'\u2568', u'\u2564', u'\u2565', u'\u2559', u'\u2558', u'\u2552', u'\u2553', u'\u256b',
        u'\u256a', u'\u2518', u'\u250c', u'\u2588', u'\u2584', u'\u258c', u'\u2590', u'\u2580',
        u'\u03b1', u'\u00df', u'\u0393', u'\u03c0', u'\u03a3', u'\u03c3', u'\u00b5', u'\u03c4',
        u'\u03a6', u'\u0398', u'\u03a9', u'\u03b4', u'\u221e', u'\u03c6', u'\u03b5', u'\u2229',
        u'\u2261', u'\u00b1', u'\u2265', u'\u2264', u'\u2320', u'\u2321', u'\u00f7', u'\u2248',
        u'\u00b0', u'\u2219', u'\u00b7', u'\u221a', u'\u207f', u'\u00b2', u'\u25a0', u'\u00a0'
    };

    QString result;
    result.reserve(bytes.size());
    for (const uchar byte : bytes) {
        result.append(byte < 0x80 ? QChar(byte) : QChar(high[byte - 0x80]));
    }
    return result;
}

bool applyZip64Extra(const QByteArray& extra, quint32 rawUncompressed, quint32 rawCompressed,
                     quint32 rawOffset, quint16 rawDisk, ZipMember& member)
{
    qsizetype position = 0;
    bool foundZip64 = false;
    while (hasBytes(extra, position, 4)) {
        const quint16 id = le16(extra, position);
        const quint16 size = le16(extra, position + 2);
        position += 4;
        if (!hasBytes(extra, position, size))
            return false;
        if (id == 0x0001) {
            foundZip64 = true;
            qsizetype value = position;
            const qsizetype end = position + size;
            auto take64 = [&](quint64& target) {
                if (value > end - 8)
                    return false;
                target = le64(extra, value);
                value += 8;
                return true;
            };
            if (rawUncompressed == 0xffffffffU && !take64(member.uncompressedSize))
                return false;
            if (rawCompressed == 0xffffffffU && !take64(member.compressedSize))
                return false;
            if (rawOffset == 0xffffffffU && !take64(member.localHeaderOffset))
                return false;
            if (rawDisk == 0xffffU) {
                if (value > end - 4 || le32(extra, value) != 0)
                    return false;
            }
        }
        position += size;
    }
    const bool needsZip64 = rawUncompressed == 0xffffffffU || rawCompressed == 0xffffffffU
        || rawOffset == 0xffffffffU || rawDisk == 0xffffU;
    return position == extra.size() && (!needsZip64 || foundZip64);
}

ZipDirectoryResult corrupt(const QString& detail)
{
    return {ZipStatus::Corrupt, detail, {}};
}

} // namespace

bool zipMethodSupported(quint16 method)
{
    return method == 0 || method == 8;
}

ZipDirectoryResult readZipDirectory(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {ZipStatus::Unreadable, file.errorString(), {}};

    const quint64 fileSize = quint64(file.size());
    QByteArray first;
    const bool hasFirstSignature = fileSize >= 4 && readAt(file, 0, 4, first)
        && le32(first, 0) == LocalHeaderSignature;

    const qint64 tailSize = qMin<qint64>(file.size(), MaximumTailSize);
    QByteArray tail;
    if (!readAt(file, fileSize - quint64(tailSize), tailSize, tail))
        return {ZipStatus::Unreadable, QStringLiteral("Could not read ZIP tail"), {}};

    qsizetype eocdInTail = -1;
    for (qsizetype i = tail.size() - 22; i >= 0; --i) {
        if (le32(tail, i) != EocdSignature)
            continue;
        const quint16 commentLength = le16(tail, i + 20);
        if (i + 22 + commentLength == tail.size()) {
            eocdInTail = i;
            break;
        }
    }
    if (eocdInTail < 0) {
        return {hasFirstSignature ? ZipStatus::Truncated : ZipStatus::NotZip,
                hasFirstSignature ? QStringLiteral("ZIP central directory is missing")
                                  : QStringLiteral("End-of-central-directory record not found"),
                {}};
    }

    const quint64 eocdOffset = fileSize - quint64(tailSize) + quint64(eocdInTail);
    if (le16(tail, eocdInTail + 4) != 0 || le16(tail, eocdInTail + 6) != 0
        || le16(tail, eocdInTail + 8) != le16(tail, eocdInTail + 10))
        return corrupt(QStringLiteral("Multi-disk ZIP files are not supported"));

    quint64 entryCount = le16(tail, eocdInTail + 10);
    quint64 directorySize = le32(tail, eocdInTail + 12);
    quint64 directoryOffset = le32(tail, eocdInTail + 16);
    const bool needsZip64 = entryCount == 0xffffU || directorySize == 0xffffffffU
        || directoryOffset == 0xffffffffU;

    if (needsZip64) {
        if (eocdOffset < 20)
            return corrupt(QStringLiteral("ZIP64 locator is missing"));
        QByteArray locator;
        if (!readAt(file, eocdOffset - 20, 20, locator)
            || le32(locator, 0) != Zip64LocatorSignature)
            return corrupt(QStringLiteral("ZIP64 locator is missing"));
        if (le32(locator, 4) != 0 || le32(locator, 16) != 1)
            return corrupt(QStringLiteral("Multi-disk ZIP64 files are not supported"));
        const quint64 recordOffset = le64(locator, 8);
        QByteArray record;
        if (!readAt(file, recordOffset, 56, record)
            || le32(record, 0) != Zip64EocdSignature
            || le64(record, 4) < 44)
            return corrupt(QStringLiteral("ZIP64 end record is invalid"));
        const quint64 recordSize = le64(record, 4);
        if (recordOffset + 12 < recordOffset || recordOffset + 12 + recordSize < recordOffset
            || recordOffset + 12 + recordSize > eocdOffset - 20)
            return corrupt(QStringLiteral("ZIP64 end record bounds are invalid"));
        if (le32(record, 16) != 0 || le32(record, 20) != 0)
            return corrupt(QStringLiteral("Multi-disk ZIP64 files are not supported"));
        if (le64(record, 24) != le64(record, 32))
            return corrupt(QStringLiteral("Multi-disk ZIP64 entry counts are not supported"));
        entryCount = le64(record, 32);
        directorySize = le64(record, 40);
        directoryOffset = le64(record, 48);
    }

    if (entryCount > MaximumEntries)
        return corrupt(QStringLiteral("ZIP has too many central-directory entries"));
    if (directorySize > MaximumDirectorySize)
        return corrupt(QStringLiteral("ZIP central directory is too large"));
    quint64 actualDirectoryOffset = directoryOffset;
    qint64 offsetDelta = 0;
    bool shiftedOffsets = false;
    auto hasCentralHeaderAt = [&](quint64 offset) {
        if (entryCount == 0)
            return true;
        QByteArray signature;
        return readAt(file, offset, 4, signature)
            && le32(signature, 0) == CentralHeaderSignature;
    };
    const bool claimedBoundsValid = directoryOffset + directorySize >= directoryOffset
        && directoryOffset + directorySize <= fileSize
        && directoryOffset + directorySize <= eocdOffset;
    if (!claimedBoundsValid || !hasCentralHeaderAt(directoryOffset)) {
        if (eocdOffset < directorySize)
            return corrupt(QStringLiteral("ZIP central-directory bounds are invalid"));
        const quint64 recovered = eocdOffset - directorySize;
        if (recovered + directorySize < recovered
            || recovered + directorySize > fileSize
            || !hasCentralHeaderAt(recovered))
            return corrupt(QStringLiteral("ZIP central-directory bounds are invalid"));
        if (recovered > quint64(std::numeric_limits<qint64>::max())
            || directoryOffset > quint64(std::numeric_limits<qint64>::max()))
            return corrupt(QStringLiteral("ZIP offset shift is too large"));
        actualDirectoryOffset = recovered;
        offsetDelta = qint64(recovered) - qint64(directoryOffset);
        shiftedOffsets = true;
    }

    QByteArray directory;
    if (!readAt(file, actualDirectoryOffset, qsizetype(directorySize), directory))
        return {ZipStatus::Unreadable, QStringLiteral("Could not read ZIP central directory"), {}};

    QList<ZipMember> members;
    members.reserve(qsizetype(entryCount));
    qsizetype position = 0;
    for (quint64 index = 0; index < entryCount; ++index) {
        if (!hasBytes(directory, position, 46)
            || le32(directory, position) != CentralHeaderSignature)
            return corrupt(QStringLiteral("Central-directory entry count or header is invalid"));

        const quint16 flags = le16(directory, position + 8);
        const quint16 nameLength = le16(directory, position + 28);
        const quint16 extraLength = le16(directory, position + 30);
        const quint16 commentLength = le16(directory, position + 32);
        const qsizetype totalLength = 46 + qsizetype(nameLength) + qsizetype(extraLength)
            + qsizetype(commentLength);
        if (!hasBytes(directory, position, totalLength))
            return corrupt(QStringLiteral("Central-directory entry is truncated"));

        const QByteArray rawName = directory.mid(position + 46, nameLength);
        const QByteArray extra = directory.mid(position + 46 + nameLength, extraLength);
        const quint32 rawCompressed = le32(directory, position + 20);
        const quint32 rawUncompressed = le32(directory, position + 24);
        const quint32 rawOffset = le32(directory, position + 42);
        const quint16 rawDisk = le16(directory, position + 34);

        ZipMember member;
        member.name = (flags & 0x0800U) ? QString::fromUtf8(rawName) : decodeCp437(rawName);
        member.method = le16(directory, position + 10);
        member.crc32 = le32(directory, position + 16);
        member.compressedSize = rawCompressed;
        member.uncompressedSize = rawUncompressed;
        member.localHeaderOffset = rawOffset;
        member.encrypted = flags & 0x0001U;
        member.isDirectory = member.name.endsWith(QLatin1Char('/'));
        if (!applyZip64Extra(extra, rawUncompressed, rawCompressed, rawOffset, rawDisk, member))
            return corrupt(QStringLiteral("ZIP64 entry metadata is invalid"));
        if (rawDisk != 0 && rawDisk != 0xffffU)
            return corrupt(QStringLiteral("Multi-disk ZIP entries are not supported"));
        if (offsetDelta < 0
            && member.localHeaderOffset < quint64(-offsetDelta)) {
            member.localHeaderOffset = 0;
            member.damaged = true;
        } else {
            const quint64 adjusted = offsetDelta < 0
                ? member.localHeaderOffset - quint64(-offsetDelta)
                : member.localHeaderOffset + quint64(offsetDelta);
            if ((offsetDelta > 0 && adjusted < member.localHeaderOffset)
                || adjusted >= actualDirectoryOffset) {
                member.damaged = true;
            } else {
                member.localHeaderOffset = adjusted;
                QByteArray signature;
                if (!readAt(file, adjusted, 4, signature)
                    || le32(signature, 0) != LocalHeaderSignature)
                    member.damaged = true;
            }
        }

        members.append(member);
        position += totalLength;
    }

    if (position != directory.size()) {
        const bool validSignature = hasBytes(directory, position, 6)
            && le32(directory, position) == CentralDigitalSignature
            && position + 6 + le16(directory, position + 4) == directory.size();
        if (!validSignature)
            return corrupt(QStringLiteral("ZIP central-directory size does not match its entries"));
    }

    if (shiftedOffsets) {
        qsizetype damaged = 0;
        for (const ZipMember& member : std::as_const(members))
            damaged += member.damaged;
        return {ZipStatus::Corrupt,
                QStringLiteral("ZIP offsets shifted by %1 bytes; %2 member(s) damaged")
                    .arg(offsetDelta).arg(damaged),
                members};
    }
    return {ZipStatus::Ok, {}, members};
}
