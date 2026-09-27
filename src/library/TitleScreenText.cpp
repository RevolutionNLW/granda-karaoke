#include "library/TitleScreenText.h"

#include <QRegularExpression>

#include <algorithm>
#include <cstdlib>

namespace {

int editDistance(const QString& a, const QString& b, int limit)
{
    if (std::abs(int(a.size()) - int(b.size())) > limit)
        return limit + 1;
    QList<int> previous(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        previous[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        QList<int> current(b.size() + 1);
        current[0] = i;
        int best = current[0];
        for (int j = 1; j <= b.size(); ++j) {
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                   previous[j - 1] + (a.at(i - 1) == b.at(j - 1) ? 0 : 1)});
            best = std::min(best, current[j]);
        }
        if (best > limit)
            return limit + 1;
        previous = current;
    }
    return previous[b.size()];
}

// Label logos and series banners, which OCR often garbles ("SUHFLY",
// "Siveer Georgia Brown", "MOWSTES" for MONSTER, "Soura Choice").
bool isLabelText(const QString& key)
{
    static const QStringList labels = {
        QStringLiteral("sunfly"), QStringLiteral("sweet georgia brown"), QStringLiteral("legends"),
        QStringLiteral("series"), QStringLiteral("legends series"), QStringLiteral("monster"),
        QStringLiteral("monster hits"), QStringLiteral("sound choice"), QStringLiteral("choice"),
        QStringLiteral("pocket songs"), QStringLiteral("priddis"), QStringLiteral("priddis music"),
        QStringLiteral("karaoke"), QStringLiteral("dkkaraoke"),
        QStringLiteral("dk karaoke"), QStringLiteral("zoom karaoke"), QStringLiteral("zoom karaoke hits"),
        QStringLiteral("top hits monthly"), QStringLiteral("top hits"), QStringLiteral("monthly"),
        QStringLiteral("pop hits monthly"), QStringLiteral("panorama s"), QStringLiteral("presents"),
        QStringLiteral("essential karaoke"), QStringLiteral("easykaraoke"), QStringLiteral("easy karaoke"),
        QStringLiteral("chartbuster"), QStringLiteral("mark s karaoke")};
    if (key.isEmpty())
        return true;
    static const QRegularExpression brandFragment(
        QStringLiteral(R"(karaok|georg|georsia|sunfl|suhfl|suntly|unfly|priddis|monst|houstes|howstes|mowstes|holstes|panorama|pocket s)"));
    if (brandFragment.match(key).hasMatch())
        return true;
    for (const QString& label : labels) {
        const int limit = label.size() >= 10 ? 3 : (label.size() >= 6 ? 2 : 1);
        if (editDistance(key, label, limit) <= limit)
            return true;
    }
    return false;
}

// Songwriter, publisher, copyright, key/tempo and production credit lines.
bool isCreditText(const QString& text, const QString& key)
{
    static const QRegularExpression credit(
        QStringLiteral(R"((©|\(c\)|copyright|\bltd\b|\blto\b|limited|\bcorp\b|publish|\bpub\b|\bpob\b|chappell|universal music|\bemi\b|\beml\b|sony|warner|\bbmg\b|\bmca\b|zomba|rondor|words\s*(?:&|and)\s*music|words by|music by|writers?\s*[:(]|writer\(s\)|written by|lyrics by|composed by|produced by|created by|^by\s*:|key\s*of\s*:?|^key\s*[:(]|time\s*:|www|\.com|\btrack\s*\d|\bmale\s*\d|\bmale\b.*\bfemale\b|ascap|\bbmi\b))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression year(QStringLiteral(R"((?:^|\D)(?:19|20)\d{2}(?:\D|$))"));
    static const QRegularExpression digitsOnly(QStringLiteral(R"(^[\d\s]+$)"));
    return credit.match(text).hasMatch() || year.match(text).hasMatch()
        || digitsOnly.match(key).hasMatch() || text.contains(QLatin1Char('/'));
}

QString cleanLine(QString text)
{
    text = text.simplified();
    static const QRegularExpression edges(QStringLiteral(R"(^["'“”‘’•·*®™\-]+|["'“”‘’•·*®™]+$)"));
    text.remove(edges);
    return text.simplified();
}

} // namespace

QString ocrKey(const QString& value)
{
    QString input = value;
    input.replace(QLatin1Char('&'), QStringLiteral(" and "));
    input = input.normalized(QString::NormalizationForm_KD).toCaseFolded();
    QString result;
    for (const QChar c : input) {
        const QChar::Category category = c.category();
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing)
            continue;
        result.append(c.isLetterOrNumber() ? c : QLatin1Char(' '));
    }
    return result.simplified();
}

