#include "SongSettings.h"

#include "Logging.h"
#include "library/ContentIdentity.h"

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

constexpr int kSettingsLockTimeoutMs = 100;
constexpr int kSettingsStaleLockMs = 30 * 1000;

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
    return contentIdentity(pair.mp3Path, pair.cdgPath);
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

bool SongSettingsStore::forgetAll(QString* backupPath, QString* error)
{
    const auto fail = [error](const QString& message) {
        qCWarning(lcApp).noquote() << message;
        if (error)
            *error = message;
        return false;
    };
    if (m_readOnly)
        return fail(QStringLiteral("Song settings are read-only this session; nothing was reset."));
    QLockFile lock(m_path + QStringLiteral(".lock"));
    lock.setStaleLockTime(kSettingsStaleLockMs);
    if (!lock.tryLock(kSettingsLockTimeoutMs))
        return fail(QStringLiteral("Song settings are busy; nothing was reset."));
    // Keep the old file first: the reset can be undone by putting it back.
    if (QFileInfo::exists(m_path)) {
        const QString copy = m_path + QStringLiteral(".before-reset-")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        if (!QFile::copy(m_path, copy))
            return fail(QStringLiteral("Could not keep a copy of the song settings; nothing was reset."));
        if (backupPath)
            *backupPath = copy;
    }
    const QHash<QString, Entry> previous = m_entries;
    const QSet<QString> previousUnsaved = m_unsaved;
    m_entries.clear();
    m_unsaved.clear();
    if (!save()) {
        m_entries = previous;
        m_unsaved = previousUnsaved;
        return fail(QStringLiteral("Song settings could not be written; nothing was reset."));
    }
    qCInfo(lcApp) << "All per-song Key/Tempo settings were reset";
    return true;
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
