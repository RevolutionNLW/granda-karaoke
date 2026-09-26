#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <atomic>

struct ScanOptions {
    bool readTags = true;
    int limitSeconds = 0;
};

class Catalogue;

class LibraryScanner : public QObject {
    Q_OBJECT

public:
    explicit LibraryScanner(QString databasePath, QString cacheDirectory = {},
                            QObject* parent = nullptr);

    void setOptions(const ScanOptions& options) { m_options = options; }
    void prepareScan() { m_cancelled.store(false); }
    void setPaused(bool paused) { m_paused.store(paused); }
    bool isPaused() const { return m_paused.load(); }
    quint64 sourceFileReads() const { return m_sourceFileReads; }

public slots:
    void scan(const QString& rootPath);
    void requestCancel();

signals:
    void progress(const QString& phase, qint64 done, qint64 total,
                  const QString& currentRelPath);
    void finished(const QVariantMap& summary);
    void failed(const QString& message);
    void libraryReady();

private:
    bool shouldStop() const;
    bool waitWhilePaused() const;
    void reportProgress(const QString& phase, qint64 done, qint64 total,
                        const QString& relPath, bool force = false);
    bool walk(Catalogue& catalogue, qint64 rootId, qint64 scanId,
              const QString& rootPath, QVariantMap& counts, QString* error);
    bool readZipDirectories(Catalogue& catalogue, qint64 rootId,
                            const QString& rootPath, QVariantMap& counts, QString* error);
    bool pairSources(Catalogue& catalogue, qint64 rootId, QVariantMap& counts,
                     QString* error);
    bool enrichTags(Catalogue& catalogue, qint64 rootId, const QString& rootPath,
                    QVariantMap& counts, QString* error);
    bool mergeZipDuplicates(Catalogue& catalogue, qint64 rootId,
                            const QString& rootPath, QVariantMap& counts, QString* error);

    QString m_databasePath;
    QString m_cacheDirectory;
    ScanOptions m_options;
    std::atomic_bool m_cancelled = false;
    std::atomic_bool m_paused = false;
    QElapsedTimer m_scanTimer;
    QElapsedTimer m_progressTimer;
    quint64 m_sourceFileReads = 0;
};
