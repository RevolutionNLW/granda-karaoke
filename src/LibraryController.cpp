#include "LibraryController.h"

#include "library/LibraryScanner.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>

namespace {
Q_LOGGING_CATEGORY(lcLibraryController, "fks.library.controller")
}

LibraryController::LibraryController(const QString& databasePath,
                                     const QString& cacheDirectory,
                                     QObject* parent)
    : QObject(parent)
    , m_catalogue(databasePath, cacheDirectory)
{
    if (!m_catalogue.open(&m_openError)) {
        qCCritical(lcLibraryController).noquote()
            << "Song library catalogue is unavailable:" << m_openError;
        return;
    }

    m_scanner = new LibraryScanner(databasePath, cacheDirectory);
    m_scanner->moveToThread(&m_scannerThread);
    connect(this, &LibraryController::scanRequested,
            m_scanner, &LibraryScanner::scan, Qt::QueuedConnection);
    connect(m_scanner, &LibraryScanner::progress,
            this, &LibraryController::onProgress);
    connect(m_scanner, &LibraryScanner::libraryReady, this, [this] {
        m_ready = true;
        emit libraryReady();
        emit stateChanged();
    });
    connect(m_scanner, &LibraryScanner::finished,
            this, &LibraryController::onFinished);
    connect(m_scanner, &LibraryScanner::failed,
            this, &LibraryController::onFailed);
    connect(&m_scannerThread, &QThread::finished,
            m_scanner, &QObject::deleteLater);
    m_scannerThread.setObjectName(QStringLiteral("LibraryScanner"));
    m_scannerThread.start();
}

LibraryController::~LibraryController()
{
    if (m_scanner) {
        m_scanner->requestCancel();
        m_scannerThread.quit();
        if (!m_scannerThread.wait(5000)) {
            qCCritical(lcLibraryController)
                << "Library scanner did not stop within 5 seconds; terminating worker";
            m_scannerThread.terminate();
            m_scannerThread.wait(1000);
        }
    }
    m_catalogue.close();
}

bool LibraryController::hasActiveRoot() const
{
    return isAvailable() && activeRoot().id != 0;
}

CatalogueRoot LibraryController::activeRoot() const
{
    if (!isAvailable())
        return {};
    return m_catalogue.activeRoot();
}

bool LibraryController::isRootConnected() const
{
    const CatalogueRoot root = activeRoot();
    return root.id != 0 && QFileInfo(root.path).isDir();
}

bool LibraryController::scannerPaused() const
{
    return m_scanner && m_scanner->isPaused();
}

QString LibraryController::statusText() const
{
    if (!isAvailable())
        return QStringLiteral("Song library is unavailable");
    if (!hasActiveRoot())
        return QString();
    if (!isRootConnected())
        return QStringLiteral("Music drive not connected");
    QString error;
    const qint64 count = m_catalogue.activeSongCount(&error);
    if (!error.isEmpty()) {
        qCWarning(lcLibraryController).noquote() << error;
        return QStringLiteral("Song library is unavailable");
    }
    // Progress is only shown while there is nothing to search yet. Background
    // rescans and tag reading stay quiet once songs can be found.
    if (m_scanning && !m_ready && count == 0) {
        if (m_phase == QLatin1String("walk"))
            return QStringLiteral("Finding songs... %L1 files checked").arg(m_done);
        return QStringLiteral("Finding songs...");
    }
    return count == 1 ? QStringLiteral("1 song")
                      : QStringLiteral("%L1 songs").arg(count);
}

QList<CatalogueSearchRow> LibraryController::search(const QString& text, int limit,
                                                    QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.searchActive(text, limit, error);
}

PlaybackPaths LibraryController::playbackPathsFor(qint64 songId, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.activePlaybackPathsFor(songId, error);
}

bool LibraryController::chooseRoot(const QString& path, QString* error)
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return false;
    }
    qint64 rootId = 0;
    if (!m_catalogue.addRoot(path, &rootId, error)
        || !m_catalogue.setActiveRoot(rootId, error))
        return false;
    m_rootWasConnected = true;
    emit stateChanged();
    startScan(m_catalogue.activeRoot().path);
    return true;
}

void LibraryController::startConfiguredScan()
{
    if (!hasActiveRoot()) {
        emit stateChanged();
        return;
    }
    recheckRoot();
}

void LibraryController::recheckRoot()
{
    const bool connected = isRootConnected();
    emit stateChanged();
    if (connected && !m_rootWasConnected)
        startScan(activeRoot().path);
    m_rootWasConnected = connected;
}

void LibraryController::requestRefreshScan()
{
    if (isRootConnected())
        startScan(activeRoot().path);
}

void LibraryController::setPlaybackActive(bool active)
{
    if (m_scanner)
        m_scanner->setPaused(active);
}

void LibraryController::startScan(const QString& rootPath)
{
    if (!m_scanner || rootPath.isEmpty())
        return;
    if (m_scanning) {
        if (m_scanningRoot == rootPath)
            return;
        m_pendingRoot = rootPath;
        m_scanner->requestCancel();
        return;
    }
    m_scanning = true;
    m_ready = false;
    m_scanningRoot = rootPath;
    m_phase = QStringLiteral("walk");
    m_done = 0;
    m_total = -1;
    m_scanner->prepareScan();
    emit stateChanged();
    emit scanRequested(rootPath);
}

void LibraryController::onProgress(const QString& phase, qint64 done, qint64 total,
                                   const QString& currentRelPath)
{
    Q_UNUSED(currentRelPath)
    m_phase = phase;
    m_done = done;
    m_total = total;
    emit progressChanged(phase, done, total);
    emit stateChanged();
}

void LibraryController::onFinished(const QVariantMap& summary)
{
    m_scanning = false;
    m_scanningRoot.clear();
    emit scanFinished(summary);
    emit stateChanged();
    startPendingScan();
}

void LibraryController::onFailed(const QString& message)
{
    qCWarning(lcLibraryController).noquote() << "Library scan failed:" << message;
    m_scanning = false;
    m_scanningRoot.clear();
    emit stateChanged();
    startPendingScan();
}

void LibraryController::startPendingScan()
{
    if (m_pendingRoot.isEmpty())
        return;
    const QString root = m_pendingRoot;
    m_pendingRoot.clear();
    QTimer::singleShot(0, this, [this, root] { startScan(root); });
}
