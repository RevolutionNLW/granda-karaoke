#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class PlaylistStore;

// The names of the application's settings. Every value is stored as text; an
// empty (or missing) value means "the default".
namespace pref {
inline const QString ScalePercent = QStringLiteral("ui.scalePercent");
inline const QString StartFullscreen = QStringLiteral("ui.startFullscreen");
inline const QString ShowSplash = QStringLiteral("ui.showSplash");
inline const QString RememberWindow = QStringLiteral("ui.rememberWindow");
inline const QString WindowGeometry = QStringLiteral("ui.windowGeometry");
inline const QString ConfirmExit = QStringLiteral("ui.confirmExit");
inline const QString CompactRows = QStringLiteral("ui.compactRows");
inline const QString AlternateRows = QStringLiteral("ui.alternateRows");
inline const QString ShowLabelColumn = QStringLiteral("ui.showLabelColumn");
inline const QString ShowPlaysColumn = QStringLiteral("ui.showPlaysColumn");
// "remember" (default), "on" or "off".
inline const QString AutoplayAtStartup = QStringLiteral("playback.autoplayAtStartup");
inline const QString DefaultKey = QStringLiteral("playback.defaultKey");
inline const QString DefaultTempo = QStringLiteral("playback.defaultTempo");
inline const QString ReturnHomeAtEnd = QStringLiteral("playback.returnHomeAtEnd");
inline const QString KeepDisplayAwake = QStringLiteral("playback.keepDisplayAwake");
// The chosen sound output's name (empty = the system default), whether it is
// used again at start-up, and the music volume (0-100).
inline const QString AudioOutput = QStringLiteral("audio.output");
inline const QString RememberAudioOutput = QStringLiteral("audio.rememberOutput");
inline const QString Volume = QStringLiteral("audio.volume");
// "last" (default) or a playlist id.
inline const QString StartupPlaylist = QStringLiteral("playlists.startup");
inline const QString ConfirmRemoveSong = QStringLiteral("playlists.confirmRemove");
// Followed by an action id (see ui/Shortcuts.h).
inline const QString ShortcutPrefix = QStringLiteral("shortcut.");
} // namespace pref

// The user's settings. In the application they are kept in the user-state
// database beside play history (never in the rebuildable catalogue), so they
// survive catalogue rebuilds; without one they live in memory only.
class AppPreferences : public QObject {
    Q_OBJECT

public:
    using Reader = std::function<QString(const QString& key)>;
    using Writer = std::function<bool(const QString& key, const QString& value)>;
    // Saves several values at once: all of them, or none.
    using BatchWriter = std::function<bool(const QList<QPair<QString, QString>>& values)>;

    explicit AppPreferences(QObject* parent = nullptr);
    AppPreferences(Reader reader, Writer writer, QObject* parent = nullptr);

    QString text(const QString& key, const QString& fallback = {}) const;
    bool flag(const QString& key, bool fallback) const;
    int number(const QString& key, int fallback) const;

    // Each returns whether the change was saved; an unsaved change does not
    // take effect.
    bool setText(const QString& key, const QString& value);
    bool setFlag(const QString& key, bool value);
    bool setNumber(const QString& key, int value);
    // Back to the default.
    bool reset(const QString& key);
    // Several changes that belong together: all are saved and take effect,
    // or none does.
    bool setTexts(const QList<QPair<QString, QString>>& values);
    void setBatchWriter(BatchWriter writer) { m_batchWriter = std::move(writer); }
    // False when settings are only kept until the program closes (no
    // settings store could be opened).
    bool isPersistent() const { return static_cast<bool>(m_writer); }

signals:
    void changed(const QString& key);
    void saveFailed(const QString& key);

private:
    Reader m_reader;
    Writer m_writer;
    BatchWriter m_batchWriter;
    mutable QHash<QString, QString> m_values;
};

// At start-up: Autoplay on or off if the user chose so (otherwise as it was),
// and the playlist to show first if the user picked one (otherwise the last).
void applyStartupChoices(const AppPreferences& preferences, PlaylistStore& playlists);
