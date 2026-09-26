#include "library/FilenameParser.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

QString collapseWhitespace(QString value)
{
    value.replace(QChar(0x00a0), QLatin1Char(' '));
    value.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    return value.trimmed();
}

QString canonicalDisc(QString token)
{
    token = token.toUpper();
    token.remove(QRegularExpression(QStringLiteral(R"([\s.-]+)")));
    return token;
}

QString discPrefix(const QString& disc)
{
    QString result;
    for (const QChar c : disc) {
        if (c.isLetter())
            result.append(c);
    }
    return result;
}

bool isGenericTrackFolder(const QString& value)
{
    static const QRegularExpression generic(
        QStringLiteral(R"(^\s*(?:Track|CD|Disc)\s*-?\s*\d{1,4}\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return generic.match(value).hasMatch();
}

QStringList folderParts(const QString& relativeDir)
{
    return relativeDir.split(QRegularExpression(QStringLiteral(R"([/\\]+)")),
                             Qt::SkipEmptyParts);
}

QString folderDisc(const QStringList& parts)
{
    // Folder IDs are deliberately conservative: a compact letter/number ID,
    // the observed "SF Gold N" family, "Vol N", or a numeric disc folder.
    static const QRegularExpression compact(
        QStringLiteral(R"(^\s*([A-Za-z]{1,8})[\s-]*(\d{1,6})\s*$)"));
    static const QRegularExpression gold(
        QStringLiteral(R"(^\s*([A-Za-z]{1,4})\s+Gold\s+(\d{1,4})\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression volume(QStringLiteral(R"(^\s*Vol\s+(\d{1,4})\s*$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression numeric(QStringLiteral(R"(^\s*(\d{1,6})\s*$)"));
    for (auto i = parts.crbegin(); i != parts.crend(); ++i) {
        if (isGenericTrackFolder(*i))
            continue;
        QRegularExpressionMatch match = compact.match(*i);
        if (match.hasMatch())
            return canonicalDisc(match.captured(1) + match.captured(2));
        match = gold.match(*i);
        if (match.hasMatch())
            return canonicalDisc(match.captured(1) + QStringLiteral("GOLD") + match.captured(2));
        match = volume.match(*i);
        if (match.hasMatch())
            return QStringLiteral("VOL") + match.captured(1);
        match = numeric.match(*i);
        if (match.hasMatch())
            return match.captured(1);
    }
    return {};
}

QString fallbackFolder(const QStringList& parts)
{
    for (auto i = parts.crbegin(); i != parts.crend(); ++i) {
        const QString value = collapseWhitespace(*i);
        if (!value.isEmpty() && !isGenericTrackFolder(value))
            return value;
    }
    return {};
}

QStringList splitFields(const QString& text)
{
    QStringList result;
    const QStringList pieces = text.split(QRegularExpression(QStringLiteral(R"(\s+-\s+)")),
                                          Qt::SkipEmptyParts);
    for (const QString& piece : pieces) {
        const QString field = collapseWhitespace(piece);
        if (!field.isEmpty())
            result.append(field);
    }
    return result;
}

void setDisc(ParsedName& parsed, const QString& disc, const QString& source)
{
    parsed.discId = canonicalDisc(disc);
    parsed.discPrefix = discPrefix(parsed.discId);
    parsed.discSource = source;
}

} // namespace

ParsedName parseSongName(const QString& relativeDir, const QString& fileName)
{
    ParsedName parsed;
    parsed.stem = QFileInfo(fileName).completeBaseName();
    parsed.cleaned = parsed.stem;
    parsed.cleaned.replace(QLatin1Char('_'), QLatin1Char(' '));
    parsed.cleaned = collapseWhitespace(parsed.cleaned);

    const QStringList directories = folderParts(relativeDir);
    const QString fromFolder = folderDisc(directories);
    parsed.fallbackFolder = fallbackFolder(directories);
    auto useFolder = [&] {
        if (parsed.discId.isEmpty() && !fromFolder.isEmpty())
            setDisc(parsed, fromFolder, QStringLiteral("folder"));
    };

    // Three-number forms such as US2-001-06 use the first two groups as the
    // disc ID. Standard IDs require letters plus disc digits before the track,
    // which prevents ordinary text such as "Utils-95 applause" becoming an ID.
    // The track must not run into further letters or digits ("Askfp10-1-07 Name").
    // A single-letter dotted prefix ("B.Spark04-03") is part of the ID.
    static const QRegularExpression threePart(
        QStringLiteral(R"(^\s*([A-Za-z]{1,12}\d{1,4})\s*-\s*(\d{1,4})-(\d{1,4})(?![A-Za-z0-9])(.*)$)"));
    static const QRegularExpression discTrack(
        QStringLiteral(R"(^\s*((?:\d{1,3})?(?:[A-Za-z]\.)?[A-Za-z]{1,12}(?:[ -]?\d{1,6})[A-Za-z]{0,12})\s*(?:-\s*|\s+)(\d{1,4})(.*)$)"));

    QRegularExpressionMatch match = threePart.match(parsed.cleaned);
    QString remainder;
    if (match.hasMatch()) {
        setDisc(parsed, match.captured(1) + match.captured(2), QStringLiteral("filename"));
        parsed.track = match.captured(3).toInt();
        remainder = match.captured(4);
    } else {
        match = discTrack.match(parsed.cleaned);
        if (match.hasMatch()) {
            setDisc(parsed, match.captured(1), QStringLiteral("filename"));
            parsed.track = match.captured(2).toInt();
            remainder = match.captured(3);
        }
    }
    if (!parsed.discId.isEmpty()) {
        remainder.remove(QRegularExpression(QStringLiteral(R"(^\s*-\s*)")));
        remainder = collapseWhitespace(remainder);
        parsed.fields = splitFields(remainder);
        parsed.kind = parsed.fields.isEmpty() ? ParsedName::Kind::DiscTrackOnly
                                              : ParsedName::Kind::DiscTrackFields;
        return parsed;
    }

    // A packed ID (EZH00807, PM00411) is only split when a folder corroborates
    // its series and numeric disc. Without that evidence it remains free text.
    static const QRegularExpression packed(QStringLiteral(R"(^([A-Za-z]{2,8})(\d{2,6})(\d{2})$)"));
    match = packed.match(parsed.cleaned);
    if (match.hasMatch() && !fromFolder.isEmpty()) {
        const QString packedPrefix = match.captured(1).toUpper();
        const QString folderPrefix = discPrefix(fromFolder);
        QString folderDigits = fromFolder;
        folderDigits.remove(QRegularExpression(QStringLiteral(R"(\D)")));
        const QString packedDigits = match.captured(2);
        const bool prefixAgrees = packedPrefix.startsWith(folderPrefix)
            || folderPrefix.startsWith(packedPrefix);
        bool numbersAgree = false;
        if (!folderDigits.isEmpty())
            numbersAgree = folderDigits.toULongLong() == packedDigits.toULongLong();
        if (prefixAgrees && numbersAgree) {
            setDisc(parsed, packedPrefix + packedDigits, QStringLiteral("filename"));
            parsed.track = match.captured(3).toInt();
            parsed.kind = ParsedName::Kind::DiscTrackOnly;
            return parsed;
        }
    }

    // Explicit Track and Vol labels are not filename disc prefixes.  Vol N may
    // still provide a disc when it is a folder name (handled above).
    static const QRegularExpression volumeTrack(
        QStringLiteral(R"(^Vol\s+(\d{1,4})\s+-\s+Track\s*-?\s*(\d{1,4})\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    match = volumeTrack.match(parsed.cleaned);
    if (match.hasMatch()) {
        parsed.track = match.captured(2).toInt();
        useFolder();
        parsed.kind = ParsedName::Kind::TrackOnly;
        return parsed;
    }

    static const QRegularExpression labelledTrack(
        QStringLiteral(R"(^Track\s*-?\s*(\d{1,4})\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    match = labelledTrack.match(parsed.cleaned);
    if (match.hasMatch()) {
        parsed.track = match.captured(1).toInt();
        useFolder();
        parsed.kind = ParsedName::Kind::TrackOnly;
        return parsed;
    }

    static const QRegularExpression leadingTrack(
        QStringLiteral(R"(^(\d{1,4})(?:\.|\s+-)\s*(.+)$)"));
    match = leadingTrack.match(parsed.cleaned);
    const int possibleTrack = match.hasMatch() ? match.captured(1).toInt() : 0;
    if (match.hasMatch() && (possibleTrack <= 99 || !fromFolder.isEmpty())) {
        parsed.track = possibleTrack;
        parsed.fields = splitFields(match.captured(2));
        useFolder();
        parsed.kind = parsed.discId.isEmpty() ? ParsedName::Kind::TrackFields
                                              : ParsedName::Kind::DiscTrackFields;
        return parsed;
    }

    static const QRegularExpression digitsOnly(QStringLiteral(R"(^\d+$)"));
    if (digitsOnly.match(parsed.cleaned).hasMatch()) {
        const QString digits = parsed.cleaned;
        if (!fromFolder.isEmpty()) {
            setDisc(parsed, fromFolder, QStringLiteral("folder"));
            QString folderDigits = fromFolder;
            folderDigits.remove(QRegularExpression(QStringLiteral(R"(\D)")));
            if (!folderDigits.isEmpty() && digits.startsWith(folderDigits)
                && digits.size() > folderDigits.size() && digits.size() - folderDigits.size() <= 2)
                parsed.track = digits.mid(folderDigits.size()).toInt();
            else
                parsed.track = digits.toInt();
        } else {
            parsed.track = digits.toInt();
        }
        parsed.kind = ParsedName::Kind::NumericOnly;
        return parsed;
    }

    parsed.fields = splitFields(parsed.cleaned);
    if (parsed.fields.size() >= 2)
        parsed.kind = ParsedName::Kind::Fields;
    else
        parsed.kind = ParsedName::Kind::FreeText;
    useFolder();
    return parsed;
}

QJsonObject parsedNameJson(const ParsedName& parsed, const QString& rawPath,
                           const QString& rawFileName)
{
    QJsonObject result;
    result.insert(QStringLiteral("rawPath"), rawPath);
    result.insert(QStringLiteral("rawFileName"), rawFileName);
    result.insert(QStringLiteral("stem"), parsed.stem);
    result.insert(QStringLiteral("cleaned"), parsed.cleaned);
    result.insert(QStringLiteral("discId"), parsed.discId);
    result.insert(QStringLiteral("discPrefix"), parsed.discPrefix);
    result.insert(QStringLiteral("track"), parsed.track);
    result.insert(QStringLiteral("kind"), int(parsed.kind));
    result.insert(QStringLiteral("discSource"), parsed.discSource);
    result.insert(QStringLiteral("fallbackFolder"), parsed.fallbackFolder);
    QJsonArray fields;
    for (const QString& field : parsed.fields)
        fields.append(field);
    result.insert(QStringLiteral("fields"), fields);
    return result;
}

QString parsedKindName(ParsedName::Kind kind)
{
    switch (kind) {
    case ParsedName::Kind::DiscTrackFields: return QStringLiteral("disc_track_fields");
    case ParsedName::Kind::TrackFields: return QStringLiteral("track_fields");
    case ParsedName::Kind::Fields: return QStringLiteral("fields");
    case ParsedName::Kind::DiscTrackOnly: return QStringLiteral("disc_track_only");
    case ParsedName::Kind::TrackOnly: return QStringLiteral("track_only");
    case ParsedName::Kind::NumericOnly: return QStringLiteral("numeric_only");
    case ParsedName::Kind::FreeText: return QStringLiteral("free_text");
    }
    return QStringLiteral("free_text");
}

QString normalizeForSearch(const QString& value)
{
    QString input = value;
    input.replace(QChar(0x2018), QLatin1Char('\''));
    input.replace(QChar(0x2019), QLatin1Char('\''));
    input.replace(QChar(0x201b), QLatin1Char('\''));
    input.replace(QChar(0x02bc), QLatin1Char('\''));
    input.replace(QLatin1Char('&'), QStringLiteral(" and "));
    input = input.normalized(QString::NormalizationForm_KD).toLower();

    QString result;
    result.reserve(input.size());
    for (const QChar c : input) {
        const QChar::Category category = c.category();
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing)
            continue;
        if (c.isLetterOrNumber())
            result.append(c);
        else
            result.append(QLatin1Char(' '));
    }
    return collapseWhitespace(result);
}

QString fallbackTitle(const ParsedName& parsed)
{
    if (!parsed.discId.isEmpty() && parsed.track > 0) {
        return QStringLiteral("Disc %1 - Track %2")
            .arg(parsed.discId, QString::number(parsed.track).rightJustified(2, QLatin1Char('0')));
    }
    if (!parsed.discId.isEmpty())
        return QStringLiteral("Disc %1").arg(parsed.discId);
    if (parsed.track > 0) {
        const QString track = QStringLiteral("Track %1").arg(
            QString::number(parsed.track).rightJustified(2, QLatin1Char('0')));
        return parsed.fallbackFolder.isEmpty()
            ? track : QStringLiteral("%1 - %2").arg(parsed.fallbackFolder, track);
    }
    return parsed.cleaned;
}
