#include "SongSettings.h"

#include "Logging.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qint64 kIdentitySampleBytes = 64 * 1024;
constexpr qint64 kIdentityReadChunkBytes = 64 * 1024;
constexpr int kSettingsLockTimeoutMs = 100;
constexpr int kSettingsStaleLockMs = 30 * 1000;

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

bool integerValue(const QJsonValue& value, int& result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max()))
        return false;
    result = static_cast<int>(number);
    return true;
}

} // namespace

SongSettings SongSettings::clamped() const
{
    return {
        std::clamp(keySemitones, kMinKey, kMaxKey),
        std::clamp(tempoPercent, kMinTempo, kMaxTempo),
    };
}

QString songIdentity(const SongPair& pair)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("fks-song-identity-v1\0", 21));
    if (!addMp3Fingerprint(hash, pair.mp3Path)
        || !addCompleteFileFingerprint(hash, "cdg", pair.cdgPath))
        return {};
    return QStringLiteral("v1:") + QString::fromLatin1(hash.result().toHex());
}

SongSettingsStore::SongSettingsStore(const QString& path)
    : m_path(path)
{
    load();
}

SongSettings SongSettingsStore::settingsFor(const QString& id) const
{
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() ? SongSettings{} : it->settings;
}

bool SongSettingsStore::store(const QString& id, const SongSettings& settings,
                              const SongPair& pair)
{
    if (id.isEmpty())
        return false;

    Entry entry;
    entry.settings = settings.clamped();
    entry.lastPath = QFileInfo(pair.mp3Path).absoluteFilePath();
    entry.displayName = pair.displayName();
    entry.updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_entries.insert(id, entry);
    m_unsaved.insert(id);
    if (m_readOnly) {
        if (!m_readOnlyWarningLogged) {
            qCWarning(lcApp) << "Song settings are not being saved (see earlier warning)";
            m_readOnlyWarningLogged = true;
        }
        return false;
    }

    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        qCWarning(lcApp) << "Cannot create song settings folder" << info.absolutePath();
        return false;
    }

    QLockFile lock(m_path + QStringLiteral(".lock"));
    lock.setStaleLockTime(kSettingsStaleLockMs);
    if (!lock.tryLock(kSettingsLockTimeoutMs)) {
        if (!m_lockFailureWarningLogged) {
            qCWarning(lcApp) << "Cannot lock song settings" << m_path << ":"
                             << lock.error() << "- keeping changes in memory";
            m_lockFailureWarningLogged = true;
        }
        return false;
    }
    m_lockFailureWarningLogged = false;

    QHash<QString, Entry> diskEntries;
    int version = 0;
    switch (readFile(diskEntries, version)) {
    case LoadResult::Missing:
        break;
    case LoadResult::Valid:
        break;
    case LoadResult::Unreadable:
        qCWarning(lcApp) << "Cannot re-read song settings" << m_path
                         << "- leaving it untouched; changes will not be saved this session";
        m_readOnly = true;
        return false;
    case LoadResult::NewerVersion:
        qCWarning(lcApp) << "Song settings file" << m_path << "is from a newer version ("
                         << version << ") - leaving it untouched; changes will not be saved this session";
        m_readOnly = true;
        return false;
    case LoadResult::Corrupt:
        qCWarning(lcApp) << "Song settings file is corrupt or unsupported:" << m_path;
        if (!preserveCorruptFile()) {
            m_readOnly = true;
            return false;
        }
        diskEntries.clear();
        break;
    }
    // Other instances' changes come from disk; this session's unsaved
    // changes (including earlier ones that could not be written) win.
    for (const QString& unsavedId : std::as_const(m_unsaved))
        diskEntries.insert(unsavedId, m_entries.value(unsavedId));
    m_entries = std::move(diskEntries);
    if (!save())
        return false;
    m_unsaved.clear();
    return true;
}

