#pragma once

#include "music/AudibleEnd.h"

#include <QHash>
#include <QPair>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

// Reads one song file and works out where its music ends (see
// music/AudibleEnd.h). The implementation decodes with the platform's audio
// components; tests give their own.
class AudibleEndEngine {
public:
    enum class Outcome {
        Analysed,     // result is filled in
        NotAudio,     // the file was read but its audio is damaged or unusable
        Unreadable,   // the file could not be read now, or a decoder is missing
        Interrupted,  // stop() asked to stop; nothing is known
    };

    virtual ~AudibleEndEngine() = default;
    // Opens the file read-only and writes nothing anywhere. stop() is asked
    // every few tens of milliseconds and ends the work promptly.
    virtual Outcome analyse(const QString& path, const std::function<bool()>& stop,
                            music::AudibleEnd* result, QString* detail) = 0;
};

// Decodes the whole MP3 once, at low priority, keeping only 50 ms loudness
// figures: decoding from the start gives exactly the timeline the player
// uses, which positions estimated from a part of the file (a seek into a
// variable-bitrate MP3) would not.
class GstAudibleEndEngine final : public AudibleEndEngine {
public:
    // Anything longer is played to its end.
    static constexpr int kMaxSeconds = 20 * 60;
    Outcome analyse(const QString& path, const std::function<bool()>& stop,
                    music::AudibleEnd* result, QString* detail) override;
};

// nullptr when GStreamer is not initialised.
std::shared_ptr<AudibleEndEngine> createAudibleEndEngine();

// Where songs' music ends, for Smart Autoplay End: worked out in the
// background, one song at a time, and kept in the enrichment cache by the
// MP3's complete audio (mp3CompleteAudioDigest, with kAudibleEndVersion), so each
// recording is analysed once whatever its name or folder. The music folder
// is only ever read.
class AudibleEnds : public QObject {
    Q_OBJECT

public:
    using EngineFactory = std::function<std::shared_ptr<AudibleEndEngine>()>;

    // cachePath: the enrichment cache (empty: results last this session only).
    explicit AudibleEnds(QString cachePath, QObject* parent = nullptr);
    ~AudibleEnds() override;

    void setEngineFactory(EngineFactory factory) { m_factory = std::move(factory); }

    // Works out where the song's music ends, unless it is known or being
    // worked out; ready() follows. A request made while another song is being
    // analysed waits for it (only the latest waiting request is kept).
    void request(const QString& mp3Path);
    // What is known about the song this session: nullopt while unknown (not
    // asked, being worked out, or the file could not be read).
    std::optional<music::AudibleEnd> known(const QString& mp3Path) const;
    // Looks, in the background, whether the file is still the one analysed
    // (same size and modification time); fileChecked(token, ...) follows.
    // Never on the interface thread: the music drive may be slow or gone.
    quint64 checkFile(const QString& mp3Path);
    // A song is being worked out (or its result being kept).
    bool isWorking() const { return !m_working.isEmpty(); }

    struct FileStamp {
        qint64 size = -1;
        qint64 modifiedMs = -1;
        bool operator==(const FileStamp&) const = default;
    };
    static FileStamp stampOf(const QString& path);

signals:
    // What is known about the song (known()) has been worked out, or could not be.
    void ready(const QString& mp3Path);
    void fileChecked(quint64 token, bool unchanged);

private:
    struct Known {
        music::AudibleEnd end;
        FileStamp file;  // as it was when its content was read
    };
    void start(const QString& mp3Path);
    void available(const QString& mp3Path, const std::optional<music::AudibleEnd>& result,
                   FileStamp file);
    void jobDone();
    void startCheck(const QString& mp3Path, quint64 token);

    QString m_cachePath;
    EngineFactory m_factory;
    std::shared_ptr<std::atomic_bool> m_stop;
    QHash<QString, Known> m_known;
    QString m_working;
    QString m_waiting;
    quint64 m_lastToken = 0;
    bool m_checking = false;
    QPair<QString, quint64> m_nextCheck;
};
