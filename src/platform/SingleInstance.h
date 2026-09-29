#pragma once

#include <QString>

#include <memory>

class QLockFile;

// One running copy of the program per user (per data folder). On Windows a
// named mutex, which Windows releases the moment the process ends, however it
// ends; elsewhere a lock file in the program's data folder (taken over from a
// copy that has crashed, as its process no longer exists).
class SingleInstance {
public:
    enum class Result { Acquired, AlreadyRunning, Failed };

    explicit SingleInstance(QString dataFolder);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    Result acquire();

private:
    QString m_dataFolder;
#ifdef Q_OS_WIN
    void* m_mutex = nullptr;
#else
    std::unique_ptr<QLockFile> m_lock;
#endif
};
