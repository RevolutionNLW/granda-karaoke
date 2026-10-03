#include "library/MetadataResolver.h"

#include "library/Catalogue.h"
#include "library/FilenameParser.h"
#include "library/Id3Reader.h"
#include "library/KaraokeLabels.h"
#include "library/TitleScreenText.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

enum class Order { Unknown, ArtistTitle, TitleArtist };
enum class StructuralKind { None, PersonalName, LocalRecurrence, Ensemble, GlobalRecurrence };

struct Candidate {
    qint64 sourceId = 0;
    qint64 songId = 0;
    qint64 rootId = 0;
    QString relDir;
    QString rawPath;
    QString discId;
    QString discPrefix;
    QString fallbackFolder;
    int track = 0;
    QStringList rawFields;
    QStringList fields;
    QString id3Title;
    QString id3Artist;
    QJsonObject parsed;
    QJsonObject rawTags;
    QVariant manualTitle;
    QVariant manualArtist;
    QStringList trustedSearch;  // trusted label, series, disc and track
    bool bareHyphenSplit = false;
    QString unsplitField;
    QString unsplitRawField;
    bool codeFieldsDropped = false;
    bool capAtMedium = false;  // a split justified by its disc's shape, not proven
    bool underscoreAfterCode = false;
    // Names from a disc track list beside the song (the song's own filename
    // had none), or a track list that names this song differently.
    bool fromSidecar = false;
    bool sidecarDiscExact = false;
    QString sidecarPath;
    QStringList sidecarFields;
    int sidecarLine = 0;
    Order tagOrder = Order::Unknown;
    Order structuralOrder = Order::Unknown;
    int structuralWeight = 0;
    StructuralKind structuralKind = StructuralKind::None;
    bool structuralStrong = false;
    Order voteOrder = Order::Unknown;
    int voteWeight = 0;
};

struct VoteCounts {
    int artistTitleWeight = 0;
    int titleArtistWeight = 0;
    int artistTitleCount = 0;
    int titleArtistCount = 0;
    int artistTitleReliableCount = 0;
    int titleArtistReliableCount = 0;
    // Votes that come from file names rather than ID3 tags.
    int artistTitleStructural = 0;
    int titleArtistStructural = 0;
};

struct Evidence {
    QHash<QString, VoteCounts> groupVotes;
    QHash<QString, VoteCounts> seriesVotes;
};

struct Decision {
    Order order = Order::Unknown;
    QString source = QStringLiteral("fallback");
    QString confidence = QStringLiteral("unresolved");
};

Decision decide(const Candidate& candidate, const Evidence& evidence, bool hideOwnTag);

QString collapseWhitespace(QString value)
{
    value.replace(QChar(0x00a0), QLatin1Char(' '));
    // Patterns are made once: making one costs far more on Windows.
    static thread_local const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
    value.replace(whitespace, QStringLiteral(" "));
    return value.trimmed();
}

QString cleanedField(QString value)
{
    value = collapseWhitespace(value);
    static const QList<QRegularExpression> markers = {
        QRegularExpression(QStringLiteral(R"(^\s*(?:VOCALS?|W\s*/?\s*VOCALS?)\s*[-:]\s*)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(^\s*(?:\[VOCALS?\]|\(VOCALS?\))\s*[-:]?\s*)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\s+(?:W\s*[~/-]?\s*VOCALS?|WITH\s+VOCALS?)\s*$)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\s*[-:]\s*(?:VOCALS?|W\s*/?\s*VOCALS?)\s*$)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\s*(?:\[VOCALS?\]|\(VOCALS?\))\s*$)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\s+VOCALS?\s*$)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\s*\((?:PRO|KARARADIO|VERSION)\)\s*$)"),
                           QRegularExpression::CaseInsensitiveOption)
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const QRegularExpression& marker : markers) {
            const QString before = value;
            value.remove(marker);
            value = collapseWhitespace(value);
            changed = changed || value != before;
        }
    }
    return value;
}

QString lettersAndDigits(const QString& value)
{
    QString result;
    for (const QChar c : value.normalized(QString::NormalizationForm_KD).toCaseFolded()) {
        if (c.isLetterOrNumber())
            result.append(c);
    }
    return result;
}

bool isOwnDiscTrackToken(const QString& value, const QString& discId, int track)
{
    if (discId.isEmpty() || track <= 0)
        return false;
    const QString token = lettersAndDigits(value);
    const QString disc = lettersAndDigits(discId);
    if (!token.startsWith(disc) || token.size() <= disc.size())
        return false;
    const QString trackPart = token.mid(disc.size());
    static const QRegularExpression digits(QStringLiteral(R"(^\d{1,4}$)"));
    return digits.match(trackPart).hasMatch() && trackPart.toInt() == track;
}

QString stripOwnDiscTrackToken(QString value, const QString& discId, int track)
{
    auto isBoundary = [](QChar c) {
        return c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char(':')
            || c == QLatin1Char('_');
    };
    for (qsizetype i = 1; i < value.size(); ++i) {
        if (!isBoundary(value.at(i)) || !isOwnDiscTrackToken(value.left(i), discId, track))
            continue;
        qsizetype start = i;
        while (start < value.size() && isBoundary(value.at(start)))
            ++start;
        if (start < value.size())
            return collapseWhitespace(value.mid(start));
    }
    for (qsizetype i = 0; i + 1 < value.size(); ++i) {
        if (!isBoundary(value.at(i)))
            continue;
        qsizetype start = i + 1;
        while (start < value.size() && isBoundary(value.at(start)))
            ++start;
        if (start < value.size()
            && isOwnDiscTrackToken(value.mid(start), discId, track)) {
            qsizetype end = i;
            while (end > 0 && isBoundary(value.at(end - 1)))
                --end;
            if (end > 0)
                return collapseWhitespace(value.left(end));
        }
    }
    return value;
}

QString cleanedCandidateField(const QString& value, const QString& discId, int track)
{
    QString result = stripOwnDiscTrackToken(cleanedField(value), discId, track);
    static const QRegularExpression leadingNumber(
        QStringLiteral(R"(^0*(\d{1,4})(?:\s+|[._-]\s*)(.+)$)"));
    const QRegularExpressionMatch match = leadingNumber.match(result);
    if (track > 0 && match.hasMatch() && match.captured(1).toInt() == track)
        result = collapseWhitespace(match.captured(2));
    return result;
}

QString fieldKey(const QString& value)
{
    return normalizeForSearch(cleanedField(value));
}

QString recurrenceKey(const QString& value)
{
    QString key = fieldKey(value);
    if (key.startsWith(QStringLiteral("the ")))
        key.remove(0, 4);
    if (key.endsWith(QStringLiteral(" the")))
        key.chop(4);
    return key.trimmed();
}

bool plainCapitalisedNameWords(const QString& value)
{
    const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty() || words.size() > 3)
        return false;
    static const QRegularExpression word(
        QStringLiteral(R"(^\p{Lu}[\p{L}'\x{2019}-]*(?:\.?)$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    for (const QString& part : words) {
        if (!word.match(part).hasMatch())
            return false;
    }
    return true;
}

QString personalFirstNames(const QString& right)
{
    static const QRegularExpression collaborators(
        QStringLiteral(R"(\s+(?:&|Feat\.?|Ft\.?|Featuring|With)\s*.*$)"),
        QRegularExpression::CaseInsensitiveOption);
    QString result = right;
    result.remove(collaborators);
    return result.trimmed();
}

