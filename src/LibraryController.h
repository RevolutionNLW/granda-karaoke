#pragma once

#include "library/Catalogue.h"

#include <QObject>
#include <QThread>

class LibraryScanner;

// Owns the GUI-thread catalogue connection and the cooperative scanner worker.
// The database remains useful for searching when the external music drive is absent.
class LibraryController : public QObject {
    Q_OBJECT

public:
    explicit LibraryController(const QString& databasePath,
                               const QString& cacheDirectory = {},
                               QObject* parent = nullptr);
    ~LibraryController() override;

    bool isAvailable() const { return m_catalogue.isOpen(); }
    QString openError() const { return m_openError; }
    bool hasActiveRoot() const;
    CatalogueRoot activeRoot() const;
    bool isRootConnected() const;
    bool isScanning() const { return m_scanning; }
    bool scannerPaused() const;
    QString statusText() const;

    QList<CatalogueSearchRow> search(const QString& text, int limit,
                                     QString* error = nullptr) const;
    PlaybackPaths playbackPathsFor(qint64 songId, QString* error = nullptr) const;

    bool chooseRoot(const QString& path, QString* error = nullptr);
    void startConfiguredScan();
    void recheckRoot();
    void requestRefreshScan();
    void setPlaybackActive(bool active);

signals:
    void stateChanged();
    void progressChanged(const QString& phase, qint64 done, qint64 total);
    void libraryReady();
    void scanFinished(const QVariantMap& summary);
    void scanRequested(const QString& rootPath);

private slots:
    void onProgress(const QString& phase, qint64 done, qint64 total,
                    const QString& currentRelPath);
    void onFinished(const QVariantMap& summary);
    void onFailed(const QString& message);

private:
    void startScan(const QString& rootPath);
    void startPendingScan();

    Catalogue m_catalogue;
    QString m_openError;
    QThread m_scannerThread;
    LibraryScanner* m_scanner = nullptr;
    bool m_scanning = false;
    bool m_ready = false;  // The current scan has made its songs searchable.
    QString m_scanningRoot;
    QString m_pendingRoot;
    QString m_phase;
    qint64 m_done = 0;
    qint64 m_total = -1;
    bool m_rootWasConnected = false;
};
