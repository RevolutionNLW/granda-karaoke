#include "ui/Shortcuts.h"

#include "AppPreferences.h"
#include "Logging.h"

#include <QHash>

namespace shortcuts {

namespace {

const QString kNone = QStringLiteral("none");

QKeySequence keys(QKeyCombination combination)
{
    return QKeySequence(combination);
}

} // namespace

const QList<Action>& actions()
{
    // Defaults only where a shortcut clearly helps; everything else can be
    // given one in Settings.
    static const QList<Action> list = {
        {QStringLiteral("playback.playPause"), QStringLiteral("Playback"), QStringLiteral("Play / Pause"), {}},
        {QStringLiteral("playback.stop"), QStringLiteral("Playback"), QStringLiteral("Stop"), {}},
        {QStringLiteral("playback.restart"), QStringLiteral("Playback"), QStringLiteral("Restart Song"), {}},
        {QStringLiteral("playback.autoplay"), QStringLiteral("Playback"), QStringLiteral("Turn Autoplay On / Off"), {}},
        {QStringLiteral("playback.lyrics"), QStringLiteral("Playback"), QStringLiteral("Show Lyrics"), {}},
        {QStringLiteral("key.down"), QStringLiteral("Key and Tempo"), QStringLiteral("Key Down"), {}},
        {QStringLiteral("key.up"), QStringLiteral("Key and Tempo"), QStringLiteral("Key Up"), {}},
        {QStringLiteral("key.reset"), QStringLiteral("Key and Tempo"), QStringLiteral("Reset Key"), {}},
        {QStringLiteral("tempo.down"), QStringLiteral("Key and Tempo"), QStringLiteral("Tempo Down"), {}},
        {QStringLiteral("tempo.up"), QStringLiteral("Key and Tempo"), QStringLiteral("Tempo Up"), {}},
        {QStringLiteral("tempo.reset"), QStringLiteral("Key and Tempo"), QStringLiteral("Reset Tempo"), {}},
        {QStringLiteral("library.focusSearch"), QStringLiteral("Library"), QStringLiteral("Go to Search"),
         keys(Qt::CTRL | Qt::Key_F)},
        {QStringLiteral("library.clearSearch"), QStringLiteral("Library"), QStringLiteral("Clear Search"), {}},
        {QStringLiteral("library.sing"), QStringLiteral("Library"), QStringLiteral("Sing Selected Song"), {}},
        {QStringLiteral("library.add"), QStringLiteral("Library"), QStringLiteral("Add Selected Song to Playlist"), {}},
        {QStringLiteral("library.sort.artistAsc"), QStringLiteral("Library"), QStringLiteral("Sort by Artist A → Z"), {}},
        {QStringLiteral("library.sort.artistDesc"), QStringLiteral("Library"), QStringLiteral("Sort by Artist Z → A"), {}},
        {QStringLiteral("library.sort.titleAsc"), QStringLiteral("Library"), QStringLiteral("Sort by Song A → Z"), {}},
        {QStringLiteral("library.sort.titleDesc"), QStringLiteral("Library"), QStringLiteral("Sort by Song Z → A"), {}},
        {QStringLiteral("library.sort.mostPlayed"), QStringLiteral("Library"), QStringLiteral("Sort by Most Played"), {}},
        {QStringLiteral("library.sort.recentlyPlayed"), QStringLiteral("Library"), QStringLiteral("Sort by Recently Played"), {}},
        {QStringLiteral("library.sort.labelAsc"), QStringLiteral("Library"), QStringLiteral("Sort by Label A → Z"), {}},
        {QStringLiteral("playlist.play"), QStringLiteral("Playlists"), QStringLiteral("Play Selected Playlist Song"), {}},
        {QStringLiteral("playlist.moveUp"), QStringLiteral("Playlists"), QStringLiteral("Move Song Up"),
         keys(Qt::ALT | Qt::Key_Up)},
        {QStringLiteral("playlist.moveDown"), QStringLiteral("Playlists"), QStringLiteral("Move Song Down"),
         keys(Qt::ALT | Qt::Key_Down)},
        {QStringLiteral("playlist.remove"), QStringLiteral("Playlists"), QStringLiteral("Remove Song from Playlist"), {}},
        {QStringLiteral("playlist.new"), QStringLiteral("Playlists"), QStringLiteral("New Playlist"), {}},
        {QStringLiteral("playlist.rename"), QStringLiteral("Playlists"), QStringLiteral("Rename Playlist"), {}},
        {QStringLiteral("playlist.delete"), QStringLiteral("Playlists"), QStringLiteral("Delete Playlist"), {}},
        {QStringLiteral("playlist.next"), QStringLiteral("Playlists"), QStringLiteral("Next Playlist"),
         keys(Qt::CTRL | Qt::Key_PageDown)},
        {QStringLiteral("playlist.previous"), QStringLiteral("Playlists"), QStringLiteral("Previous Playlist"),
         keys(Qt::CTRL | Qt::Key_PageUp)},
        {QStringLiteral("app.settings"), QStringLiteral("Application"), QStringLiteral("Open Settings"),
         keys(Qt::CTRL | Qt::Key_Comma)},
        {QStringLiteral("app.review"), QStringLiteral("Application"), QStringLiteral("Open Needs Review"),
         keys(Qt::CTRL | Qt::SHIFT | Qt::Key_M)},
        {QStringLiteral("app.openFile"), QStringLiteral("Application"), QStringLiteral("Open Song File"),
         keys(Qt::CTRL | Qt::Key_O)},
        {QStringLiteral("app.changeFolder"), QStringLiteral("Application"), QStringLiteral("Change Music Folder"), {}},
        {QStringLiteral("app.rescan"), QStringLiteral("Application"), QStringLiteral("Rescan Library"), {}},
        {QStringLiteral("app.exit"), QStringLiteral("Application"), QStringLiteral("Exit"), {}},
    };
    return list;
}

const Action* find(const QString& id)
{
    for (const Action& action : actions()) {
        if (action.id == id)
            return &action;
    }
    return nullptr;
}

namespace {

// The user's own choice for an action, if it is a usable one. Anything
// damaged or not allowed (e.g. Escape written into the store by hand) is
// ignored, and the action keeps its default.
std::optional<QKeySequence> storedChoice(const AppPreferences& preferences, const Action& action)
{
    const QString stored = preferences.text(pref::ShortcutPrefix + action.id);
    if (stored.isEmpty())
        return std::nullopt;
    if (stored == kNone)
        return QKeySequence();
    const QKeySequence keys = QKeySequence::fromString(stored, QKeySequence::PortableText);
    if (keys.isEmpty() || !problem(keys).isEmpty()) {
        qCWarning(lcUi).noquote() << "Ignoring an unusable shortcut for" << action.id << ":" << stored;
        return std::nullopt;
    }
    return keys;
}

} // namespace

QHash<QString, QKeySequence> effective(const AppPreferences& preferences)
{
    // The user's choices first, then defaults, so a default can never take
    // keys the user gave to something else. Keys are never shared: if two
    // choices collide (a damaged store), the first action keeps them.
    QHash<QString, QKeySequence> result;
    QList<QKeySequence> used;
    const auto take = [&result, &used](const QString& id, const QKeySequence& keys) {
        if (keys.isEmpty() || used.contains(keys)) {
            result.insert(id, QKeySequence());
            return;
        }
        used.append(keys);
        result.insert(id, keys);
    };
    for (const Action& action : actions()) {
        if (const auto choice = storedChoice(preferences, action))
            take(action.id, *choice);
    }
    for (const Action& action : actions()) {
        if (!result.contains(action.id))
            take(action.id, action.defaultKeys);
    }
    return result;
}

QKeySequence current(const AppPreferences& preferences, const QString& id)
{
    return effective(preferences).value(id);
}

namespace {

// How a choice is stored: empty for the action's default, "none" for no keys.
QString storedValue(const Action& action, const QKeySequence& keys)
{
    if (keys == action.defaultKeys)
        return QString();
    return keys.isEmpty() ? kNone : keys.toString(QKeySequence::PortableText);
}

} // namespace

bool assign(AppPreferences& preferences, const QString& id, const QKeySequence& keys)
{
    const Action* action = find(id);
    if (!action)
        return false;
    return preferences.setText(pref::ShortcutPrefix + id, storedValue(*action, keys));
}

bool move(AppPreferences& preferences, const QString& fromId, const QString& toId,
          const QKeySequence& keys)
{
    const Action* from = find(fromId);
    const Action* to = find(toId);
    if (!from || !to)
        return false;
    return preferences.setTexts({{pref::ShortcutPrefix + fromId, storedValue(*from, {})},
                                 {pref::ShortcutPrefix + toId, storedValue(*to, keys)}});
}

bool resetToDefault(AppPreferences& preferences, const QString& id)
{
    return preferences.reset(pref::ShortcutPrefix + id);
}

bool resetAll(AppPreferences& preferences)
{
    bool saved = true;
    for (const Action& action : actions())
        saved = resetToDefault(preferences, action.id) && saved;
    return saved;
}

QHash<QString, QString> storedChoices(const AppPreferences& preferences)
{
    QHash<QString, QString> result;
    for (const Action& action : actions())
        result.insert(action.id, preferences.text(pref::ShortcutPrefix + action.id));
    return result;
}

QString problem(const QKeySequence& keys)
{
    if (keys.isEmpty())
        return {};
    if (keys.count() != 1)
        return QStringLiteral("Use one key or one key combination.");
    const QKeyCombination combination = keys[0];
    const Qt::Key key = combination.key();
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers() & ~Qt::KeypadModifier;
    switch (key) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("Escape and Enter are built in and cannot be used.");
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return QStringLiteral("Tab moves between controls and cannot be used.");
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown:
        return QStringLiteral("Add a key to go with the modifier.");
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        if (modifiers == Qt::NoModifier)
            return QStringLiteral("Arrow and page keys move through the lists; add Ctrl, Alt or Shift.");
        break;
    default:
        break;
    }
    return {};
}

QString conflict(const AppPreferences& preferences, const QKeySequence& keys,
                 const QString& exceptId)
{
    if (keys.isEmpty())
        return {};
    const QHash<QString, QKeySequence> all = effective(preferences);
    for (const Action& action : actions()) {
        if (action.id != exceptId && all.value(action.id) == keys)
            return action.id;
    }
    return {};
}

QString display(const QKeySequence& keys)
{
    return keys.toString(QKeySequence::NativeText);
}

} // namespace shortcuts