bool looksLikePersonalName(const QString& value)
{
    static thread_local const QRegularExpression digit(QStringLiteral(R"(\d)"));
    if (value.count(QLatin1Char(',')) != 1 || value.contains(digit))
        return false;
    const qsizetype comma = value.indexOf(QLatin1Char(','));
    const QString left = value.left(comma).trimmed();
    const QString right = value.mid(comma + 1).trimmed();
    if (left.isEmpty() || right.isEmpty())
        return false;
    static const QRegularExpression article(QStringLiteral(R"(^(?:A|An|The)$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (article.match(right).hasMatch() || fieldKey(left) == fieldKey(right))
        return false;
    return plainCapitalisedNameWords(personalFirstNames(right));
}

bool looksLikePlainPersonalName(const QString& value)
{
    if (!looksLikePersonalName(value))
        return false;
    const QString right = value.mid(value.indexOf(QLatin1Char(',')) + 1).trimmed();
    return right == personalFirstNames(right);
}

bool isPersonalSuffix(const QString& value)
{
    static const QRegularExpression suffix(
        QStringLiteral(R"(^(?:Jr\.?|Sr\.?|II|III)$)"),
        QRegularExpression::CaseInsensitiveOption);
    return suffix.match(value).hasMatch();
}

bool looksLikeDisplayPersonalName(const QString& value)
{
    if (!looksLikePlainPersonalName(value))
        return false;
    const QString right = value.mid(value.indexOf(QLatin1Char(',')) + 1).trimmed();
    const QStringList words = right.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return words.size() == 1 || (words.size() == 2 && isPersonalSuffix(words.last()));
}

bool looksLikeEnsemble(const QString& value)
{
    static const QRegularExpression ensemble(
        QStringLiteral(R"((?:\s&\s|\s(?:Feat\.?|Ft\.?|Featuring|With)\s*))"),
        QRegularExpression::CaseInsensitiveOption);
    return ensemble.match(value).hasMatch();
}

QString displayArtist(QString value)
{
    value = cleanedField(value);
    if (value.count(QLatin1Char(',')) != 1)
        return value;
    const qsizetype comma = value.indexOf(QLatin1Char(','));
    const QString left = value.left(comma).trimmed();
    const QString right = value.mid(comma + 1).trimmed();
    if (left.isEmpty() || right.isEmpty())
        return value;
    static const QRegularExpression article(QStringLiteral(R"(^(?:A|An|The)$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (article.match(right).hasMatch())
        return QStringLiteral("%1 %2").arg(right, left);
    if (looksLikeDisplayPersonalName(value)) {
        QStringList words = right.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString suffix;
        if (words.size() == 2 && isPersonalSuffix(words.last()))
            suffix = words.takeLast();
        QString result = QStringLiteral("%1 %2").arg(words.join(QLatin1Char(' ')), left);
        if (!suffix.isEmpty())
            result += QLatin1Char(' ') + suffix;
        return result;
    }
    return value;
}

Order priorFor(const QString& prefix)
{
    static const QSet<QString> titleArtist = {QStringLiteral("DK"), QStringLiteral("PI")};
    static const QSet<QString> artistTitle = {
        QStringLiteral("SF"), QStringLiteral("SFG"), QStringLiteral("SFMW"),
        QStringLiteral("MH"), QStringLiteral("ZKH"), QStringLiteral("ZPA"),
        QStringLiteral("ZMP"), QStringLiteral("ZML"), QStringLiteral("MRE"),
        QStringLiteral("MRH")};
    if (titleArtist.contains(prefix))
        return Order::TitleArtist;
    if (artistTitle.contains(prefix))
        return Order::ArtistTitle;
    return Order::Unknown;
}

QString voteGroupKey(const Candidate& candidate);

QString groupKey(const Candidate& candidate)
{
    const QString group = candidate.discId.isEmpty() ? candidate.relDir.toCaseFolded()
                                                     : candidate.discId.toCaseFolded();
    return QString::number(candidate.rootId) + QLatin1Char(':') + group;
}

QString seriesKey(const Candidate& candidate)
{
    QString series = candidate.discPrefix;
    if (series.isEmpty())
        series = candidate.relDir.section(QLatin1Char('/'), 0, 0);
    return QString::number(candidate.rootId) + QLatin1Char(':') + series.toCaseFolded();
}

void addVote(VoteCounts& counts, Order order, int weight, bool fromTag)
{
    if (order == Order::ArtistTitle) {
        counts.artistTitleWeight += weight;
        ++counts.artistTitleCount;
        if (weight >= 2)
            ++counts.artistTitleReliableCount;
        if (!fromTag)
            ++counts.artistTitleStructural;
    } else if (order == Order::TitleArtist) {
        counts.titleArtistWeight += weight;
        ++counts.titleArtistCount;
        if (weight >= 2)
            ++counts.titleArtistReliableCount;
        if (!fromTag)
            ++counts.titleArtistStructural;
    }
}

VoteCounts withoutCandidate(VoteCounts counts, const Candidate& candidate)
{
    const bool fromTag = candidate.tagOrder != Order::Unknown;
    if (candidate.voteOrder == Order::ArtistTitle) {
        counts.artistTitleWeight -= candidate.voteWeight;
        --counts.artistTitleCount;
        if (candidate.voteWeight >= 2)
            --counts.artistTitleReliableCount;
        if (!fromTag)
            --counts.artistTitleStructural;
    } else if (candidate.voteOrder == Order::TitleArtist) {
        counts.titleArtistWeight -= candidate.voteWeight;
        --counts.titleArtistCount;
        if (candidate.voteWeight >= 2)
            --counts.titleArtistReliableCount;
        if (!fromTag)
            --counts.titleArtistStructural;
    }
    return counts;
}

Order strongVote(const VoteCounts& counts, bool allowTagOnly);

Order strongVote(const VoteCounts& counts)
{
    // ID3 tags are supporting evidence only: tag votes alone never make a
    // disc's order strong, at least one vote must come from a file name.
    return strongVote(counts, false);
}

Order strongVote(const VoteCounts& counts, bool allowTagOnly)
{
    auto strong = [allowTagOnly](int winningWeight, int losingWeight, int winningCount,
                                 int reliableCount, int structural) {
        return winningCount >= 2 && winningWeight >= 6
            && reliableCount >= 1 && (structural >= 1 || allowTagOnly)
            && winningWeight - losingWeight >= 4
            && winningWeight >= (2 * losingWeight + 2);
    };
    if (strong(counts.artistTitleWeight, counts.titleArtistWeight,
               counts.artistTitleCount, counts.artistTitleReliableCount,
               counts.artistTitleStructural))
        return Order::ArtistTitle;
    if (strong(counts.titleArtistWeight, counts.artistTitleWeight,
               counts.titleArtistCount, counts.titleArtistReliableCount,
               counts.titleArtistStructural))
        return Order::TitleArtist;
    return Order::Unknown;
}

Order consistentVote(const VoteCounts& counts)
{
    if (counts.artistTitleWeight >= 2
        && counts.artistTitleWeight >= counts.titleArtistWeight + 2)
        return Order::ArtistTitle;
    if (counts.titleArtistWeight >= 2
        && counts.titleArtistWeight >= counts.artistTitleWeight + 2)
        return Order::TitleArtist;
    return Order::Unknown;
}

Order weakVote(const VoteCounts& counts)
{
    if (counts.artistTitleWeight > counts.titleArtistWeight)
        return Order::ArtistTitle;
    if (counts.titleArtistWeight > counts.artistTitleWeight)
        return Order::TitleArtist;
    return Order::Unknown;
}

QString databaseError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

// Cooperative cancellation for the resolver's stages. It is consulted once per
// item and never blocks, so a request to stop is seen within milliseconds.
// Once it has said stop it keeps saying so, and every later stage returns at
// once; the caller then discards everything built in memory.
class StopCheck {
public:
    StopCheck() = default;  // never stops
    explicit StopCheck(const std::function<bool()>& cancelled)
        : m_cancelled(cancelled ? &cancelled : nullptr)
    {
    }
    bool operator()()
    {
        if (!m_stopped && m_cancelled)
            m_stopped = (*m_cancelled)();
        return m_stopped;
    }
    bool stopped() const { return m_stopped; }

private:
    const std::function<bool()>* m_cancelled = nullptr;
    bool m_stopped = false;
};

bool loadCandidates(QSqlDatabase database, qint64 rootId, QList<Candidate>* candidates,
                    StopCheck& stop, QString* error)
{
    QString sql = QStringLiteral(
        "SELECT s.id,s.song_id,s.root_id,s.parsed_json,COALESCE(f.rel_dir,''),"
        "COALESCE(f.rel_path,s.zip_mp3_member,''),COALESCE(f.raw_tags_json,''),"
        "so.manual_title,so.manual_artist,so.manual_label,so.manual_series,"
        "so.manual_disc_id,so.manual_track "
        "FROM sources s JOIN songs so ON so.id=s.song_id "
        "LEFT JOIN files f ON f.id=s.mp3_file_id ");
    if (rootId >= 0)
        sql += QStringLiteral("WHERE s.root_id=? ");
    sql += QStringLiteral(
        "ORDER BY s.song_id,CASE s.kind WHEN 'loose_cdg' THEN 0 "
        "WHEN 'loose_mcg' THEN 1 ELSE 2 END,s.id");
    QSqlQuery query(database);
    query.prepare(sql);
    if (rootId >= 0)
        query.addBindValue(rootId);
    if (!query.exec()) {
        if (error)
            *error = databaseError(query, QStringLiteral("Could not load metadata inputs"));
        return false;
    }
    QSet<qint64> seenSongs;
    while (query.next()) {
        if (stop())
            return true;
        const qint64 songId = query.value(1).toLongLong();
        if (seenSongs.contains(songId))
            continue;
        seenSongs.insert(songId);
        const QJsonObject parsed = QJsonDocument::fromJson(query.value(3).toByteArray()).object();
        const QJsonObject tags = QJsonDocument::fromJson(query.value(6).toByteArray()).object();
        Candidate candidate;
        candidate.sourceId = query.value(0).toLongLong();
        candidate.songId = songId;
        candidate.rootId = query.value(2).toLongLong();
        candidate.relDir = query.value(4).toString();
        candidate.rawPath = parsed.value(QStringLiteral("rawPath")).toString(query.value(5).toString());
        candidate.discId = parsed.value(QStringLiteral("discId")).toString();
        candidate.discPrefix = parsed.value(QStringLiteral("discPrefix")).toString();
        candidate.fallbackFolder = parsed.value(QStringLiteral("fallbackFolder")).toString();
        candidate.track = parsed.value(QStringLiteral("track")).toInt();
        for (const QJsonValue& value : parsed.value(QStringLiteral("fields")).toArray()) {
            candidate.rawFields.append(value.toString());
            candidate.fields.append(cleanedCandidateField(value.toString(), candidate.discId,
                                                          candidate.track));
        }
        if (parsed.value(QStringLiteral("underscoreSplit")).toBool() && candidate.fields.size() == 2) {
            // Treated like a bare-hyphen split: shown split only with strong
            // order evidence, never voting, never above medium.
            candidate.bareHyphenSplit = true;
            candidate.underscoreAfterCode = parsed.value(QStringLiteral("underscoreAfterCode")).toBool();
            candidate.unsplitRawField = parsed.value(QStringLiteral("unsplit")).toString();
            candidate.unsplitField = cleanedCandidateField(candidate.unsplitRawField,
                                                           candidate.discId, candidate.track);
        }
        candidate.id3Title = tags.value(QStringLiteral("title")).toString();
        candidate.id3Artist = tags.value(QStringLiteral("artist")).toString();
        candidate.parsed = parsed;
        candidate.rawTags = tags;
        candidate.manualTitle = query.value(7);
        candidate.manualArtist = query.value(8);
        for (int column = 9; column <= 12; ++column) {
            if (!query.value(column).isNull())
                candidate.trustedSearch.append(query.value(column).toString());
        }
        candidates->append(candidate);
    }
    return true;
}

QString digitsOf(const QString& value)
{
    QString digits;
    for (const QChar c : value) {
        if (c.isDigit())
            digits.append(c);
    }
    return digits;
}

// Disc IDs must match exactly, except for reviewed aliases of one label's
// prefix: "SUNFLY063" (file) and "SF063" (track list) are the same disc, but
// "SC123" and "SF123" are not.
bool discsCompatible(const QString& song, const QString& list)
{
    if (song.compare(list, Qt::CaseInsensitive) == 0)
        return true;
    auto canonical = [](const QString& disc) {
        QString letters;
        for (const QChar c : disc) {
            if (c.isLetter())
                letters.append(c.toUpper());
        }
        static const QHash<QString, QString> aliases = {{QStringLiteral("SUNFLY"), QStringLiteral("SF")}};
        return aliases.value(letters, letters) + QLatin1Char(':')
            + QString::number(digitsOf(disc).toULongLong());
    };
    return !digitsOf(song).isEmpty() && canonical(song) == canonical(list);
}

QString unorderedKey(const QStringList& fields)
{
    QStringList keys;
    for (const QString& field : fields)
        keys.append(fieldKey(field));
    keys.sort();
    return keys.join(QChar(0x1f));
}

bool applySidecars(QSqlDatabase database, qint64 rootId, QList<Candidate>& candidates,
                   StopCheck& stop, QString* error)
{
    struct Entry { QString disc; QStringList fields; QString path; int line; };
    QString sql = QStringLiteral(
        "SELECT f.root_id,f.rel_dir,e.disc_id,e.track,e.fields_json,f.rel_path,e.line "
        "FROM sidecar_entries e JOIN sidecar_files sf ON sf.file_id=e.file_id "
        "JOIN files f ON f.id=e.file_id WHERE sf.state='track_list' AND f.present=1");
    if (rootId >= 0)
        sql += QStringLiteral(" AND f.root_id=?");
    QSqlQuery query(database);
    query.prepare(sql);
    if (rootId >= 0)
        query.addBindValue(rootId);
    if (!query.exec()) {
        if (error)
            *error = databaseError(query, QStringLiteral("Could not load track lists"));
        return false;
    }
    QHash<QString, QList<Entry>> byFolderTrack;
    while (query.next()) {
        if (stop())
            return true;
        QStringList fields;
        for (const QJsonValue& value : QJsonDocument::fromJson(query.value(4).toByteArray()).array())
            fields.append(value.toString());
        if (fields.size() != 2)
            continue;
        const QString key = query.value(0).toString() + QLatin1Char(':')
            + query.value(1).toString().toCaseFolded() + QLatin1Char(':')
            + QString::number(query.value(3).toInt());
        byFolderTrack[key].append({query.value(2).toString(), fields,
                                   query.value(5).toString(), query.value(6).toInt()});
    }
    if (byFolderTrack.isEmpty())
        return true;
    for (Candidate& candidate : candidates) {
        if (stop())
            return true;
        if (candidate.track <= 0)
            continue;
        const QString key = QString::number(candidate.rootId) + QLatin1Char(':')
            + candidate.relDir.toCaseFolded() + QLatin1Char(':')
            + QString::number(candidate.track);
        const auto found = byFolderTrack.constFind(key);
        if (found == byFolderTrack.cend())
            continue;
        QList<Entry> matching;
        for (const Entry& entry : *found) {
            if (candidate.discId.isEmpty() || discsCompatible(candidate.discId, entry.disc))
                matching.append(entry);
        }
        if (matching.isEmpty())
            continue;
        // Two lists in one folder that name the track differently prove nothing.
        const QString firstKey = unorderedKey(matching.first().fields);
        bool agree = true;
        for (const Entry& entry : std::as_const(matching))
            agree = agree && unorderedKey(entry.fields) == firstKey;
        if (!agree)
            continue;
        const Entry& entry = matching.first();
        candidate.sidecarPath = entry.path;
        candidate.sidecarFields = entry.fields;
        candidate.sidecarLine = entry.line;
        candidate.sidecarDiscExact = candidate.discId.compare(entry.disc, Qt::CaseInsensitive) == 0;
        if (candidate.fields.isEmpty()) {
            candidate.rawFields = entry.fields;
            candidate.fields.clear();
            for (const QString& field : entry.fields)
                candidate.fields.append(cleanedCandidateField(field, candidate.discId,
                                                              candidate.track));
            candidate.fromSidecar = true;
        }
    }
    return true;
}

// "HM136 - Homemade (Irish) - Joe McDonnell", "MMVE3-32 - 08 - Presley, Elvis
// - If You Don't Come Back": catalogue codes and track numbers in name
// positions. A field is a code only when it has no spaces and is a bare track
// number, a hyphenated letter/digit code, or letters with three or more
// digits, so band names such as "Level 42", "UB40" or "Blink 182" stay names.
bool isCodeField(const QString& value)
{
    QString field = value.trimmed();
    static thread_local const QRegularExpression proPrefix(
        QStringLiteral(R"(^\(\s*pro\s*\)\s*)"), QRegularExpression::CaseInsensitiveOption);
    field.remove(proPrefix);
    if (field.contains(QLatin1Char(' ')))
        return false;
    static const QRegularExpression trackNumber(QStringLiteral(R"(^\d{1,3}$)"));
    static const QRegularExpression hyphenCode(
        QStringLiteral(R"(^(?=[0-9]*[A-Za-z])[A-Za-z0-9]{1,12}-\d{1,8}(?:-\d{1,4})?$)"));
    static const QRegularExpression packedCode(
        QStringLiteral(R"(^[A-Za-z]{1,10}\d{3,}[A-Za-z0-9]*(?:-\d{1,4})?$|^[A-Za-z]{1,6}\d[A-Za-z]\d{1,4}$)"));
    return trackNumber.match(field).hasMatch() || hyphenCode.match(field).hasMatch()
        || packedCode.match(field).hasMatch();
}

// A field that is only a karaoke marker ("wvocals", "(Pro)").
bool isMarkerField(const QString& value)
{
    static const QRegularExpression marker(
        QStringLiteral(R"(^\s*[\[(]?\s*(?:w\s*[/~-]?\s*vocals?|with\s+vocals?|vocals?|pro)\s*[\])]?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return marker.match(value).hasMatch();
}

QString letterSkeleton(const QString& value)
{
    QString result;
    for (const QChar c : value.toCaseFolded()) {
        if (c.isLetter())
            result.append(c);
    }
    return result;
}

// Names with three or more fields keep their two real names when the other
// fields are codes. The order is then decided by the usual evidence, but a
// name repaired this way is never more than medium confidence and never
// votes for its disc or folder.
void dropCodeFields(QList<Candidate>& candidates, StopCheck& stop)
{
    // A short capitalised or numbered first field that recurs across three or
    // more three-field names in one folder or disc ("KV - ...", "XXX 05 -
    // ...") while the names after it vary is a label code. A recurring plain
    // name ("Presley, Elvis - ...") is the artist of that folder and is kept.
    static const QRegularExpression labelShape(
        QStringLiteral(R"(^(?:[A-Z]{1,3}|[A-Za-z]{1,8}\s*\d{1,4})$)"));
    QHash<QString, QSet<QString>> leadingSeen;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return;
        if (candidate.fields.size() < 3
            || !labelShape.match(candidate.rawFields.value(0).trimmed()).hasMatch())
            continue;
        const QString skeleton = letterSkeleton(candidate.fields.first());
        if (!skeleton.isEmpty())
            leadingSeen[groupKey(candidate) + QChar(0x1f) + skeleton]
                .insert(fieldKey(candidate.fields.at(1)));
    }
    for (Candidate& candidate : candidates) {
        if (stop())
            return;
        if (candidate.fields.size() < 3)
            continue;
        const QString skeleton = letterSkeleton(candidate.fields.first());
        const bool recurringLabel = !skeleton.isEmpty()
            && labelShape.match(candidate.rawFields.value(0).trimmed()).hasMatch()
            && leadingSeen.value(groupKey(candidate) + QChar(0x1f) + skeleton).size() >= 3;
        static const QRegularExpression bareNumber(QStringLiteral(R"(^\s*\d{1,3}\s*$)"));
        QStringList fields;
        QStringList raw;
        bool previousDropped = true;  // a number first in the name is a track
        for (int i = 0; i < candidate.fields.size(); ++i) {
            const QString value = candidate.rawFields.value(i);
            // A lone number is a track only first or right after a code; in a
            // name position it may be a title ("3", "22").
            const bool number = bareNumber.match(value).hasMatch();
            const bool drop = number ? previousDropped
                                     : ((i == 0 && recurringLabel) || isMarkerField(value)
                                        || isCodeField(value) || isCodeField(candidate.fields.at(i)));
            previousDropped = drop;
            if (drop)
                continue;
            fields.append(candidate.fields.at(i));
            raw.append(candidate.rawFields.value(i));
        }
        if (fields.size() != 2)
            continue;
        candidate.fields = fields;
        candidate.rawFields = raw;
        candidate.codeFieldsDropped = true;
    }
}

int wordCount(const QString& value)
{
    return int(value.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
}

// "Everly Brothers-Bye Bye Love": one hyphen with no spaces joining two names.
// Hyphens are common inside real names ("Jay-Z", "Twenty-Four Hours"), so the
// name is split only when both sides have several words, or when at least
// three names in the same disc/folder share this shape. The result is never
// better than medium confidence.
void splitBareHyphenFields(QList<Candidate>& candidates, StopCheck& stop)
{
    // An underscore is taken as a field separator only when several names on
    // the same disc or folder are written that way; a lone underscore
    // ("Bridge_Over Troubled Water") is an ordinary space.
    QHash<QString, int> underscoreShaped;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return;
        if (candidate.bareHyphenSplit && !candidate.unsplitRawField.isEmpty())
            ++underscoreShaped[groupKey(candidate)];
    }
    for (Candidate& candidate : candidates) {
        if (stop())
            return;
        if (!candidate.bareHyphenSplit || candidate.unsplitRawField.isEmpty())
            continue;
        if (underscoreShaped.value(groupKey(candidate)) < 3 && !candidate.underscoreAfterCode) {
            candidate.fields = {candidate.unsplitField};
            candidate.rawFields = {candidate.unsplitRawField};
        } else {
            // The whole disc is written this way: ordinary fields that take
            // part in the disc's order evidence, but never shown as HIGH.
            candidate.capAtMedium = true;
        }
        candidate.bareHyphenSplit = false;
        candidate.unsplitField.clear();
        candidate.unsplitRawField.clear();
    }
    static const QRegularExpression bareHyphen(
        QStringLiteral(R"((?<=[\p{L}\)'])-(?=\p{L}))"),
        QRegularExpression::UseUnicodePropertiesOption);
    auto splitPoint = [](const QString& value) -> qsizetype {
        qsizetype found = -1;
        QRegularExpressionMatchIterator it = bareHyphen.globalMatch(value);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            if (found >= 0)
                return -1;
            found = match.capturedStart();
        }
        return found;
    };
    QHash<QString, int> shapedInGroup;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return;
        if (candidate.fields.size() == 1 && splitPoint(candidate.fields.first()) > 0)
            ++shapedInGroup[groupKey(candidate)];
    }
    for (Candidate& candidate : candidates) {
        if (stop())
            return;
        if (candidate.fields.size() != 1)
            continue;
        const QString field = candidate.fields.first();
        const qsizetype at = splitPoint(field);
        if (at <= 0)
            continue;
        const QString left = collapseWhitespace(field.left(at));
        const QString right = collapseWhitespace(field.mid(at + 1));
        // Codes and fragments ("sf-gold19", "Sfg2113-There's") are not names.
        static const QRegularExpression digit(QStringLiteral(R"(\d)"));
        if (left.size() <= 3 || right.size() <= 3 || left.contains(digit) || right.contains(digit))
            continue;
        const bool clearOnItsOwn = wordCount(left) >= 2 && wordCount(right) >= 2;
        if (!clearOnItsOwn && shapedInGroup.value(groupKey(candidate)) < 3)
            continue;
        const QString raw = candidate.rawFields.first();
        const qsizetype rawAt = splitPoint(raw);
        candidate.unsplitField = field;
        candidate.unsplitRawField = candidate.rawFields.first();
        candidate.fields = {left, right};
        candidate.rawFields = rawAt > 0
            ? QStringList{collapseWhitespace(raw.left(rawAt)), collapseWhitespace(raw.mid(rawAt + 1))}
            : QStringList{left, right};
        candidate.bareHyphenSplit = true;
    }
}

// Names split at underscores because their disc is written that way vote only
// among themselves: they may order each other, but never set the order for
// names whose fields were separated plainly.
QString voteGroupKey(const Candidate& candidate)
{
    return candidate.capAtMedium ? groupKey(candidate) + QStringLiteral("#underscore")
                                 : groupKey(candidate);
}

Evidence prepareEvidence(QList<Candidate>& candidates, StopCheck& stop)
{
    QHash<QString, QSet<QString>> partners;
    QHash<QString, QSet<QString>> canonicalPartners;
    QHash<QString, QHash<QString, QSet<QString>>> groupPartners;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return {};
        if (candidate.fields.size() != 2)
            continue;
        const QString a = fieldKey(candidate.fields.at(0));
        const QString b = fieldKey(candidate.fields.at(1));
        if (a.isEmpty() || b.isEmpty())
            continue;
        // A repaired name counts towards names recurring on its own disc
        // ("Everly Brothers" with three different titles), but it never adds
        // to wider recurrence or to order votes: those stay with names whose
        // fields were separated plainly.
        groupPartners[groupKey(candidate)][a].insert(b);
        groupPartners[groupKey(candidate)][b].insert(a);
        if (candidate.bareHyphenSplit || candidate.codeFieldsDropped || candidate.capAtMedium)
            continue;
        if (a == b)
            continue;
        partners[a].insert(b);
        partners[b].insert(a);
        const QString recurringA = recurrenceKey(candidate.fields.at(0));
        const QString recurringB = recurrenceKey(candidate.fields.at(1));
        if (recurringA != recurringB) {
            canonicalPartners[recurringA].insert(recurringB);
            canonicalPartners[recurringB].insert(recurringA);
        }
    }

    Evidence evidence;
    for (Candidate& candidate : candidates) {
        if (stop())
            return {};
        if (candidate.fields.size() != 2)
            continue;
        const QString a = candidate.fields.at(0);
        const QString b = candidate.fields.at(1);
        const QString artistKey = fieldKey(candidate.id3Artist);
        if (!isPlaceholderTagValue(candidate.id3Artist) && artistKey == fieldKey(a))
            candidate.tagOrder = Order::ArtistTitle;
        else if (!isPlaceholderTagValue(candidate.id3Artist) && artistKey == fieldKey(b))
            candidate.tagOrder = Order::TitleArtist;

        const bool aName = looksLikePersonalName(a);
        const bool bName = looksLikePersonalName(b);
        if (aName != bName) {
            candidate.structuralOrder = aName ? Order::ArtistTitle : Order::TitleArtist;
            candidate.structuralWeight = 3;
            candidate.structuralKind = StructuralKind::PersonalName;
        } else {
            const int aLocalPartners = int(groupPartners.value(groupKey(candidate))
                                               .value(fieldKey(a)).size());
            const int bLocalPartners = int(groupPartners.value(groupKey(candidate))
                                               .value(fieldKey(b)).size());
            if (aLocalPartners >= 2 && aLocalPartners > bLocalPartners) {
                candidate.structuralOrder = Order::ArtistTitle;
                candidate.structuralWeight = 2;
                candidate.structuralKind = StructuralKind::LocalRecurrence;
            } else if (bLocalPartners >= 2 && bLocalPartners > aLocalPartners) {
                candidate.structuralOrder = Order::TitleArtist;
                candidate.structuralWeight = 2;
                candidate.structuralKind = StructuralKind::LocalRecurrence;
            }
            const bool aEnsemble = looksLikeEnsemble(a);
            const bool bEnsemble = looksLikeEnsemble(b);
            if (candidate.structuralOrder == Order::Unknown && aEnsemble != bEnsemble) {
                candidate.structuralOrder = aEnsemble ? Order::ArtistTitle : Order::TitleArtist;
                candidate.structuralWeight = 2;
                candidate.structuralKind = StructuralKind::Ensemble;
            }
            const int aPartners = int(partners.value(fieldKey(a)).size());
            const int bPartners = int(partners.value(fieldKey(b)).size());
            if (candidate.structuralOrder == Order::Unknown
                && aPartners >= 2 && aPartners >= 2 * qMax(1, bPartners)) {
                candidate.structuralOrder = Order::ArtistTitle;
                candidate.structuralWeight = 1;
                candidate.structuralKind = StructuralKind::GlobalRecurrence;
            } else if (candidate.structuralOrder == Order::Unknown
                       && bPartners >= 2 && bPartners >= 2 * qMax(1, aPartners)) {
                candidate.structuralOrder = Order::TitleArtist;
                candidate.structuralWeight = 1;
                candidate.structuralKind = StructuralKind::GlobalRecurrence;
            }
        }

        if (candidate.tagOrder != Order::Unknown) {
            candidate.voteOrder = candidate.tagOrder;
            candidate.voteWeight = 4;
        } else {
            candidate.voteOrder = candidate.structuralOrder;
            candidate.voteWeight = candidate.structuralWeight;
        }
        if (candidate.bareHyphenSplit || candidate.codeFieldsDropped) {
            candidate.voteOrder = Order::Unknown;
            candidate.voteWeight = 0;
        }
        if (candidate.voteOrder != Order::Unknown) {
            const bool fromTag = candidate.tagOrder != Order::Unknown;
            addVote(evidence.groupVotes[voteGroupKey(candidate)], candidate.voteOrder,
                    candidate.voteWeight, fromTag);
            if (!candidate.capAtMedium)
                addVote(evidence.seriesVotes[seriesKey(candidate)], candidate.voteOrder,
                        candidate.voteWeight, fromTag);
        }
    }

    struct RecurrenceSupport {
        int artist = 0;
        int title = 0;
    };
    QHash<QString, RecurrenceSupport> support;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return {};
        if (candidate.fields.size() != 2 || candidate.bareHyphenSplit
            || candidate.codeFieldsDropped || candidate.capAtMedium)
            continue;
        const Decision preliminary = decide(candidate, evidence, false);
        if (preliminary.confidence != QLatin1String("high")
            && preliminary.confidence != QLatin1String("medium"))
            continue;
        const QString a = recurrenceKey(candidate.fields.at(0));
        const QString b = recurrenceKey(candidate.fields.at(1));
        if (preliminary.order == Order::ArtistTitle) {
            ++support[a].artist;
            ++support[b].title;
        } else {
            ++support[b].artist;
            ++support[a].title;
        }
    }
    auto stronglySupportedArtist = [&](const QString& key) {
        const RecurrenceSupport counts = support.value(key);
        return canonicalPartners.value(key).size() >= 3 && counts.artist >= 3
            && counts.artist >= 3 * qMax(1, counts.title);
    };
    for (Candidate& candidate : candidates) {
        if (stop())
            return {};
        if (candidate.fields.size() != 2
            || (candidate.structuralKind != StructuralKind::None
                && candidate.structuralKind != StructuralKind::GlobalRecurrence))
            continue;
        const bool aStrong = stronglySupportedArtist(recurrenceKey(candidate.fields.at(0)));
        const bool bStrong = stronglySupportedArtist(recurrenceKey(candidate.fields.at(1)));
        if (aStrong == bStrong)
            continue;
        candidate.structuralOrder = aStrong ? Order::ArtistTitle : Order::TitleArtist;
        candidate.structuralWeight = 1;
        candidate.structuralKind = StructuralKind::GlobalRecurrence;
        candidate.structuralStrong = true;
    }
    return evidence;
}

Decision decide(const Candidate& candidate, const Evidence& evidence, bool hideOwnTag)
{
    const Order ownOrder = !hideOwnTag && candidate.tagOrder != Order::Unknown
        ? candidate.tagOrder : candidate.structuralOrder;
    const bool ownIsTag = !hideOwnTag && candidate.tagOrder != Order::Unknown;
    const bool ownOverridesStrong = ownIsTag
        || candidate.structuralKind == StructuralKind::PersonalName
        || (candidate.structuralKind == StructuralKind::GlobalRecurrence
            && candidate.structuralStrong);
    const bool ownOverridesConsistent = ownOverridesStrong
        || candidate.structuralKind == StructuralKind::LocalRecurrence
        || (candidate.structuralKind == StructuralKind::GlobalRecurrence
            && candidate.structuralStrong);
    const VoteCounts group = withoutCandidate(evidence.groupVotes.value(voteGroupKey(candidate)),
                                               candidate);
    const VoteCounts series = candidate.capAtMedium
        ? evidence.seriesVotes.value(seriesKey(candidate))
        : withoutCandidate(evidence.seriesVotes.value(seriesKey(candidate)), candidate);
    // Tag-only disc votes count as strong only when they agree with the
    // label's known naming convention (e.g. Sunfly is artist - title).
    Order strongGroup = strongVote(group);
    if (strongGroup == Order::Unknown) {
        const Order tagOnly = strongVote(group, true);
        if (tagOnly != Order::Unknown && tagOnly == priorFor(candidate.discPrefix))
            strongGroup = tagOnly;
    }
    if (strongGroup != Order::Unknown) {
        if (ownOrder != Order::Unknown && ownOrder != strongGroup) {
            return {ownOverridesStrong ? ownOrder : strongGroup,
                    ownOverridesStrong ? QStringLiteral("filename_evidence")
                                       : QStringLiteral("filename_disc_rule"),
                    QStringLiteral("medium")};
        }
        return {strongGroup, QStringLiteral("filename_disc_rule"), QStringLiteral("high")};
    }
    if (ownOrder != Order::Unknown && ownOverridesConsistent)
        return {ownOrder, QStringLiteral("filename_evidence"), QStringLiteral("medium")};

    Order order = consistentVote(group);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_disc_rule"), QStringLiteral("medium")};
    if (ownOrder != Order::Unknown)
        return {ownOrder, QStringLiteral("filename_evidence"), QStringLiteral("medium")};
    order = strongVote(series);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_series_rule"), QStringLiteral("medium")};
    order = weakVote(group);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_disc_rule"), QStringLiteral("low")};
    order = consistentVote(series);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_series_rule"), QStringLiteral("low")};
    order = priorFor(candidate.discPrefix);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_series_prior"), QStringLiteral("low")};
    order = weakVote(series);
    if (order != Order::Unknown)
        return {order, QStringLiteral("filename_series_rule"), QStringLiteral("low")};
    return {Order::ArtistTitle, QStringLiteral("filename_default_order"), QStringLiteral("low")};
}

void addAccuracy(QVariantMap& counts, const QString& confidence, bool correct)
{
    QVariantMap row = counts.value(confidence).toMap();
    row.insert(QStringLiteral("total"), row.value(QStringLiteral("total")).toLongLong() + 1);
    if (correct)
        row.insert(QStringLiteral("correct"), row.value(QStringLiteral("correct")).toLongLong() + 1);
    counts.insert(confidence, row);
}

QVariantMap finaliseAccuracy(const QVariantMap& counts)
{
    QVariantMap result;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        QVariantMap row = it.value().toMap();
        const qlonglong total = row.value(QStringLiteral("total")).toLongLong();
        const qlonglong correct = row.value(QStringLiteral("correct")).toLongLong();
        row.insert(QStringLiteral("accuracyPercent"),
                   total == 0 ? 0.0 : 100.0 * double(correct) / double(total));
        result.insert(it.key(), row);
    }
    return result;
}

