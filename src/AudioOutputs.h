#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

typedef struct _GstElement GstElement;

// The computer's sound outputs (speakers, headphones, HDMI...), by the names
// the system shows for them. Choosing one is optional: without a choice the
// system's default output is used, as before.
namespace audio {

struct Output {
    QString id;     // what is remembered: the system's own id where it has one
    QString label;  // what is shown (names that repeat are told apart)
};
// The outputs that can play sound now.
QList<Output> outputs();
QStringList outputNames();
// A sound output element for an output (by id, or by the name shown), or
// nullptr if it is not there (unplugged, renamed). The caller owns the
// returned (floating) element.
GstElement* createSink(const QString& output);

// Finds the outputs on a worker thread (asking the system can take a moment)
// and remembers them for the whole session; refresh() looks again. Only one
// search runs at a time.
class OutputFinder : public QObject {
    Q_OBJECT

public:
    using Lister = std::function<QList<Output>()>;
    static OutputFinder* instance();

    bool hasList() const { return m_hasList; }
    bool isSearching() const { return m_searching; }
    QList<Output> outputs() const { return m_outputs; }
    void refresh();
    // Replaces the system search (tests).
    void setLister(Lister lister) { m_lister = std::move(lister); }
    void forget();

signals:
    void outputsChanged();
    void searchingChanged(bool searching);

private:
    using QObject::QObject;
    void start();

    Lister m_lister;
    QList<Output> m_outputs;
    bool m_hasList = false;
    bool m_searching = false;
    bool m_again = false;
    quint64 m_generation = 0;
};

// A short, quiet tone through an output (empty = the default), to check that
// it works. Deletes itself when the tone has finished.
class TestTone : public QObject {
    Q_OBJECT

public:
    // The tone playing now (or nullptr), or false with a message if it
    // cannot be played. Only one plays at a time.
    static TestTone* play(const QString& output, int volumePercent, QString* error = nullptr);
    static bool isPlaying();
    ~TestTone() override;

signals:
    void finished();

private:
    explicit TestTone(GstElement* pipeline);
    void poll();

    GstElement* m_pipeline;
};

} // namespace audio
