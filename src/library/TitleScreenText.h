#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

// One line of text recognised on a CD+G title screen. The box is in fractions
// of the image, measured from its top-left corner.
struct OcrLine {
    QString text;
    double confidence = 0.0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// A local text-recognition engine. Implementations are platform specific
// (Apple Vision on macOS); none may use the network.
class TitleScreenOcrEngine {
public:
    virtual ~TitleScreenOcrEngine() = default;
    // Stable name recorded with every result, e.g. "apple-vision-accurate-1".
    virtual QString name() const = 0;
    // Recognises text in one CD+G frame (ARGB pixels as CdgDecoder renders).
    virtual bool recognise(const std::vector<std::uint32_t>& argb, int width, int height,
                           QList<OcrLine>* lines, QString* error) = 0;
};

// What a title screen says. Karaoke title screens usually show the song title
// in the largest text, plus label logos, songwriter and publisher credits and
// copyright lines. Most labels do not name the performer; an artist is only
// taken from an explicit cue such as "in the style of" or "made famous by".
struct TitleScreenReading {
    QString title;
    QString artist;
    QString reason;
};

TitleScreenReading readTitleScreen(const QList<OcrLine>& lines);

// Case-, accent- and punctuation-insensitive form used to compare OCR text.
QString ocrKey(const QString& value);