QString provenanceFor(const Decision& decision, bool combined)
{
    if (combined)
        return QStringLiteral("combined_evidence");
    if (decision.source == QLatin1String("id3_tags"))
        return QStringLiteral("id3");
    if (decision.source == QLatin1String("fallback"))
        return QStringLiteral("fallback");
    if (decision.source == QLatin1String("filename_disc_rule")
        || decision.source == QLatin1String("filename_series_rule")
        || decision.source == QLatin1String("filename_series_prior"))
        return QStringLiteral("disc_rule");
    return QStringLiteral("filename");
}

QString orderName(Order order)
{
    if (order == Order::ArtistTitle)
        return QStringLiteral("artist_title");
    if (order == Order::TitleArtist)
        return QStringLiteral("title_artist");
    return QStringLiteral("unknown");
}

// One song's automatic result. The base result comes from the filename, folder
// and tag rules; later evidence stages may replace it or record conflicts.
struct Resolved {
    const Candidate* candidate = nullptr;
    QString titleRaw;
    QString artistRaw;
    QString title;
    QString artist;
    Decision decision;
    QString provenance;
    QString baseConfidence;
    QJsonArray extraEvidence;
    QJsonArray conflicts;
    QStringList extraSearch;
};

QVariant nullable(const QString& value)
{
    return value.isEmpty() ? QVariant() : QVariant(value);
}

