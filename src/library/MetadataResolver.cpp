#include "library/MetadataResolver.h"

#include "library/Catalogue.h"
#include "library/FilenameParser.h"
#include "library/Id3Reader.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

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
};

struct Evidence {
    QHash<QString, VoteCounts> groupVotes;
    QHash<QString, VoteCounts> seriesVotes;
};

struct Decision {
    Order order = Order::Unknown;
    QString source = QStringLiteral("fallback");
    QString confidence = QStringLiteral("none");
};

Decision decide(const Candidate& candidate, const Evidence& evidence, bool hideOwnTag);

QString collapseWhitespace(QString value)
{
    value.replace(QChar(0x00a0), QLatin1Char(' '));
    value.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
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
    if (value.count(QLatin1Char(',')) != 1
        || value.contains(QRegularExpression(QStringLiteral(R"(\d)"))))
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

void addVote(VoteCounts& counts, Order order, int weight)
{
    if (order == Order::ArtistTitle) {
        counts.artistTitleWeight += weight;
        ++counts.artistTitleCount;
        if (weight >= 2)
            ++counts.artistTitleReliableCount;
    } else if (order == Order::TitleArtist) {
        counts.titleArtistWeight += weight;
        ++counts.titleArtistCount;
        if (weight >= 2)
            ++counts.titleArtistReliableCount;
    }
}

VoteCounts withoutCandidate(VoteCounts counts, const Candidate& candidate)
{
    if (candidate.voteOrder == Order::ArtistTitle) {
        counts.artistTitleWeight -= candidate.voteWeight;
        --counts.artistTitleCount;
        if (candidate.voteWeight >= 2)
            --counts.artistTitleReliableCount;
    } else if (candidate.voteOrder == Order::TitleArtist) {
        counts.titleArtistWeight -= candidate.voteWeight;
        --counts.titleArtistCount;
        if (candidate.voteWeight >= 2)
            --counts.titleArtistReliableCount;
    }
    return counts;
}

Order strongVote(const VoteCounts& counts)
{
    auto strong = [](int winningWeight, int losingWeight, int winningCount,
                     int reliableCount) {
        return winningCount >= 2 && winningWeight >= 6
            && reliableCount >= 1
            && winningWeight - losingWeight >= 4
            && winningWeight >= (2 * losingWeight + 2);
    };
    if (strong(counts.artistTitleWeight, counts.titleArtistWeight,
               counts.artistTitleCount, counts.artistTitleReliableCount))
        return Order::ArtistTitle;
    if (strong(counts.titleArtistWeight, counts.artistTitleWeight,
               counts.titleArtistCount, counts.titleArtistReliableCount))
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

bool createMissingSongs(QSqlDatabase database, qint64 rootId, QString* error)
{
    QSqlQuery missing(database);
    QString sql = QStringLiteral("SELECT id FROM sources WHERE song_id IS NULL");
    if (rootId >= 0)
        sql += QStringLiteral(" AND root_id=?");
    missing.prepare(sql);
    if (rootId >= 0)
        missing.addBindValue(rootId);
    if (!missing.exec()) {
        if (error)
            *error = databaseError(missing, QStringLiteral("Could not find unresolved sources"));
        return false;
    }
    QList<qint64> sourceIds;
    while (missing.next())
        sourceIds.append(missing.value(0).toLongLong());
    missing.finish();
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin unresolved-song batch: %1").arg(database.lastError().text());
        return false;
    }
    qint64 created = 0;
    for (qint64 sourceId : sourceIds) {
        QSqlQuery create(database);
        if (!create.exec(QStringLiteral("INSERT INTO songs(search_text) VALUES('')"))) {
            database.rollback();
            if (error)
                *error = databaseError(create, QStringLiteral("Could not create song"));
            return false;
        }
        QSqlQuery attach(database);
        attach.prepare(QStringLiteral("UPDATE sources SET song_id=? WHERE id=?"));
        attach.addBindValue(create.lastInsertId());
        attach.addBindValue(sourceId);
        if (!attach.exec()) {
            database.rollback();
            if (error)
                *error = databaseError(attach, QStringLiteral("Could not attach song"));
            return false;
        }
        if ((++created % 500) == 0 && (!database.commit() || !database.transaction())) {
            if (error)
                *error = QStringLiteral("Could not commit unresolved-song batch: %1").arg(database.lastError().text());
            return false;
        }
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit unresolved songs: %1").arg(database.lastError().text());
        return false;
    }
    return true;
}

bool loadCandidates(QSqlDatabase database, qint64 rootId, QList<Candidate>* candidates,
                    QString* error)
{
    QString sql = QStringLiteral(
        "SELECT s.id,s.song_id,s.root_id,s.parsed_json,COALESCE(f.rel_dir,''),"
        "COALESCE(f.rel_path,s.zip_mp3_member,''),COALESCE(f.raw_tags_json,'') "
        "FROM sources s LEFT JOIN files f ON f.id=s.mp3_file_id ");
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
        candidate.id3Title = tags.value(QStringLiteral("title")).toString();
        candidate.id3Artist = tags.value(QStringLiteral("artist")).toString();
        candidates->append(candidate);
    }
    return true;
}

