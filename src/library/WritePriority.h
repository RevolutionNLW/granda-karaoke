#pragma once

#include <QString>

#include <functional>

class QSqlDatabase;
class QSqlError;

// The user's own changes (a song's key, a name correction) are written to the
// catalogue at once, ahead of background work. While one is waiting, the
// library scan commits what it has done at its next row, waits outside any
// transaction until the change is written, and then carries on. Without this
// a scan of a slow music drive keeps the write lock almost all the time
// (it commits and at once begins again), and the change fails with
// "database is locked".
namespace writepriority {

// Held on the interface thread while the user's change is written. Taken
// before MetadataOverrideStore::synchronisation(), so the scan steps aside
// as early as possible; the scan never waits for a user's change while it
// holds that lock (nor inside a transaction), so this cannot deadlock.
class UserWrite {
public:
    UserWrite();
    ~UserWrite();
    UserWrite(const UserWrite&) = delete;
    UserWrite& operator=(const UserWrite&) = delete;
};

// True while a user's change is waiting to be written, or being written.
// Cheap: background work asks it for every row.
bool userWriteWaiting();

// SQLite refused only because another connection holds the lock ("database
// is locked"): worth trying again. A damaged file, a full disk or an I/O
// error is not, and is never treated as busy.
bool isBusy(const QSqlError& error);
// The same for an error passed on as text.
bool isBusy(const QString& errorText);

// Begins a write transaction that holds SQLite's write lock from the start
// (BEGIN IMMEDIATE). SQLite waits for a busy lock (the connection's busy
// timeout) only when a transaction starts: a plain BEGIN that reads first is
// refused at once, without waiting, when it comes to write while another
// connection holds the lock or has written since the read. Inside a
// transaction begun here no statement can be refused as busy.
bool beginWrite(QSqlDatabase& database, QString* error = nullptr);

// How long background work keeps trying for the lock before giving up.
inline constexpr int kBackgroundBusyMs = 120 * 1000;

// While one exists, SQLite waits at most kBriefWaitMs for a busy lock on
// this connection (instead of its usual five seconds), so background work
// trying again and again still notices promptly when it is asked to stop.
class BriefWait {
public:
    static constexpr int kBriefWaitMs = 250;
    explicit BriefWait(const QSqlDatabase& database);
    ~BriefWait();
    BriefWait(const BriefWait&) = delete;
    BriefWait& operator=(const BriefWait&) = delete;

private:
    QString m_connection;
    int m_previousMs = -1;
};

// beginWrite for background work: a busy lock is tried again, with growing
// pauses, for up to kBackgroundBusyMs (each try waiting only briefly). False
// only on a real failure (or a lock busy for all that time), with *error set
// and no transaction open. When stop() asks to stop meanwhile, a plain
// transaction (which needs no lock) is opened instead, *stopped is set and
// true is returned: the caller sees the stop at its next check and commits
// nothing.
bool beginBackgroundWrite(QSqlDatabase& database, const std::function<bool()>& stop,
                          QString* error, bool* stopped = nullptr);

// Runs `attempt` again, with the same pauses and limit, while it fails only
// because the lock is busy. `attempt` must leave nothing half done when it
// fails (one transaction, begun with beginWrite), and should hold a
// BriefWait on its connection. Stops trying when stop() asks to. Returns the
// last attempt's result.
bool retryWhileBusy(const std::function<bool(QString*)>& attempt,
                    const std::function<bool()>& stop, QString* error);

} // namespace writepriority
