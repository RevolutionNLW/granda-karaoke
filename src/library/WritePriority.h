#pragma once

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

} // namespace writepriority
