#include "library/ContentIdentity.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>

#include <algorithm>

namespace {

constexpr qint64 kIdentitySampleBytes = 64 * 1024;
constexpr qint64 kIdentityReadChunkBytes = 64 * 1024;

void addFingerprintHeader(QCryptographicHash& hash, const char* label, qint64 size)
{
    hash.addData(label);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArray::number(size));
    hash.addData(QByteArrayView("\0", 1));
}

bool readExactlyAt(QFile& file, qint64 offset, qint64 size, QByteArray& bytes)
{
    if (offset < 0 || size < 0 || !file.seek(offset))
        return false;
    bytes = file.read(size);
    return bytes.size() == size && file.error() == QFileDevice::NoError;
}

bool addCompleteFileFingerprint(QCryptographicHash& hash, const char* label,
                                const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const qint64 size = file.size();
    if (size < 0)
        return false;
    addFingerprintHeader(hash, label, size);
    qint64 remaining = size;
    while (remaining > 0) {
        const qint64 wanted = std::min(remaining, kIdentityReadChunkBytes);
        const QByteArray bytes = file.read(wanted);
        if (bytes.size() != wanted)
            return false;
        hash.addData(bytes);
        remaining -= wanted;
    }
    hash.addData(QByteArrayView("\0", 1));
    return file.error() == QFileDevice::NoError;
}

quint32 littleEndian32(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(bytes[offset]))
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

bool mp3PayloadRange(QFile& file, qint64 fileSize, qint64& start, qint64& end)
{
    start = 0;
    end = fileSize;
    QByteArray bytes;

    if (fileSize >= 3 && !readExactlyAt(file, 0, std::min<qint64>(10, fileSize), bytes))
        return false;
    if (bytes.startsWith("ID3")) {
        if (bytes.size() < 10)
            return true; // Truncated tag header: defensively hash the whole file.
        const auto byte = [&bytes](qsizetype index) {
            return static_cast<unsigned char>(bytes[index]);
        };
        if ((byte(6) | byte(7) | byte(8) | byte(9)) & 0x80)
            return true; // Not a valid syncsafe size.
        const quint64 tagPayloadSize = (static_cast<quint64>(byte(6)) << 21)
            | (static_cast<quint64>(byte(7)) << 14)
            | (static_cast<quint64>(byte(8)) << 7)
            | static_cast<quint64>(byte(9));
        const quint64 tagSize = 10 + tagPayloadSize + ((byte(5) & 0x10) ? 10 : 0);
        if (tagSize > static_cast<quint64>(fileSize))
            return true;
        start = static_cast<qint64>(tagSize);
    }

    if (end - start >= 128) {
        if (!readExactlyAt(file, end - 128, 3, bytes))
            return false;
        if (bytes == QByteArrayLiteral("TAG"))
            end -= 128;
    }

    if (end - start >= 32) {
        if (!readExactlyAt(file, end - 32, 32, bytes))
            return false;
        if (bytes.startsWith("APETAGEX")) {
            const quint32 apeSize = littleEndian32(bytes, 12);
            if (apeSize < 32 || static_cast<quint64>(apeSize) > static_cast<quint64>(end - start)) {
                start = 0;
                end = fileSize;
                return true;
            }
            end -= apeSize;
        }
    }

    if (end < start) {
        start = 0;
        end = fileSize;
    }
    return true;
}

bool addMp3Fingerprint(QCryptographicHash& hash, const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const qint64 fileSize = file.size();
    if (fileSize < 0)
        return false;

    qint64 start = 0;
    qint64 end = fileSize;
    if (!mp3PayloadRange(file, fileSize, start, end))
        return false;
    const qint64 payloadSize = end - start;
    const qint64 sampleSize = std::min(payloadSize, kIdentitySampleBytes);
    QByteArray first;
    QByteArray last;
    if (!readExactlyAt(file, start, sampleSize, first)
        || !readExactlyAt(file, end - sampleSize, sampleSize, last))
        return false;

    addFingerprintHeader(hash, "mp3", payloadSize);
    hash.addData(first);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(last);
    hash.addData(QByteArrayView("\0", 1));
    return file.error() == QFileDevice::NoError;
}

// Counts tile blocks that draw a visible two-colour pattern (glyph edges,
// pictures). Presets, scrolls and palette loads draw nothing, and a tile in a
// single colour or with a uniform pattern is a fill that may change nothing,
// so none of those count as content.
bool isGraphicsInstruction(const char* packet)
{
    if ((static_cast<unsigned char>(packet[0]) & 0x3f) != 0x09)
        return false;
    const unsigned char code = static_cast<unsigned char>(packet[1]) & 0x3f;
    if (code != 6 && code != 38)
        return false;
    const unsigned char color0 = static_cast<unsigned char>(packet[4]) & 0x0f;
    const unsigned char color1 = static_cast<unsigned char>(packet[5]) & 0x0f;
    if (color0 == color1)
        return false;
    bool anySet = false;
    bool anyClear = false;
    for (int row = 8; row < 20; ++row) {
        const unsigned char bits = static_cast<unsigned char>(packet[row]) & 0x3f;
        anySet = anySet || bits != 0;
        anyClear = anyClear || bits != 0x3f;
    }
    return anySet && anyClear;
}

} // namespace

QString contentIdentity(const QString& mp3Path, const QString& cdgPath)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("fks-song-identity-v1\0", 21));
    if (!addMp3Fingerprint(hash, mp3Path)
        || !addCompleteFileFingerprint(hash, "cdg", cdgPath))
        return {};
    return QStringLiteral("v1:") + QString::fromLatin1(hash.result().toHex());
}

bool cdgContentDigest(const QString& path, QByteArray* digest, qint64* graphicsPackets)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 packets = 0;
    QByteArray carry;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kIdentityReadChunkBytes * 4);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return false;
        hash.addData(chunk);
        carry.append(chunk);
        const qsizetype whole = carry.size() - carry.size() % 24;
        for (qsizetype offset = 0; offset < whole; offset += 24) {
            if (isGraphicsInstruction(carry.constData() + offset))
                ++packets;
        }
        carry.remove(0, whole);
    }
    if (file.error() != QFileDevice::NoError)
        return false;
    *digest = hash.result();
    *graphicsPackets = packets;
    return true;
}

bool mp3AudioDigest(const QString& path, QByteArray* digest)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("fks-mp3-audio-v1\0", 17));
    if (!addMp3Fingerprint(hash, path))
        return false;
    *digest = hash.result();
    return true;
}

bool quickFileDigest(const QString& path, QByteArray* digest)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const qint64 size = file.size();
    if (size < 0)
        return false;
    const qint64 sample = std::min(size, kIdentitySampleBytes);
    QByteArray first;
    QByteArray last;
    if (!readExactlyAt(file, 0, sample, first) || !readExactlyAt(file, size - sample, sample, last))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFingerprintHeader(hash, "quick", size);
    hash.addData(first);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(last);
    *digest = hash.result();
    return true;
}