void SongSettingsStore::load()
{
    QHash<QString, Entry> entries;
    int version = 0;
    switch (readFile(entries, version)) {
    case LoadResult::Missing:
        return;
    case LoadResult::Valid:
        m_entries = std::move(entries);
        return;
    case LoadResult::Unreadable:
        qCWarning(lcApp) << "Cannot read song settings" << m_path
                         << "- changes will not be saved this session";
        m_readOnly = true;
        return;
    case LoadResult::NewerVersion:
        qCWarning(lcApp) << "Song settings file" << m_path << "is from a newer version (" << version
                         << ") - leaving it untouched; changes will not be saved this session";
        m_readOnly = true;
        return;
    case LoadResult::Corrupt:
        qCWarning(lcApp) << "Song settings file is corrupt or unsupported:" << m_path;
        if (!preserveCorruptFile())
            m_readOnly = true;
        return;
    }
}

SongSettingsStore::LoadResult SongSettingsStore::readFile(
    QHash<QString, Entry>& entries, int& version) const
{
    QFile file(m_path);
    if (!file.exists())
        return LoadResult::Missing;
    if (!file.open(QIODevice::ReadOnly)) {
        return LoadResult::Unreadable;
    }

    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return LoadResult::Unreadable;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
    version = 0;
    const bool recognised = parseError.error == QJsonParseError::NoError
        && document.isObject()
        && root.value(QStringLiteral("format")).toString() == QStringLiteral("fks-song-settings")
        && integerValue(root.value(QStringLiteral("version")), version);
    if (recognised && version > 1)
        return LoadResult::NewerVersion;
    const bool valid = recognised && version == 1
        && root.value(QStringLiteral("songs")).isObject();
    if (!valid)
        return LoadResult::Corrupt;

    const QJsonObject songs = root.value(QStringLiteral("songs")).toObject();
    for (auto it = songs.begin(); it != songs.end(); ++it) {
        if (!it.value().isObject()) {
            qCWarning(lcApp) << "Ignoring invalid song settings entry" << it.key();
            continue;
        }
        const QJsonObject object = it.value().toObject();
        int key = 0;
        int tempo = 100;
        if (!integerValue(object.value(QStringLiteral("key")), key)
            || !integerValue(object.value(QStringLiteral("tempo")), tempo)) {
            qCWarning(lcApp) << "Ignoring invalid song settings entry" << it.key();
            continue;
        }

        Entry entry;
        entry.settings = SongSettings{key, tempo}.clamped();
        if (object.value(QStringLiteral("lastPath")).isString())
            entry.lastPath = object.value(QStringLiteral("lastPath")).toString();
        if (object.value(QStringLiteral("displayName")).isString())
            entry.displayName = object.value(QStringLiteral("displayName")).toString();
        if (object.value(QStringLiteral("updated")).isString())
            entry.updated = object.value(QStringLiteral("updated")).toString();
        entries.insert(it.key(), entry);
    }
    return LoadResult::Valid;
}

bool SongSettingsStore::preserveCorruptFile()
{
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString preserved = m_path + QStringLiteral(".corrupt-") + stamp;
    for (int suffix = 1; QFileInfo::exists(preserved); ++suffix)
        preserved = m_path + QStringLiteral(".corrupt-") + stamp + '-' + QString::number(suffix);
    if (!QFile::rename(m_path, preserved)) {
        qCWarning(lcApp) << "Could not preserve corrupt song settings file" << m_path;
        return false;
    } else {
        qCWarning(lcApp) << "Preserved corrupt song settings as" << preserved;
        return true;
    }
}

bool SongSettingsStore::save() const
{
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        qCWarning(lcApp) << "Cannot create song settings folder" << info.absolutePath();
        return false;
    }

    QJsonObject songs;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        QJsonObject object;
        object.insert(QStringLiteral("key"), it->settings.keySemitones);
        object.insert(QStringLiteral("tempo"), it->settings.tempoPercent);
        object.insert(QStringLiteral("lastPath"), it->lastPath);
        object.insert(QStringLiteral("displayName"), it->displayName);
        object.insert(QStringLiteral("updated"), it->updated);
        songs.insert(it.key(), object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("fks-song-settings"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("songs"), songs);

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcApp) << "Cannot write song settings" << m_path << ":" << file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        qCWarning(lcApp) << "Cannot save song settings" << m_path << ":" << file.errorString();
        return false;
    }
    return true;
}