int confidenceRank(const QString& value)
{
    if (value == QLatin1String("high")) return 3;
    if (value == QLatin1String("medium")) return 2;
    if (value == QLatin1String("low")) return 1;
    return 0;
}

// ID3 values are unreliable in this collection: taggers copied disc codes,
// label names, file names or track numbers into them. A value is usable only
// when it is none of those.
bool usableTagValue(const QString& value, const Candidate& candidate)
{
    if (isPlaceholderTagValue(value))
        return false;
    const QString key = normalizeForSearch(value);
    static thread_local const QRegularExpression letter(
        QStringLiteral(R"(\p{L})"), QRegularExpression::UseUnicodePropertiesOption);
    if (key.isEmpty() || !key.contains(letter))
        return false;
    static const QRegularExpression discLike(QStringLiteral(R"(^[a-z]{1,8}\s?\d{2,}(?:\s?\d+)?$)"));
    if (discLike.match(key).hasMatch())
        return false;
    static const QSet<QString> labels = {
        QStringLiteral("sunfly"), QStringLiteral("karaoke"), QStringLiteral("zoom"),
        QStringLiteral("sound choice"), QStringLiteral("essential"), QStringLiteral("legends"),
        QStringLiteral("sweet georgia brown"), QStringLiteral("priddis"), QStringLiteral("dk"),
        QStringLiteral("unknown artist"), QStringLiteral("various"), QStringLiteral("various artists")};
    if (labels.contains(key))
        return false;
    return key != normalizeForSearch(candidate.discId)
        && key != normalizeForSearch(candidate.parsed.value(QStringLiteral("stem")).toString())
        && key != normalizeForSearch(candidate.fallbackFolder);
}

