#include "Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>

#include <cstdio>

Q_LOGGING_CATEGORY(lcApp, "fks.app")
Q_LOGGING_CATEGORY(lcPlayer, "fks.player")
Q_LOGGING_CATEGORY(lcCdg, "fks.cdg")
Q_LOGGING_CATEGORY(lcUi, "fks.ui")

namespace {

QFile* g_logFile = nullptr;
QMutex g_logMutex;

const char* levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "DEBUG";
    case QtInfoMsg: return "INFO";
    case QtWarningMsg: return "WARN";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg: return "FATAL";
    }
    return "?";
}

void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QByteArray line = QStringLiteral("%1 %2 [%3] %4\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
             QString::fromLatin1(levelName(type)),
             QString::fromLatin1(context.category ? context.category : "default"),
             message)
        .toUtf8();

    QMutexLocker lock(&g_logMutex);
    std::fputs(line.constData(), stderr);
    if (g_logFile) {
        g_logFile->write(line);
        g_logFile->flush();
    }
}

} // namespace

namespace logging {

QString install()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString path;
    if (!dir.isEmpty() && QDir().mkpath(dir)) {
        path = QDir(dir).filePath(QStringLiteral("frankies-karaoke-studio.log"));
        // Keep one previous session's log for diagnosis.
        const QString previous = path + QStringLiteral(".1");
        QFile::remove(previous);
        QFile::rename(path, previous);
        auto* file = new QFile(path);
        if (file->open(QIODevice::WriteOnly | QIODevice::Text)) {
            g_logFile = file;
        } else {
            delete file;
            path.clear();
        }
    }
    qInstallMessageHandler(handleMessage);
    return path;
}

} // namespace logging