Evidence prepareEvidence(QList<Candidate>& candidates)
{
    QHash<QString, QSet<QString>> partners;
    QHash<QString, QSet<QString>> canonicalPartners;
    QHash<QString, QHash<QString, QSet<QString>>> groupPartners;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (candidate.fields.size() != 2)
            continue;
        const QString a = fieldKey(candidate.fields.at(0));
        const QString b = fieldKey(candidate.fields.at(1));
        if (a.isEmpty() || b.isEmpty())
            continue;
        groupPartners[groupKey(candidate)][a].insert(b);
        groupPartners[groupKey(candidate)][b].insert(a);
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
            const int aLocalPartners = groupPartners.value(groupKey(candidate))
                                           .value(fieldKey(a)).size();
            const int bLocalPartners = groupPartners.value(groupKey(candidate))
                                           .value(fieldKey(b)).size();
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
            const int aPartners = partners.value(fieldKey(a)).size();
            const int bPartners = partners.value(fieldKey(b)).size();
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
        if (candidate.voteOrder != Order::Unknown) {
            addVote(evidence.groupVotes[groupKey(candidate)], candidate.voteOrder,
                    candidate.voteWeight);
            addVote(evidence.seriesVotes[seriesKey(candidate)], candidate.voteOrder,
                    candidate.voteWeight);
        }
    }

    struct RecurrenceSupport {
        int artist = 0;
        int title = 0;
    };
    QHash<QString, RecurrenceSupport> support;
    for (const Candidate& candidate : std::as_const(candidates)) {
        if (candidate.fields.size() != 2)
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
    const VoteCounts group = withoutCandidate(evidence.groupVotes.value(groupKey(candidate)),
                                               candidate);
    const VoteCounts series = withoutCandidate(evidence.seriesVotes.value(seriesKey(candidate)),
                                                candidate);
    const Order strongGroup = strongVote(group);
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

} // namespace