Resolved resolveBase(const Candidate& candidate, const Evidence& evidence)
{
    Resolved result;
    result.candidate = &candidate;
    if (candidate.fields.size() >= 2) {
        result.decision = decide(candidate, evidence, false);
        if (result.decision.order == Order::TitleArtist) {
            result.titleRaw = candidate.rawFields.at(0);
            result.artistRaw = candidate.rawFields.at(1);
            result.title = candidate.fields.at(0);
            result.artist = candidate.fields.at(1);
        } else {
            result.artistRaw = candidate.rawFields.at(0);
            result.titleRaw = candidate.rawFields.at(1);
            result.artist = candidate.fields.at(0);
            result.title = candidate.fields.at(1);
        }
    } else if (candidate.fields.size() == 1) {
        result.titleRaw = candidate.rawFields.first();
        result.title = candidate.fields.first();
        if (!isPlaceholderTagValue(candidate.id3Artist)) {
            result.artistRaw = candidate.id3Artist;
            result.artist = cleanedField(candidate.id3Artist);
        }
        result.decision.source = QStringLiteral("filename_single_field");
        result.decision.confidence = QStringLiteral("low");
    }
    if (result.titleRaw.isEmpty() && usableTagValue(candidate.id3Title, candidate)
        && usableTagValue(candidate.id3Artist, candidate)
        && fieldKey(candidate.id3Title) != fieldKey(candidate.id3Artist)) {
        // The filename names nothing; clean tags are a weak suggestion only.
        result.titleRaw = candidate.id3Title;
        result.artistRaw = candidate.id3Artist;
        result.title = cleanedField(candidate.id3Title);
        result.artist = cleanedField(candidate.id3Artist);
        result.decision.source = QStringLiteral("id3_tags");
        result.decision.confidence = QStringLiteral("low");
    }
    if (result.titleRaw.isEmpty()) {
        ParsedName parsed;
        parsed.discId = candidate.discId;
        parsed.track = candidate.track;
        parsed.fallbackFolder = candidate.fallbackFolder;
        parsed.cleaned = QFileInfo(candidate.rawPath).completeBaseName();
        result.titleRaw = fallbackTitle(parsed);
        result.title = result.titleRaw;
        result.decision.source = QStringLiteral("fallback");
        result.decision.confidence = QStringLiteral("unresolved");
    }
    result.title = collapseWhitespace(result.title);
    result.artist = collapseWhitespace(result.artist);
    const bool combined = candidate.fields.size() == 1
        && !isPlaceholderTagValue(candidate.id3Artist);
    result.provenance = provenanceFor(result.decision, combined);
    if (candidate.capAtMedium && result.decision.confidence == QLatin1String("high"))
        result.decision.confidence = QStringLiteral("medium");
    if (candidate.bareHyphenSplit || candidate.codeFieldsDropped) {
        // A repaired name is shown split only when its order is well supported:
        // by the rest of its disc or folder, a "Last, First" name, or a name
        // that recurs locally. Otherwise the order is a guess.
        // Only a strong disc/folder vote counts: a mixed folder such as
        // "Added Songs" gives weak, near-even votes.
        const bool wellOrdered = (result.decision.source == QLatin1String("filename_disc_rule")
                                  && result.decision.confidence == QLatin1String("high"))
            || candidate.structuralKind == StructuralKind::PersonalName
            || candidate.structuralKind == StructuralKind::LocalRecurrence;
        if (!wellOrdered && candidate.bareHyphenSplit) {
            // Keep the whole name as a weak title, exactly as before the split.
            result.titleRaw = candidate.unsplitRawField;
            result.title = collapseWhitespace(candidate.unsplitField);
            result.artistRaw.clear();
            result.artist.clear();
            result.decision.order = Order::Unknown;
            result.decision.source = QStringLiteral("filename_single_field");
            result.decision.confidence = QStringLiteral("low");
            result.provenance = QStringLiteral("filename");
        } else if (!wellOrdered) {
            result.decision.confidence = QStringLiteral("low");
        } else if (result.decision.confidence == QLatin1String("high")) {
            result.decision.confidence = QStringLiteral("medium");
        }
    }
    if (!candidate.sidecarPath.isEmpty()) {
        QJsonObject item;
        item.insert(QStringLiteral("source"), QStringLiteral("sidecar_track_list"));
        item.insert(QStringLiteral("path"), candidate.sidecarPath);
        item.insert(QStringLiteral("line"), candidate.sidecarLine);
        item.insert(QStringLiteral("fields"), QJsonArray::fromStringList(candidate.sidecarFields));
        result.extraEvidence.append(item);
    }
    if (candidate.fromSidecar) {
        result.provenance = QStringLiteral("sidecar_track_list");
        // A list naming exactly this disc, with the disc's order evidence
        // strongly agreeing, is shown with confidence; otherwise at most medium.
        if (result.decision.confidence == QLatin1String("high") && !candidate.sidecarDiscExact)
            result.decision.confidence = QStringLiteral("medium");
    } else if (!candidate.sidecarPath.isEmpty() && candidate.fields.size() == 2
               && unorderedKey(candidate.fields) != unorderedKey(candidate.sidecarFields)) {
        result.conflicts.append(QStringLiteral("sidecar_disagrees"));
        if (result.decision.confidence == QLatin1String("high"))
            result.decision.confidence = QStringLiteral("medium");
    }
    result.baseConfidence = result.decision.confidence;
    // Tags are supporting evidence only and are often wrong, so a tag that
    // merely differs is recorded but is not a conflict. It is a conflict when
    // the tag identifies the opposite field as the artist of a two-field name.
    if (candidate.tagOrder != Order::Unknown && result.decision.order != Order::Unknown
        && candidate.tagOrder != result.decision.order)
        result.conflicts.append(QStringLiteral("id3_order"));
    return result;
}

