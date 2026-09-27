#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>

#include <optional>

class AppPreferences;

// The keyboard shortcuts the user can change in Settings. Escape (back to the
// controls) and Enter (back to the lyrics) are built in and never assignable.
namespace shortcuts {

struct Action {
    QString id;
    QString group;
    QString title;
    QKeySequence defaultKeys;
};

// Every configurable action, in the order Settings lists them.
const QList<Action>& actions();
const Action* find(const QString& id);

// Every action's current shortcut: the user's choice, else its default; empty
// when it has none. Unusable stored values are ignored, and no two actions
// ever share keys.
QHash<QString, QKeySequence> effective(const AppPreferences& preferences);
QKeySequence current(const AppPreferences& preferences, const QString& id);
// Stores a shortcut (empty = none). Nothing is checked here: see problem()
// and conflict() first.
bool assign(AppPreferences& preferences, const QString& id, const QKeySequence& keys);
bool resetToDefault(AppPreferences& preferences, const QString& id);
// Moves keys from one action to another, saving both together (or neither).
bool move(AppPreferences& preferences, const QString& fromId, const QString& toId,
          const QKeySequence& keys);
bool resetAll(AppPreferences& preferences);
// Every action's stored choice as saved, to tell whether any changed.
QHash<QString, QString> storedChoices(const AppPreferences& preferences);

// Why these keys cannot be a shortcut, or empty when they can: the built-in
// keys, keys the lists and search box need, bare modifiers and multi-key
// sequences are refused.
QString problem(const QKeySequence& keys);
// The id of another action already using these keys, or empty.
QString conflict(const AppPreferences& preferences, const QKeySequence& keys,
                 const QString& exceptId = {});

// How a shortcut is written in Settings on this computer.
QString display(const QKeySequence& keys);

} // namespace shortcuts
