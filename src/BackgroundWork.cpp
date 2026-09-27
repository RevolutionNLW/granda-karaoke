#include "BackgroundWork.h"

#include <QDeadlineTimer>
#include <QList>
#include <QPointer>
#include <QThread>

namespace background {

namespace {
QList<QPointer<QThread>> g_jobs;  // GUI thread only

void forgetFinished()
{
    g_jobs.removeIf([](const QPointer<QThread>& thread) { return thread.isNull(); });
}
} // namespace

void run(const QString& name, std::function<void()> job)
{
    forgetFinished();
    QThread* thread = QThread::create(std::move(job));
    thread->setObjectName(name);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    g_jobs.append(thread);
    thread->start();
}

int running()
{
    forgetFinished();
    int count = 0;
    for (const QPointer<QThread>& thread : std::as_const(g_jobs))
        count += thread->isFinished() ? 0 : 1;
    return count;
}

bool waitForAll(int timeoutMs)
{
    forgetFinished();
    const QDeadlineTimer deadline(timeoutMs);
    bool allDone = true;
    for (const QPointer<QThread>& thread : std::as_const(g_jobs)) {
        if (!thread->wait(deadline))
            allDone = false;
    }
    // Finished ones are deleted now: after the event loop has stopped their
    // deleteLater would never run.
    for (const QPointer<QThread>& thread : std::as_const(g_jobs)) {
        if (thread && thread->isFinished())
            delete thread.data();
    }
    forgetFinished();
    return allDone;
}

} // namespace background
