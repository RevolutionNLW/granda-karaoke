#include "platform/SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

SingleInstance::SingleInstance(QString dataFolder)
    : m_dataFolder(std::move(dataFolder))
{
}

SingleInstance::~SingleInstance()
{
#ifdef Q_OS_WIN
    if (m_mutex) {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
    }
#endif
}

SingleInstance::Result SingleInstance::acquire()
{
#ifdef Q_OS_WIN
    // Every Windows session of this user (the same account signed in twice
    // shares one data folder), and only this user: named after the folder.
    const QByteArray folder = QDir::cleanPath(m_dataFolder).toCaseFolded().toUtf8();
    const QString name = QStringLiteral("Global\\Granda.FrankiesKaraokeStudio.%1")
                             .arg(QString::fromLatin1(
                                 QCryptographicHash::hash(folder, QCryptographicHash::Sha256).toHex().left(32)));
    HANDLE mutex = CreateMutexW(nullptr, TRUE, reinterpret_cast<const wchar_t*>(name.utf16()));
    if (!mutex)
        return Result::Failed;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return Result::AlreadyRunning;
    }
    m_mutex = mutex;
    return Result::Acquired;
#else
    m_lock = std::make_unique<QLockFile>(QDir(m_dataFolder).filePath(QStringLiteral("running.lock")));
    // Never taken over after a time limit, only when its process has gone.
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0))
        return Result::Acquired;
    return m_lock->error() == QLockFile::LockFailedError ? Result::AlreadyRunning : Result::Failed;
#endif
}
