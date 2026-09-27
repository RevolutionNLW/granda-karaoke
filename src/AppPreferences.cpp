#include "AppPreferences.h"

#include "Logging.h"
#include "playlist/PlaylistStore.h"

AppPreferences::AppPreferences(QObject* parent)
    : QObject(parent)
{
}

AppPreferences::AppPreferences(Reader reader, Writer writer, QObject* parent)
    : QObject(parent)
    , m_reader(std::move(reader))
    , m_writer(std::move(writer))
{
}

QString AppPreferences::text(const QString& key, const QString& fallback) const
{
    auto it = m_values.constFind(key);
    if (it == m_values.constEnd())
        it = m_values.insert(key, m_reader ? m_reader(key) : QString());
    return it->isEmpty() ? fallback : *it;
}

bool AppPreferences::flag(const QString& key, bool fallback) const
{
    const QString value = text(key);
    if (value == QLatin1String("1"))
        return true;
    if (value == QLatin1String("0"))
        return false;
    return fallback;
}

int AppPreferences::number(const QString& key, int fallback) const
{
    bool ok = false;
    const int value = text(key).toInt(&ok);
    return ok ? value : fallback;
}

bool AppPreferences::setText(const QString& key, const QString& value)
{
    if (text(key) == value)
        return true;
    // Only a saved change takes effect: a failed one leaves the old value in
    // place (and the controls showing it), and can simply be tried again.
    if (m_writer && !m_writer(key, value)) {
        qCWarning(lcUi).noquote() << "Setting could not be saved:" << key;
        emit saveFailed(key);
        emit changed(key);
        return false;
    }
    m_values.insert(key, value);
    emit changed(key);
    return true;
}

bool AppPreferences::setTexts(const QList<QPair<QString, QString>>& values)
{
    QList<QPair<QString, QString>> changes;
    for (const auto& [key, value] : values) {
        if (text(key) != value)
            changes.append({key, value});
    }
    if (changes.isEmpty())
        return true;
    bool saved = true;
    if (m_batchWriter) {
        saved = m_batchWriter(changes);
    } else if (m_writer) {
        // One at a time, putting back what was saved if a later one fails.
        // Anything that cannot be put back stays as saved, and is shown so.
        QList<QPair<QString, QString>> done;
        for (const auto& [key, value] : std::as_const(changes)) {
            if (!m_writer(key, value)) {
                saved = false;
                for (const auto& [doneKey, doneValue] : std::as_const(done)) {
                    if (!m_writer(doneKey, text(doneKey)))
                        m_values.insert(doneKey, doneValue);
                }
                break;
            }
            done.append({key, value});
        }
    }
    if (!saved) {
        qCWarning(lcUi).noquote() << "Settings could not be saved together:" << changes.first().first;
        for (const auto& [key, value] : std::as_const(changes)) {
            emit saveFailed(key);
            emit changed(key);
        }
        return false;
    }
    for (const auto& [key, value] : std::as_const(changes))
        m_values.insert(key, value);
    for (const auto& [key, value] : std::as_const(changes))
        emit changed(key);
    return true;
}

bool AppPreferences::setFlag(const QString& key, bool value)
{
    return setText(key, value ? QStringLiteral("1") : QStringLiteral("0"));
}

bool AppPreferences::setNumber(const QString& key, int value)
{
    return setText(key, QString::number(value));
}

bool AppPreferences::reset(const QString& key)
{
    return setText(key, QString());
}

void applyStartupChoices(const AppPreferences& preferences, PlaylistStore& playlists)
{
    if (!playlists.isOpen())
        return;
    const QString autoplay = preferences.text(pref::AutoplayAtStartup);
    QString error;
    if ((autoplay == QLatin1String("on") || autoplay == QLatin1String("off"))
        && !playlists.setAutoplay(autoplay == QLatin1String("on"), &error))
        qCWarning(lcUi).noquote() << "Autoplay could not be set at start-up:" << error;
    bool ok = false;
    const qint64 startup = preferences.text(pref::StartupPlaylist).toLongLong(&ok);
    if (ok && playlists.playlist(startup) && !playlists.setLastPlaylistId(startup, &error))
        qCWarning(lcUi).noquote() << "The start-up playlist could not be chosen:" << error;
}