bool MetadataResolver::resolve(Catalogue& catalogue, qint64 rootId, QString* error)
{
    QSqlDatabase database = catalogue.database();
    if (!database.isOpen()) {
        if (error)
            *error = QStringLiteral("Catalogue is not open");
        return false;
    }
    if (!createMissingSongs(database, rootId, error))
        return false;

    QList<Candidate> candidates;
    if (!loadCandidates(database, rootId, &candidates, error))
        return false;
    const Evidence evidence = prepareEvidence(candidates);

    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE songs SET title=?,artist=?,title_raw=?,artist_raw=?,disc_id=?,"
        "disc_prefix=?,track=?,display_title=?,display_artist=?,search_text=?,"
        "metadata_source=?,confidence=?,best_source_id=(SELECT id FROM sources WHERE song_id=? "
        "ORDER BY playable DESC,CASE kind WHEN 'loose_cdg' THEN 0 WHEN 'loose_mcg' THEN 1 ELSE 2 END,id LIMIT 1),"
        "playable=EXISTS(SELECT 1 FROM sources WHERE song_id=? AND playable=1) WHERE id=?"));
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin metadata-resolution batch: %1").arg(database.lastError().text());
        return false;
    }
    qint64 resolved = 0;
    for (const Candidate& candidate : std::as_const(candidates)) {
        QString titleRaw;
        QString artistRaw;
        QString title;
        QString artist;
        Decision decision;
        if (candidate.fields.size() >= 2) {
            decision = decide(candidate, evidence, false);
            if (decision.order == Order::TitleArtist) {
                titleRaw = candidate.rawFields.at(0);
                artistRaw = candidate.rawFields.at(1);
                title = candidate.fields.at(0);
                artist = candidate.fields.at(1);
            } else {
                artistRaw = candidate.rawFields.at(0);
                titleRaw = candidate.rawFields.at(1);
                artist = candidate.fields.at(0);
                title = candidate.fields.at(1);
            }
        } else if (candidate.fields.size() == 1) {
            titleRaw = candidate.rawFields.first();
            title = candidate.fields.first();
            if (!isPlaceholderTagValue(candidate.id3Artist)) {
                artistRaw = candidate.id3Artist;
                artist = cleanedField(candidate.id3Artist);
            }
            decision.source = QStringLiteral("filename_single_field");
            decision.confidence = QStringLiteral("low");
        }
        if (titleRaw.isEmpty()) {
            ParsedName parsed;
            parsed.discId = candidate.discId;
            parsed.track = candidate.track;
            parsed.fallbackFolder = candidate.fallbackFolder;
            parsed.cleaned = QFileInfo(candidate.rawPath).completeBaseName();
            titleRaw = fallbackTitle(parsed);
            title = titleRaw;
            decision.source = QStringLiteral("fallback");
            decision.confidence = QStringLiteral("none");
        }
        title = collapseWhitespace(title);
        artist = collapseWhitespace(artist);
        const QString shownArtist = displayArtist(artist);
        QStringList searchParts = {title, artist, shownArtist, candidate.discId};
        if (candidate.track > 0)
            searchParts.append(QString::number(candidate.track));

        const QVariantList values = {
            title, artist, titleRaw, artistRaw, candidate.discId, candidate.discPrefix,
            candidate.track, title, shownArtist,
            normalizeForSearch(searchParts.join(QLatin1Char(' '))),
            decision.source, decision.confidence, candidate.songId, candidate.songId,
            candidate.songId};
        for (int i = 0; i < values.size(); ++i)
            update.bindValue(i, values.at(i));
        if (!update.exec()) {
            database.rollback();
            if (error)
                *error = databaseError(update, QStringLiteral("Could not store resolved metadata"));
            return false;
        }
        if ((++resolved % 500) == 0 && (!database.commit() || !database.transaction())) {
            if (error)
                *error = QStringLiteral("Could not commit metadata-resolution batch: %1").arg(database.lastError().text());
            return false;
        }
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit metadata resolution: %1").arg(database.lastError().text());
        return false;
    }
    QSqlQuery cleanup(database);
    if (!cleanup.exec(QStringLiteral(
            "DELETE FROM songs WHERE NOT EXISTS(SELECT 1 FROM sources WHERE song_id=songs.id)"))) {
        if (error)
            *error = databaseError(cleanup, QStringLiteral("Could not remove empty songs"));
        return false;
    }
    return true;
}

QVariantMap MetadataResolver::evaluateTags(Catalogue& catalogue, int mismatchLimit,
                                           QString* error)
{
    QVariantMap result;
    QSqlDatabase database = catalogue.database();
    QList<Candidate> candidates;
    if (!loadCandidates(database, -1, &candidates, error))
        return result;
    const Evidence evidence = prepareEvidence(candidates);
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