QString evidenceJson(const Resolved& resolved)
{
    const Candidate& candidate = *resolved.candidate;
    QJsonObject result;
    result.insert(QStringLiteral("v"), MetadataResolver::Version);
    result.insert(QStringLiteral("rule"), resolved.decision.source);
    result.insert(QStringLiteral("order"), orderName(resolved.decision.order));
    QJsonArray fields;
    for (const QString& field : candidate.rawFields)
        fields.append(field);
    result.insert(QStringLiteral("fields"), fields);
    QJsonObject id3;
    id3.insert(QStringLiteral("artist"), candidate.id3Artist);
    id3.insert(QStringLiteral("title"), candidate.id3Title);
    result.insert(QStringLiteral("id3"), id3);
    result.insert(QStringLiteral("baseConfidence"), resolved.baseConfidence);
    QJsonArray evidence;
    QJsonObject automatic;
    automatic.insert(QStringLiteral("source"), resolved.provenance);
    automatic.insert(QStringLiteral("artist"), resolved.artist);
    automatic.insert(QStringLiteral("title"), resolved.title);
    automatic.insert(QStringLiteral("confidence"), resolved.decision.confidence);
    evidence.append(automatic);
    for (const QJsonValue& value : resolved.extraEvidence)
        evidence.append(value);
    result.insert(QStringLiteral("evidence"), evidence);
    result.insert(QStringLiteral("conflicts"), resolved.conflicts);
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

// Minimum drawing instructions for a CDG to prove identity: a blank or nearly
// empty stream is shared by unrelated songs and proves nothing.
constexpr qint64 kMinimumIdentityPackets = 1000;

QString identityKey(const QString& title, const QString& artist)
{
    return normalizeForSearch(title) + QChar(0x1f)
        + normalizeForSearch(displayArtist(artist));
}

// True when at least 80% of the words of `title` occur in `text`.
bool containsTitleWords(const QString& text, const QString& title)
{
    const QStringList words = normalizeForSearch(title).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return false;
    const QStringList available = normalizeForSearch(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QSet<QString> have(available.cbegin(), available.cend());
    int found = 0;
    for (const QString& word : words)
        found += have.contains(word) ? 1 : 0;
    return found * 5 >= int(words.size()) * 4;
}

// Names an unresolved or weakly named song from a copy elsewhere in the
// collection whose complete CDG is byte-identical (the MP3 audio is compared
// as well). File sizes only nominated the pair for hashing; they prove nothing.
bool applyDuplicateEvidence(QSqlDatabase database, qint64 rootId, QList<Resolved>& results,
                            StopCheck& stop, QString* error)
{
    QHash<qint64, int> bySong;
    for (int i = 0; i < results.size(); ++i) {
        if (stop())
            return true;
        bySong.insert(results.at(i).candidate->songId, i);
    }
    QString sql = QStringLiteral(
        "SELECT ts.song_id,ds.song_id,"
        "CASE WHEN tm.content_sha256 IS NOT NULL AND tm.content_sha256=dm.content_sha256 "
        "THEN 1 ELSE 0 END,dm.rel_path "
        "FROM sources ts JOIN files tg ON tg.id=ts.graphics_file_id "
        "JOIN files tm ON tm.id=ts.mp3_file_id "
        "JOIN files dg ON dg.content_sha256=tg.content_sha256 AND dg.kind='cdg' "
        "AND dg.id<>tg.id AND dg.present=1 "
        "JOIN sources ds ON ds.graphics_file_id=dg.id AND ds.kind='loose_cdg' "
        "JOIN files dm ON dm.id=ds.mp3_file_id "
        "WHERE ts.kind='loose_cdg' AND tg.kind='cdg' AND tg.present=1 "
        "AND tg.content_sha256 IS NOT NULL AND tg.cdg_packets>=? "
        "AND ts.song_id<>ds.song_id");
    if (rootId >= 0)
        sql += QStringLiteral(" AND ts.root_id=? AND ds.root_id=?");
    QSqlQuery query(database);
    query.prepare(sql);
    query.addBindValue(kMinimumIdentityPackets);
    if (rootId >= 0) {
        query.addBindValue(rootId);
        query.addBindValue(rootId);
    }
    if (!query.exec()) {
        if (error)
            *error = databaseError(query, QStringLiteral("Could not load content matches"));
        return false;
    }
    struct Donor { int index; bool audioEqual; QString path; };
    QHash<int, QList<Donor>> donors;
    while (query.next()) {
        if (stop())
            return true;
        const auto target = bySong.constFind(query.value(0).toLongLong());
        const auto donor = bySong.constFind(query.value(1).toLongLong());
        if (target == bySong.cend() || donor == bySong.cend())
            continue;
        const Resolved& targetResult = results.at(*target);
        const Resolved& donorResult = results.at(*donor);
        // Only weakly named songs are named from a copy, and only by copies
        // named well by their own evidence (never by another copy).
        if (confidenceRank(targetResult.baseConfidence) > 1
            || confidenceRank(donorResult.baseConfidence) < 2)
            continue;
        donors[*target].append({*donor, query.value(2).toBool(), query.value(3).toString()});
    }
    for (auto it = donors.cbegin(); it != donors.cend(); ++it) {
        if (stop())
            return true;
        Resolved& target = results[it.key()];
        // Every copy must name the same song (title); the singer is taken
        // from the best-named copies, which must agree with each other.
        QSet<QString> titles;
        int bestRank = 0;
        for (const Donor& donor : it.value()) {
            if (stop())
                return true;
            bestRank = qMax(bestRank, confidenceRank(results.at(donor.index).baseConfidence));
        }
        QSet<QString> keys;
        const Resolved* best = nullptr;
        bool audioEqual = false;
        QJsonArray donorList;
        for (const Donor& donor : it.value()) {
            if (stop())
                return true;
            const Resolved& named = results.at(donor.index);
            titles.insert(normalizeForSearch(named.title));
            if (confidenceRank(named.baseConfidence) == bestRank) {
                keys.insert(identityKey(named.title, named.artist));
                if (!best)
                    best = &named;
            }
            audioEqual = audioEqual || donor.audioEqual;
            QJsonObject item;
            item.insert(QStringLiteral("path"), donor.path);
            item.insert(QStringLiteral("title"), named.title);
            item.insert(QStringLiteral("artist"), named.artist);
            item.insert(QStringLiteral("confidence"), named.baseConfidence);
            item.insert(QStringLiteral("audioEqual"), donor.audioEqual);
            donorList.append(item);
        }
        const Resolved& first = *best;
        QString confidence = audioEqual
            ? (bestRank == 3 ? QStringLiteral("high") : QStringLiteral("medium"))
            : (bestRank == 3 ? QStringLiteral("medium") : QStringLiteral("low"));
        QJsonObject item;
        item.insert(QStringLiteral("source"), QStringLiteral("exact_duplicate"));
        item.insert(QStringLiteral("title"), first.title);
        item.insert(QStringLiteral("artist"), first.artist);
        item.insert(QStringLiteral("confidence"), confidence);
        item.insert(QStringLiteral("donors"), donorList);
        target.extraEvidence.append(item);
        if (titles.size() > 1 || keys.size() > 1) {
            target.conflicts.append(QStringLiteral("duplicate_donors_disagree"));
            continue;
        }
        // A weak name that disagrees with a proven copy is kept, and the
        // disagreement is flagged for review rather than silently chosen. It
        // agrees when it already contains the copy's title ("u2-all because of
        // you" for U2 - All Because Of You).
        if (target.baseConfidence == QLatin1String("low")
            && !containsTitleWords(target.title + QLatin1Char(' ') + target.artist, first.title)) {
            target.conflicts.append(QStringLiteral("duplicate_title_differs"));
            continue;
        }
        target.titleRaw = first.titleRaw;
        target.artistRaw = first.artistRaw;
        target.title = first.title;
        target.artist = first.artist;
        target.decision.source = QStringLiteral("exact_duplicate");
        target.decision.confidence = confidence;
        target.provenance = QStringLiteral("exact_duplicate");
    }
    return true;
}

bool setMeta(QSqlDatabase database, const QString& key, const QString& value,
             QString* error)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO catalogue_meta(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (query.exec())
        return true;
    if (error)
        *error = databaseError(query, QStringLiteral("Could not update catalogue metadata"));
    return false;
}


// Titles and artists already named well in this collection. A title-screen
// reading is trusted only when it is (or is one unambiguous letter or two
// away from) a title that exists here, which rejects OCR garbage and logos.
struct NameDictionary {
    QHash<QString, QString> titles;            // key -> most common display form
    QHash<QString, QString> artists;           // key -> display form
    QHash<QString, QHash<QString, int>> titleForms;
    QHash<QString, QList<QString>> titlesByFirst;
    QHash<QString, QPair<QString, QString>> byDiscTrack;  // disc:track -> key, artist
};

QString nameKey(const QString& value)
{
    return ocrKey(value);
}

NameDictionary buildDictionary(const QList<Resolved>& results, StopCheck& stop)
{
    NameDictionary dictionary;
    QHash<QString, QHash<QString, int>> artistForms;
    for (const Resolved& result : results) {
        if (stop())
            return {};
        if (confidenceRank(result.decision.confidence) < 2
            || result.provenance == QLatin1String("cdg_ocr")
            || result.provenance == QLatin1String("fallback"))
            continue;
        const QString key = nameKey(result.title);
        if (key.size() < 2)
            continue;
        ++dictionary.titleForms[key][result.title];
        if (!result.artist.isEmpty())
            ++artistForms[nameKey(displayArtist(result.artist))][displayArtist(result.artist)];
        const Candidate& candidate = *result.candidate;
        if (!candidate.discId.isEmpty() && candidate.track > 0) {
            const QString slot = candidate.discId.toCaseFolded() + QLatin1Char(':')
                + QString::number(candidate.track);
            const auto existing = dictionary.byDiscTrack.constFind(slot);
            // Copies that disagree about a disc/track (a disc ID reused by
            // another label) make that slot useless as corroboration.
            if (existing == dictionary.byDiscTrack.cend())
                dictionary.byDiscTrack.insert(slot, {key, result.artist});
            else if (existing->first != key
                     || nameKey(displayArtist(existing->second)) != nameKey(displayArtist(result.artist)))
                dictionary.byDiscTrack.insert(slot, {QString(), QString()});
        }
    }
    auto mostCommon = [](const QHash<QString, int>& forms) {
        QString best;
        int count = -1;
        for (auto it = forms.cbegin(); it != forms.cend(); ++it) {
            if (it.value() > count || (it.value() == count && it.key() < best)) {
                best = it.key();
                count = it.value();
            }
        }
        return best;
    };
    for (auto it = dictionary.titleForms.cbegin(); it != dictionary.titleForms.cend(); ++it) {
        if (stop())
            return {};
        dictionary.titles.insert(it.key(), mostCommon(it.value()));
        dictionary.titlesByFirst[it.key().left(1)].append(it.key());
    }
    for (auto it = artistForms.cbegin(); it != artistForms.cend(); ++it) {
        if (stop())
            return {};
        dictionary.artists.insert(it.key(), mostCommon(it.value()));
    }
    return dictionary;
}

int boundedDistance(const QString& a, const QString& b, int limit)
{
    if (std::abs(int(a.size()) - int(b.size())) > limit)
        return limit + 1;
    QList<int> previous(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        previous[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        QList<int> current(b.size() + 1, limit + 1);
        current[0] = i;
        int best = current[0];
        for (int j = std::max(1, i - limit); j <= std::min(int(b.size()), i + limit); ++j) {
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

// The dictionary key for an OCR title: exact, or the single closest known
// title within one edit (two for long titles). Ties are rejected.
QString matchKnownTitle(const NameDictionary& dictionary, const QString& key, bool* corrected)
{
    *corrected = false;
    if (dictionary.titles.contains(key))
        return key;
    if (key.size() < 6)
        return {};
    const int limit = key.size() >= 12 ? 2 : 1;
    QString best;
    int bestDistance = limit + 1;
    bool tie = false;
    for (const QString& known : dictionary.titlesByFirst.value(key.left(1))) {
        const int distance = boundedDistance(key, known, limit);
        if (distance < bestDistance) {
            best = known;
            bestDistance = distance;
            tie = false;
        } else if (distance == bestDistance && distance <= limit) {
            tie = true;
        }
    }
    if (best.isEmpty() || tie || bestDistance > limit)
        return {};
    *corrected = true;
    return best;
}

bool applyTitleScreenEvidence(QSqlDatabase database, qint64 rootId, QList<Resolved>& results,
                              StopCheck& stop, QString* error)
{
    QHash<qint64, int> bySong;
    for (int i = 0; i < results.size(); ++i) {
        if (stop())
            return true;
        bySong.insert(results.at(i).candidate->songId, i);
    }
    QString sql = QStringLiteral(
        "SELECT s.song_id,t.frames_json,t.engine FROM sources s "
        "JOIN files g ON g.id=s.graphics_file_id "
        "JOIN enrich.title_screens t ON t.cdg_quick_sha256=g.quick_sha256 AND t.cdg_size=g.size "
        "WHERE s.kind='loose_cdg' AND g.present=1 AND t.status='ok'");
    if (rootId >= 0)
        sql += QStringLiteral(" AND s.root_id=?");
    QSqlQuery query(database);
    query.prepare(sql);
    if (rootId >= 0)
        query.addBindValue(rootId);
    if (!query.exec()) {
        if (error)
            *error = databaseError(query, QStringLiteral("Could not load title-screen text"));
        return false;
    }
    struct Screen { int index; QJsonArray frames; QString engine; };
    QList<Screen> screens;
    while (query.next()) {
        if (stop())
            return true;
        const auto found = bySong.constFind(query.value(0).toLongLong());
        if (found == bySong.cend())
            continue;
        if (results.at(*found).decision.confidence != QLatin1String("unresolved"))
            continue;
        screens.append({*found, QJsonDocument::fromJson(query.value(1).toByteArray()).array(),
                        query.value(2).toString()});
    }
    if (screens.isEmpty())
        return true;
    const NameDictionary dictionary = buildDictionary(results, stop);
    for (const Screen& screen : std::as_const(screens)) {
        if (stop())
            return true;
        Resolved& target = results[screen.index];
        TitleScreenReading reading;
        QString titleKey;
        bool corrected = false;
        int frameMs = -1;
        QString firstTitle;
        for (const QJsonValue& value : screen.frames) {
            if (stop())
                return true;
            const QJsonObject frame = value.toObject();
            QList<OcrLine> lines;
            for (const QJsonValue& lineValue : frame.value(QStringLiteral("lines")).toArray()) {
                const QJsonObject item = lineValue.toObject();
                const QJsonArray box = item.value(QStringLiteral("box")).toArray();
                OcrLine line;
                line.text = item.value(QStringLiteral("text")).toString();
                line.confidence = item.value(QStringLiteral("confidence")).toDouble();
                line.x = box.at(0).toDouble();
                line.y = box.at(1).toDouble();
                line.width = box.at(2).toDouble();
                line.height = box.at(3).toDouble();
                lines.append(line);
            }
            const TitleScreenReading candidate = readTitleScreen(lines);
            if (candidate.title.isEmpty())
                continue;
            if (firstTitle.isEmpty())
                firstTitle = candidate.title;
            const QString key = matchKnownTitle(dictionary, nameKey(candidate.title), &corrected);
            if (!key.isEmpty()) {
                reading = candidate;
                titleKey = key;
                frameMs = frame.value(QStringLiteral("timeMs")).toInt();
                break;
            }
        }
        QJsonObject item;
        item.insert(QStringLiteral("source"), QStringLiteral("cdg_ocr"));
        item.insert(QStringLiteral("engine"), screen.engine);
        if (titleKey.isEmpty()) {
            // Unconfirmed text is never shown, but it stays searchable.
            if (firstTitle.isEmpty())
                continue;
            item.insert(QStringLiteral("text"), firstTitle);
            item.insert(QStringLiteral("validated"), false);
            target.extraEvidence.append(item);
            target.extraSearch.append(firstTitle);
            continue;
        }
        QString title = dictionary.titles.value(titleKey);
        QString artist;
        QString confidence = QStringLiteral("medium");
        QString provenance = QStringLiteral("cdg_ocr");
        if (!reading.artist.isEmpty()) {
            const QString artistKey = nameKey(reading.artist);
            if (dictionary.artists.contains(artistKey))
                artist = dictionary.artists.value(artistKey);
        }
        // The same disc and track named elsewhere with this very title: two
        // independent sources agree, and that copy supplies the artist.
        const Candidate& candidate = *target.candidate;
        if (!candidate.discId.isEmpty() && candidate.track > 0) {
            const auto sibling = dictionary.byDiscTrack.constFind(
                candidate.discId.toCaseFolded() + QLatin1Char(':') + QString::number(candidate.track));
            if (sibling != dictionary.byDiscTrack.cend() && sibling->first == titleKey
                && !sibling->second.isEmpty()) {
                artist = sibling->second;
                confidence = QStringLiteral("high");
                provenance = QStringLiteral("combined_evidence");
            }
        }
        item.insert(QStringLiteral("text"), reading.title);
        item.insert(QStringLiteral("title"), title);
        item.insert(QStringLiteral("artist"), artist);
        item.insert(QStringLiteral("cueArtist"), reading.artist);
        item.insert(QStringLiteral("validated"), true);
        item.insert(QStringLiteral("corrected"), corrected);
        item.insert(QStringLiteral("frameMs"), frameMs);
        item.insert(QStringLiteral("confidence"), confidence);
        target.extraEvidence.append(item);
        target.extraSearch.append(reading.title);
        target.titleRaw = reading.title;
        target.artistRaw = reading.artist;
        target.title = title;
        target.artist = artist;
        target.decision.source = QStringLiteral("cdg_ocr");
        target.decision.confidence = confidence;
        target.provenance = provenance;
    }
    return true;
}
} // namespace

bool MetadataResolver::resolve(Catalogue& catalogue, qint64 rootId, QString* error)
{
    return resolve(catalogue, rootId, Options{}, error) == Status::Completed;
}

MetadataResolver::Status MetadataResolver::resolve(Catalogue& catalogue, qint64 rootId,
                                                   const Options& options,
                                                   QString* error)
{
    QSqlDatabase database = catalogue.database();
    if (!database.isOpen()) {
        if (error)
            *error = QStringLiteral("Catalogue is not open");
        return Status::Failed;
    }
    if (!setMeta(database, QStringLiteral("reprocess_pending"), QStringLiteral("1"), error))
        return Status::Failed;

    // Everything up to the writes happens in memory and is checked for a stop
    // request item by item. A stop discards it all: nothing is written, and
    // reprocess_pending (set above) stays on so the work resumes next time.
    StopCheck stop(options.cancelled);
    auto stage = [&options](const char* name) {
        if (options.stageStarted)
            options.stageStarted(QString::fromLatin1(name));
    };
    stage("candidates");
    QList<Candidate> candidates;
    if (!loadCandidates(database, rootId, &candidates, stop, error))
        return Status::Failed;
    if (stop.stopped())
        return Status::Cancelled;
    stage("sidecars");
    if (!applySidecars(database, rootId, candidates, stop, error))
        return Status::Failed;
    if (stop.stopped())
        return Status::Cancelled;
    stage("names");
    dropCodeFields(candidates, stop);
    splitBareHyphenFields(candidates, stop);
    stage("evidence");
    const Evidence evidence = prepareEvidence(candidates, stop);
    if (stop.stopped())
        return Status::Cancelled;
    stage("base");
    QList<Resolved> results;
    results.reserve(candidates.size());
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (stop())
            return Status::Cancelled;
        results.append(resolveBase(candidate, evidence));
    }
    stage("duplicates");
    if (!applyDuplicateEvidence(database, rootId, results, stop, error))
        return Status::Failed;
    if (stop.stopped())
        return Status::Cancelled;
    stage("title_screens");
    if (!applyTitleScreenEvidence(database, rootId, results, stop, error))
        return Status::Failed;
    if (stop.stopped())
        return Status::Cancelled;
    stage("write");
    if (options.progress)
        options.progress(0, results.size());

    // Automatic values are always stored; the shown (effective) values take a
    // trusted manual or imported value wherever one is set, so reprocessing
    // can never overwrite what a person entered.
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE songs SET auto_title=:autoTitle,auto_artist=:autoArtist,auto_source=:autoSource,"
        "auto_confidence=:autoConfidence,evidence_json=:evidence,conflict=:conflict,"
        "resolver_version=:version,base_confidence=:base,"
        "auto_label=:autoLabel,auto_series=:autoSeries,auto_label_source=:autoLabelSource,"
        "auto_disc_id=:autoDisc,auto_track=:autoTrack,"
        "label=COALESCE(manual_label,:label),series=COALESCE(manual_series,:series),"
        "label_source=CASE WHEN manual_label IS NOT NULL THEN COALESCE(manual_origin,'manual') "
        "ELSE :labelSource END,"
        "title_raw=:titleRaw,artist_raw=:artistRaw,disc_id=COALESCE(manual_disc_id,:disc),"
        "disc_prefix=:prefix,track=COALESCE(manual_track,:track),"
        "title=COALESCE(manual_title,:title),artist=COALESCE(manual_artist,:artist),"
        "display_title=COALESCE(manual_title,:displayTitle),"
        "display_artist=COALESCE(manual_artist,:displayArtist),search_text=:search,"
        "metadata_source=CASE WHEN ") + MetadataResolver::hasTrustedNameSql()
        + QStringLiteral(" THEN COALESCE(manual_origin,'manual') ELSE :source END,"
        "confidence=CASE WHEN manual_title IS NOT NULL THEN 'high' ELSE :confidence END,"
        "best_source_id=(SELECT id FROM sources WHERE song_id=:song1 "
        "ORDER BY playable DESC,CASE kind WHEN 'loose_cdg' THEN 0 WHEN 'loose_mcg' THEN 1 ELSE 2 END,id LIMIT 1),"
        "playable=EXISTS(SELECT 1 FROM sources WHERE song_id=:song2 AND playable=1) WHERE id=:song3"));
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin metadata-resolution batch: %1").arg(database.lastError().text());
        return Status::Failed;
    }
    qint64 resolved = 0;
    for (const Resolved& result : std::as_const(results)) {
        // A stop between commits drops the open batch whole: every batch is
        // either written completely or not at all.
        if (stop()) {
            database.rollback();
            return Status::Cancelled;
        }
        const Candidate& candidate = *result.candidate;
        const KaraokeLabel label = identifyKaraokeLabel(candidate.discPrefix, candidate.relDir);
        const QString& title = result.title;
        const QString& artist = result.artist;
        const QString shownArtist = displayArtist(artist);
        const QString evidenceText = evidenceJson(result);
        const bool conflict = !result.conflicts.isEmpty();
        const QString effectiveTitle = candidate.manualTitle.isNull()
            ? title : candidate.manualTitle.toString();
        const QString effectiveArtist = candidate.manualArtist.isNull()
            ? artist : candidate.manualArtist.toString();
        MetadataResolver::SearchInputs search;
        search.effectiveTitle = effectiveTitle;
        search.effectiveArtist = effectiveArtist;
        search.autoTitle = title;
        search.autoArtist = artist;
        search.manualTitle = candidate.manualTitle;
        search.manualArtist = candidate.manualArtist;
        search.discId = candidate.discId;
        search.track = candidate.track;
        search.relDir = candidate.relDir;
        search.parsed = candidate.parsed;
        search.tags = candidate.rawTags;
        search.extra = result.extraSearch;
        search.extra << label.label << label.series << candidate.trustedSearch;
        const QString searchText = MetadataResolver::buildSearchText(search);

        const QList<QPair<const char*, QVariant>> values = {
            {":autoTitle", title}, {":autoArtist", artist}, {":autoSource", result.provenance},
            {":autoConfidence", result.decision.confidence}, {":evidence", evidenceText},
            {":conflict", conflict}, {":version", Version}, {":base", result.baseConfidence},
            {":autoLabel", nullable(label.label)}, {":autoSeries", nullable(label.series)},
            {":autoLabelSource", nullable(label.source)}, {":autoDisc", candidate.discId},
            {":autoTrack", candidate.track}, {":label", nullable(label.label)},
            {":series", nullable(label.series)}, {":labelSource", nullable(label.source)},
            {":titleRaw", result.titleRaw}, {":artistRaw", result.artistRaw},
            {":disc", candidate.discId}, {":prefix", candidate.discPrefix},
            {":track", candidate.track}, {":title", title}, {":artist", artist},
            {":displayTitle", title}, {":displayArtist", shownArtist}, {":search", searchText},
            {":source", result.provenance}, {":confidence", result.decision.confidence},
            {":song1", candidate.songId}, {":song2", candidate.songId}, {":song3", candidate.songId}};
        for (const auto& value : values)
            update.bindValue(QLatin1String(value.first), value.second);
        if (!update.exec()) {
            database.rollback();
            if (error)
                *error = databaseError(update, QStringLiteral("Could not store resolved metadata"));
            return Status::Failed;
        }
        ++resolved;
        if ((resolved % 500) == 0 || (options.yieldRequested && options.yieldRequested())) {
            if (!database.commit()) {
                if (error)
                    *error = QStringLiteral("Could not commit metadata-resolution batch: %1")
                                 .arg(database.lastError().text());
                return Status::Failed;
            }
            if (options.progress)
                options.progress(resolved, results.size());
            if (options.shouldStop && options.shouldStop())
                return Status::Cancelled;
            if (!database.transaction()) {
                if (error)
                    *error = QStringLiteral("Could not begin metadata-resolution batch: %1")
                                 .arg(database.lastError().text());
                return Status::Failed;
            }
        }
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit metadata resolution: %1").arg(database.lastError().text());
        return Status::Failed;
    }
    if (options.progress)
        options.progress(resolved, results.size());
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin resolver completion transaction: %1")
                         .arg(database.lastError().text());
        return Status::Failed;
    }
    if (!setMeta(database, QStringLiteral("resolver_version"),
                 QString::number(Version), error)
        || !setMeta(database, QStringLiteral("reprocess_pending"),
                    QStringLiteral("0"), error)
        || !database.commit()) {
        database.rollback();
        if (error && error->isEmpty())
            *error = QStringLiteral("Could not commit resolver completion: %1")
                         .arg(database.lastError().text());
        return Status::Failed;
    }
    return Status::Completed;
}