TitleScreenReading readTitleScreen(const QList<OcrLine>& input)
{
    TitleScreenReading reading;
    QList<OcrLine> lines = input;
    std::stable_sort(lines.begin(), lines.end(),
                     [](const OcrLine& a, const OcrLine& b) { return a.y < b.y; });
    static const QRegularExpression cue(
        QStringLiteral(R"((?:in the style of|made famous by|as made popular by|popular by|as populari[sz]ed by|populari[sz]ed by|sung by|style of)\s*:?\s*(.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cueStart(
        QStringLiteral(R"(^(?:as\s+made|as\s+made\s+popular|made\s+popular|in\s+the\s+style|in\s+the\s+style\s+of|as\s+popularised|made\s+famous|of)$)"),
        QRegularExpression::CaseInsensitiveOption);
    struct Kept { OcrLine line; QString text; };
    QList<Kept> kept;
    bool artistNext = false;
    // The Priddis logo reads "Priddis" over "Music"; elsewhere "Music" can be a title.
    bool priddisLogo = false;
    for (const OcrLine& line : std::as_const(lines))
        priddisLogo = priddisLogo || ocrKey(line.text).contains(QLatin1String("priddis"));
    for (const OcrLine& line : std::as_const(lines)) {
        const QString text = cleanLine(line.text);
        const QString key = ocrKey(text);
        const QRegularExpressionMatch match = cue.match(text);
        if (match.hasMatch()) {
            const QString rest = cleanLine(match.captured(1));
            if (!rest.isEmpty() && reading.artist.isEmpty())
                reading.artist = rest;
            else
                artistNext = rest.isEmpty();
            continue;
        }
        if (cueStart.match(text).hasMatch()) {
            artistNext = true;
            continue;
        }
        if (isLabelText(key) || isCreditText(text, key) || key.size() < 2
            || (priddisLogo && key == QLatin1String("music")))
            continue;
        if (artistNext) {
            if (reading.artist.isEmpty())
                reading.artist = text;
            artistNext = false;
            continue;
        }
        kept.append({line, text});
    }
    if (kept.isEmpty()) {
        reading.reason = QStringLiteral("no_title_text");
        return reading;
    }
    // The title is the largest remaining text. A title that wraps over
    // several lines keeps a similar size and stays close together.
    double tallest = 0.0;
    int tallestIndex = 0;
    for (int i = 0; i < kept.size(); ++i) {
        if (kept.at(i).line.height > tallest) {
            tallest = kept.at(i).line.height;
            tallestIndex = i;
        }
    }
    auto similar = [&](const OcrLine& line) { return line.height >= 0.75 * tallest; };
    int first = tallestIndex;
    int last = tallestIndex;
    while (first > 0 && similar(kept.at(first - 1).line)
           && kept.at(first).line.y - (kept.at(first - 1).line.y + kept.at(first - 1).line.height)
                  < 1.2 * tallest)
        --first;
    while (last + 1 < kept.size() && similar(kept.at(last + 1).line)
           && kept.at(last + 1).line.y - (kept.at(last).line.y + kept.at(last).line.height)
                  < 1.2 * tallest)
        ++last;
    QStringList parts;
    for (int i = first; i <= last; ++i)
        parts.append(kept.at(i).text);
    reading.title = parts.join(QLatin1Char(' ')).simplified();
    reading.reason = last > first ? QStringLiteral("largest_text_block")
                                  : QStringLiteral("largest_text_line");
    return reading;
}
