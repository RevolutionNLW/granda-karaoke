#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include <atomic>
#include <memory>

class SongKeyEngine;
class TitleScreenOcrEngine;

struct ScanOptions {
    bool readTags = true;
    // Compare weakly named songs with same-size named copies by content.
    bool identifyDuplicates = true;
    // Read CDG title screens of unresolved songs with the local OCR engine.
    // Slow, so only an explicit maintenance reprocess enables it.
    bool readTitleScreens = false;
    int limitSeconds = 0;
};

class Catalogue;

class LibraryScanner : public QObject {
    Q_OBJECT

public:
    explicit LibraryScanner(QString databasePath, QString cacheDirectory = {},
                            QString overrideStorePath = {},
                            QObject* parent = nullptr);

    void setOptions(const ScanOptions& options) { m_options = options; }
    // The engine used for title screens; without one, previously imported
    // results can still be matched to songs.
    void setTitleScreenOcr(std::shared_ptr<TitleScreenOcrEngine> engine)
    {
        m_titleScreenOcr = std::move(engine);
    }
    bool hasTitleScreenOcr() const { return m_titleScreenOcr != nullptr; }
    // Library roots known from outside the catalogue. Every database this
    // worker opens is checked against them before SQLite touches it.
    void setKnownRoots(QStringList roots) { m_knownRoots = std::move(roots); }
    // The user-state store whose play history each scan reconnects to songs.
    void setUserStatePath(QString path) { m_userStatePath = std::move(path); }
    // Asks the next reprocess (only) to read title screens as well. Safe to
    // call from another thread before queueing reprocessMetadata().
    void requestTitleScreensOnce() { m_titleScreensRequested.store(true); }
    void prepareScan() { m_cancelled.store(false); }
    void setPaused(bool paused) { m_paused.store(paused); }
    bool isPaused() const { return m_paused.load(); }
    quint64 sourceFileReads() const { return m_sourceFileReads; }
    // Asks a running key-analysis batch to end after (or during) the current
    // song, so other work queued on this thread can start. Safe from any
    // thread. The controller clears it before queueing the next batch.
    void setKeyYield(bool yield) { m_keyYield.store(yield); }
    // One short batch of background key analysis in the active music folder
    // (at most about kKeyBatchMs of work), on this worker's thread. Ends
    // early when asked to yield or cancel, or when playback starts, and
    // always emits songKeyBatchFinished().
    void analyseSongKeys(std::shared_ptr<SongKeyEngine> engine);
    static constexpr int kKeyBatchMs = 8000;

public slots:
    void scan(const QString& rootPath);
    void reprocessMetadata();
    void requestCancel();

signals:
    void progress(const QString& phase, qint64 done, qint64 total,
                  const QString& currentRelPath);
    void finished(const QVariantMap& summary);
    void failed(const QString& message);
    void libraryReady();
    // "reason": "more" (work left), "done" (nothing left to try),
    // "stopped" (yield, cancel or playback), "offline", "unavailable";
    // plus the counts of SongKeySummary ("total", "analysed", "confident")
    // and of this batch ("decoded", "reused", "failed", "decodeMs").
    void songKeyBatchFinished(const QVariantMap& summary);

private:
    bool shouldStop() const;
    bool waitWhilePaused() const;
    bool pauseOutsideTransaction(class QSqlDatabase& database) const;
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
    bool readTitleScreens(Catalogue& catalogue, qint64 rootId, const QString& rootPath,
                          QVariantMap& counts, QString* error, bool recogniseNew = true);
    bool readSidecars(Catalogue& catalogue, qint64 rootId, const QString& rootPath,
                      QVariantMap& counts, QString* error);
    bool identifyDuplicates(Catalogue& catalogue, qint64 rootId, const QString& rootPath,
                            QVariantMap& counts, QString* error);
    void reconcilePlayHistory(Catalogue& catalogue, QVariantMap& counts);
    bool resolveAndSync(Catalogue& catalogue, qint64 rootId, const QString& phase,
                        QString* error, bool* cancelled = nullptr);

    QString m_databasePath;
    QString m_cacheDirectory;
    QString m_overrideStorePath;
    QStringList m_knownRoots;
    QString m_userStatePath;
    ScanOptions m_options;
    std::shared_ptr<TitleScreenOcrEngine> m_titleScreenOcr;
    std::atomic_bool m_cancelled = false;
    std::atomic_bool m_paused = false;
    std::atomic_bool m_titleScreensRequested = false;
    std::atomic_bool m_keyYield = false;
    // MP3 files that could not be read this session (not recorded, so a
    // later session tries again); key batches pass over them.
    QSet<qint64> m_keySkip;
    QElapsedTimer m_scanTimer;
    QElapsedTimer m_progressTimer;
    quint64 m_sourceFileReads = 0;
};