QString MetadataResolver::hasTrustedSql(const QString& alias)
{
    const QString p = alias.isEmpty() ? QString() : alias + QLatin1Char('.');
    return QStringLiteral("(%1manual_title IS NOT NULL OR %1manual_artist IS NOT NULL OR "
                          "%1manual_label IS NOT NULL OR %1manual_series IS NOT NULL OR "
                          "%1manual_disc_id IS NOT NULL OR %1manual_track IS NOT NULL)").arg(p);
}

QString MetadataResolver::hasTrustedNameSql(const QString& alias)
{
    const QString p = alias.isEmpty() ? QString() : alias + QLatin1Char('.');
    return QStringLiteral("(%1manual_title IS NOT NULL OR %1manual_artist IS NOT NULL)").arg(p);
}

QString MetadataResolver::displayArtistName(const QString& artist)
{
    return displayArtist(artist);
}

QString MetadataResolver::buildSearchText(const SearchInputs& inputs)
{
    QStringList parts = {inputs.effectiveTitle, inputs.effectiveArtist,
                         displayArtist(inputs.effectiveArtist), inputs.autoTitle,
                         inputs.autoArtist, displayArtist(inputs.autoArtist),
                         inputs.discId, inputs.relDir};
    if (!inputs.manualTitle.isNull())
        parts.append(inputs.manualTitle.toString());
    if (!inputs.manualArtist.isNull())
        parts.append(inputs.manualArtist.toString());
    parts.append(inputs.extra);
    for (const QJsonValue& field : inputs.parsed.value(QStringLiteral("fields")).toArray())
        parts.append(field.toString());
    parts.append(inputs.parsed.value(QStringLiteral("stem")).toString());
    const QString id3Artist = inputs.tags.value(QStringLiteral("artist")).toString();
    const QString id3Title = inputs.tags.value(QStringLiteral("title")).toString();
    if (!isPlaceholderTagValue(id3Artist))
        parts.append(id3Artist);
    if (!isPlaceholderTagValue(id3Title))
        parts.append(id3Title);
    // Other raw tag values are searchable too; they never drive the display.
    for (const char* key : {"album", "albumArtist", "track"})
        parts.append(inputs.tags.value(QLatin1String(key)).toString());
    if (inputs.track > 0) {
        const QString padded = QStringLiteral("%1").arg(inputs.track, 2, 10, QLatin1Char('0'));
        parts.append(QString::number(inputs.track));
        parts.append(padded);
        if (!inputs.discId.isEmpty())
            parts.append(inputs.discId + padded);
    }
    // normalizeForSearch turns '_' and other punctuation into spaces, so the raw
    // stem "Frank_Sinatra-My_Way" stays searchable as words.
    return normalizeForSearch(parts.join(QLatin1Char(' ')));
}

