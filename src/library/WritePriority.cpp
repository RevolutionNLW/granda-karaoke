#include "library/WritePriority.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <algorithm>
#include <atomic>

namespace writepriority {

namespace {

std::atomic_int g_waiting = 0;

// SQLITE_BUSY and SQLITE_LOCKED (and their extended forms).
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;

// The pause before the next try: 25 ms, doubling up to half a second.
int pauseBefore(int attempt)
{
    return std::min(500, 25 << std::min(attempt, 5));
}

// Sleeps for the pause, looking at stop() every 50 ms. True: asked to stop.
bool pauseUnlessStopped(int milliseconds, const std::function<bool()>& stop)
{
    for (int slept = 0; slept < milliseconds; slept += 50) {
        if (stop && stop())
            return true;
        QThread::msleep(std::min(50, milliseconds - slept));
    }
    return stop && stop();
}

} // namespace

UserWrite::UserWrite()
{
    g_waiting.fetch_add(1);
}

UserWrite::~UserWrite()
{
    g_waiting.fetch_sub(1);
}

bool userWriteWaiting()
{
    return g_waiting.load() > 0;
}

bool isBusy(const QSqlError& error)
{
    if (error.type() == QSqlError::NoError)
        return false;
    bool numeric = false;
    const int code = error.nativeErrorCode().toInt(&numeric);
    if (numeric)
        return (code & 0xff) == kSqliteBusy || (code & 0xff) == kSqliteLocked;
    return isBusy(error.text());
}

bool isBusy(const QString& errorText)
{
    // SQLite's own words for SQLITE_BUSY and SQLITE_LOCKED; no other
    // failure is described this way.
    return errorText.contains(QLatin1String("database is locked"), Qt::CaseInsensitive)
        || errorText.contains(QLatin1String("database table is locked"), Qt::CaseInsensitive)
        || errorText.contains(QLatin1String("database is busy"), Qt::CaseInsensitive);
}

BriefWait::BriefWait(const QSqlDatabase& database)
    : m_connection(database.connectionName())
{
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("PRAGMA busy_timeout")) && query.next())
        m_previousMs = query.value(0).toInt();
    query.finish();
    if (m_previousMs > kBriefWaitMs)
        query.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(kBriefWaitMs));
    else
        m_previousMs = -1;  // already brief: nothing to restore
}

BriefWait::~BriefWait()
{
    if (m_previousMs < 0)
        return;
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(m_previousMs));
}

namespace {

bool tryBeginWrite(QSqlDatabase& database, QSqlError* failure)
{
    QSqlQuery begin(database);
    if (begin.exec(QStringLiteral("BEGIN IMMEDIATE")))
        return true;
    *failure = begin.lastError();
    return false;
}

} // namespace

bool beginWrite(QSqlDatabase& database, QString* error)
{
    QSqlError failure;
    if (tryBeginWrite(database, &failure))
        return true;
    if (error)
        *error = failure.text();
    return false;
}

bool beginBackgroundWrite(QSqlDatabase& database, const std::function<bool()>& stop,
                          QString* error, bool* stopped)
{
    if (stopped)
        *stopped = false;
    const BriefWait brief(database);
    QElapsedTimer elapsed;
    elapsed.start();
    for (int attempt = 0;; ++attempt) {
        QSqlError failure;
        if (tryBeginWrite(database, &failure)) {
            if (attempt > 0) {
                qInfo().noquote() << "Background library work waited" << elapsed.elapsed()
                                  << "ms for the library (attempt" << attempt + 1 << ")";
            }
            return true;
        }
        if (!isBusy(failure) || elapsed.elapsed() >= kBackgroundBusyMs) {
            qWarning().noquote() << "Background library work could not begin writing after"
                                 << elapsed.elapsed() << "ms:" << failure.text();
            if (error)
                *error = failure.text();
            return false;
        }
        if (pauseUnlessStopped(pauseBefore(attempt), stop)) {
            if (stopped)
                *stopped = true;
            return database.transaction();  // nothing will be written in it
        }
    }
}

bool retryWhileBusy(const std::function<bool(QString*)>& attempt,
                    const std::function<bool()>& stop, QString* error)
{
    QElapsedTimer elapsed;
    elapsed.start();
    for (int tries = 0;; ++tries) {
        QString attemptError;
        if (attempt(&attemptError)) {
            if (tries > 0) {
                qInfo().noquote() << "Background library work waited" << elapsed.elapsed()
                                  << "ms for the library (attempt" << tries + 1 << ")";
            }
            return true;
        }
        if (!isBusy(attemptError) || elapsed.elapsed() >= kBackgroundBusyMs
            || pauseUnlessStopped(pauseBefore(tries), stop)) {
            if (error)
                *error = attemptError;
            return false;
        }
    }
}

} // namespace writepriority
