#include "library/SidecarParser.h"

#include "library/FilenameParser.h"

#include <QHash>
#include <QSet>
#include <QRegularExpression>
#include <QStringDecoder>

namespace {

QString decodeText(const QByteArray& contents)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(contents);
    if (!utf8.hasError())
        return text;
    // The generators of these lists were Windows programs: fall back to the
    // Windows Latin code page for bytes that are not valid UTF-8.
    QStringDecoder windows("windows-1252");
    if (windows.isValid())
        return windows.decode(contents);
    return QString::fromLatin1(contents);
}

bool isCommentOrBlank(const QString& line)
{
    const QString trimmed = line.trimmed();
    return trimmed.isEmpty() || trimmed.startsWith(QLatin1String("REM"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char('*'))
        || trimmed.startsWith(QLatin1Char(';'));
}

constexpr int kMinimumEntries = 3;
constexpr double kMinimumEntryShare = 0.8;
constexpr double kMinimumDiscShare = 0.8;

} // namespace

SidecarTrackList parseTrackListSidecar(const QByteArray& contents)
{
    SidecarTrackList result;
    if (contents.contains('\0')) {
        result.reason = QStringLiteral("binary");
        return result;
    }
    static const QRegularExpression listNumber(QStringLiteral(R"(^\s*\d{1,3}\.\s+)"));
    static const QRegularExpression mediaSuffix(QStringLiteral(R"(\.(?:mp3|cdg|zip)\s*$)"),
                                                QRegularExpression::CaseInsensitiveOption);
    const QStringList lines = decodeText(contents).split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
    int contentLines = 0;
    QList<SidecarEntry> parsed;
    for (int i = 0; i < lines.size(); ++i) {
        if (isCommentOrBlank(lines.at(i)))
            continue;
        ++contentLines;
        QString line = lines.at(i).trimmed();
        line.remove(listNumber);
        line.remove(mediaSuffix);
        // The filename parser already knows every disc-track shape in this
        // collection; the line is parsed exactly as a file of that name.
        const ParsedName name = parseSongName(QString(), line.trimmed() + QStringLiteral(".mp3"));
        if (name.kind != ParsedName::Kind::DiscTrackFields || name.fields.size() != 2
            || name.track <= 0 || name.discSource != QLatin1String("filename"))
            continue;
        parsed.append({name.discId, name.track, name.fields, i + 1});
    }
    if (contentLines < kMinimumEntries || parsed.size() < kMinimumEntries) {
        result.reason = QStringLiteral("too_few_entries");
        return result;
    }
    if (double(parsed.size()) < kMinimumEntryShare * double(contentLines)) {
        result.reason = QStringLiteral("not_a_track_list");
        return result;
    }
    QHash<QString, int> discs;
    for (const SidecarEntry& entry : std::as_const(parsed))
        ++discs[entry.discId];
    QString disc;
    int discCount = 0;
    for (auto it = discs.cbegin(); it != discs.cend(); ++it) {
        if (it.value() > discCount) {
            disc = it.key();
            discCount = it.value();
        }
    }
    if (double(discCount) < kMinimumDiscShare * double(parsed.size())) {
        result.reason = QStringLiteral("mixed_discs");
        return result;
    }
    QHash<int, int> seen;
    QList<SidecarEntry> entries;
    QSet<int> ambiguous;
    for (const SidecarEntry& entry : std::as_const(parsed)) {
        if (entry.discId != disc)
            continue;
        const auto previous = seen.constFind(entry.track);
        if (previous != seen.cend()) {
            const SidecarEntry& other = entries.at(*previous);
            if (normalizeForSearch(other.fields.join(QLatin1Char(' ')))
                != normalizeForSearch(entry.fields.join(QLatin1Char(' '))))
                ambiguous.insert(entry.track);
            continue;
        }
        seen.insert(entry.track, entries.size());
        entries.append(entry);
    }
    for (const SidecarEntry& entry : std::as_const(entries)) {
        if (!ambiguous.contains(entry.track))
            result.entries.append(entry);
    }
    result.recognised = !result.entries.isEmpty();
    result.discId = disc;
    result.reason = result.recognised ? QString() : QStringLiteral("ambiguous");
    return result;
}