QVariantMap MetadataResolver::evaluateTags(Catalogue& catalogue, int mismatchLimit,
                                           QString* error)
{
    QVariantMap result;
    QSqlDatabase database = catalogue.database();
    StopCheck never;
    QList<Candidate> candidates;
    if (!loadCandidates(database, -1, &candidates, never, error))
        return result;
    const Evidence evidence = prepareEvidence(candidates, never);
    QVariantMap counts;
    QVariantList mismatches;
    qlonglong eligible = 0;
    qlonglong mismatchCount = 0;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (candidate.fields.size() != 2
            || isPlaceholderTagValue(candidate.id3Artist)
            || isPlaceholderTagValue(candidate.id3Title))
            continue;
        const QString artistKey = fieldKey(candidate.id3Artist);
        const QString titleKey = fieldKey(candidate.id3Title);
        int artistIndex = -1;
        int titleIndex = -1;
        for (int i = 0; i < 2; ++i) {
            if (fieldKey(candidate.fields.at(i)) == artistKey)
                artistIndex = i;
            if (fieldKey(candidate.fields.at(i)) == titleKey)
                titleIndex = i;
        }
        if (artistIndex < 0 || titleIndex < 0 || artistIndex == titleIndex)
            continue;
        ++eligible;
        const Decision decision = decide(candidate, evidence, true);
        const int chosenIndex = decision.order == Order::TitleArtist ? 1 : 0;
        const bool correct = chosenIndex == artistIndex;
        addAccuracy(counts, decision.confidence, correct);
        if (!correct) {
            ++mismatchCount;
            if (mismatches.size() < mismatchLimit) {
                QVariantMap mismatch;
                mismatch.insert(QStringLiteral("path"), candidate.rawPath);
                mismatch.insert(QStringLiteral("confidence"), decision.confidence);
                mismatch.insert(QStringLiteral("fields"), candidate.rawFields);
                mismatch.insert(QStringLiteral("expectedArtist"), candidate.id3Artist);
                mismatch.insert(QStringLiteral("chosenArtist"),
                                displayArtist(candidate.fields.at(chosenIndex)));
                mismatches.append(mismatch);
            }
        }
    }
    result.insert(QStringLiteral("eligible"), eligible);
    result.insert(QStringLiteral("accuracy"), finaliseAccuracy(counts));
    result.insert(QStringLiteral("mismatchCount"), mismatchCount);
    result.insert(QStringLiteral("mismatchesListed"), mismatches.size());
    result.insert(QStringLiteral("mismatches"), mismatches);
    return result;
}
