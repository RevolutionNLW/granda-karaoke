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
        QStringLiteral(R"(^\s*([A-Za-z]{1,8})[\s-]*(\d{1,6})[\s-]*$)"));
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
    // " - " is the usual separator. "--" between two names is one as well,
    // with or without spaces ("Act Naturally--Buck Owens"); "8--3" is not.
    QStringList pieces;
    static const QRegularExpression letter(QStringLiteral(R"(\p{L})"),
                                           QRegularExpression::UseUnicodePropertiesOption);
    // " - " inside brackets belongs to the name: "(Remix - Radio Version)".
    QStringList spaced;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('(') || c == QLatin1Char('['))
            ++depth;
        else if ((c == QLatin1Char(')') || c == QLatin1Char(']')) && depth > 0)
            --depth;
        else if (depth == 0 && c == QLatin1Char('-') && i > 0 && i + 1 < text.size()
                 && text.at(i - 1).isSpace() && text.at(i + 1).isSpace()) {
            spaced.append(text.mid(start, i - start));
            start = i + 1;
        }
    }
    spaced.append(text.mid(start));
    // A truncated name can leave a bracket open ("(Theme From The Legend O -
    // Pat Benatar"): then the brackets say nothing and every " - " separates.
    if (depth > 0)
        spaced = text.split(QRegularExpression(QStringLiteral(R"(\s+-\s+)")), Qt::SkipEmptyParts);
    for (const QString& rawPiece : std::as_const(spaced)) {
        const QString piece = rawPiece.trimmed();
        if (piece.isEmpty())
            continue;
        const QStringList parts = piece.split(QRegularExpression(QStringLiteral(R"(\s*-{2,}\s*)")),
                                              Qt::SkipEmptyParts);
        bool allNamed = parts.size() > 1;
        for (const QString& part : parts)
            allNamed = allNamed && part.contains(letter);
        if (allNamed)
            pieces.append(parts);
        else
            pieces.append(piece);
    }
    for (const QString& piece : pieces) {
        const QString field = collapseWhitespace(piece);
        if (!field.isEmpty())
            result.append(field);
    }
    return result;
}

bool underscoresSeparateFields(const QString& stem)
{
    if (!stem.contains(QLatin1Char(' ')) || !stem.contains(QLatin1Char('_'))
        || stem.contains(QRegularExpression(QStringLiteral(R"(_-_|_-\s|\s-_)"))))
        return false;
    if (stem.count(QLatin1Char('_')) > 4)
        return false;
    // "Guns_N_Roses" or "Don_t" use underscores inside a name: a one- or
    // two-letter word touching an underscore is a word fragment, not a field.
    static const QRegularExpression fragment(
        QStringLiteral(R"((?<![\p{L}\p{N}])\p{L}{1,2}_|_\p{L}{1,2}(?![\p{L}\p{N}])|__|^_|_$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return !fragment.match(stem.trimmed()).hasMatch();
}

void setDisc(ParsedName& parsed, const QString& disc, const QString& source)
{
    parsed.discId = canonicalDisc(disc);
    parsed.discPrefix = discPrefix(parsed.discId);
    parsed.discSource = source;
}

} // namespace

namespace {
ParsedName parseCleanedName(const QString& relativeDir, const QString& stem,
                            const QString& cleaned);
} // namespace

ParsedName parseSongName(const QString& relativeDir, const QString& fileName)
{
    const QString stem = QFileInfo(fileName).completeBaseName();
    // An underscore normally stands for a space ("Cliff_Richard_-_Thank_You").
    ParsedName parsed = parseCleanedName(relativeDir, stem,
                                         QString(stem).replace(QLatin1Char('_'), QLatin1Char(' ')));
    // A name that uses spaces between words but has no second name field may
    // use underscores as its field separator ("ZOOM004-01_24 HOURS FROM
    // TULSA_GENE PITNEY", "MRH62-11 - Kasabian_underdog"). When " - " already
    // gives two names, an underscore is part of a name ("Newton John_Travolta").
    if (parsed.fields.size() <= 1 && underscoresSeparateFields(stem)) {
        const ParsedName separated = parseCleanedName(
            relativeDir, stem, QString(stem).replace(QLatin1Char('_'), QStringLiteral(" - ")));
        if (separated.fields.size() == 2) {
            // Only a heuristic: the resolver shows this split only with strong
            // order evidence, and otherwise keeps the name whole.
            ParsedName result = separated;
            result.underscoreSplit = true;
            static const QRegularExpression afterCode(QStringLiteral(R"(^[^_]*\d_)"));
            result.underscoreAfterCode = afterCode.match(stem).hasMatch()
                && !result.discId.isEmpty();
            result.unsplit = parsed.fields.isEmpty() ? QString() : parsed.fields.first();
            return result;
        }
    }
    return parsed;
}

namespace {

ParsedName parseCleanedName(const QString& relativeDir, const QString& stem,
                            const QString& cleaned)
{
    ParsedName parsed;
    parsed.stem = stem;
    parsed.cleaned = collapseWhitespace(cleaned);

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
        remainder.remove(QRegularExpression(QStringLiteral(R"(^\s*-+\s*)")));
        remainder = collapseWhitespace(remainder);
        parsed.fields = splitFields(remainder);
        parsed.kind = parsed.fields.isEmpty() ? ParsedName::Kind::DiscTrackOnly
                                              : ParsedName::Kind::DiscTrackFields;
        return parsed;
    }

    // A packed ID (EZH00807, PM00411) is only split when a folder corroborates
    // its series and numeric disc. Without that evidence it remains free text.
    // A one-digit disc ("sgb301" in folder sgb3) and a space ("leg 12203" in
    // folder LEG 122) are accepted only with that same folder corroboration.
    static const QRegularExpression packed(QStringLiteral(R"(^([A-Za-z]{2,8}) ?(\d{1,6})(\d{2})$)"));
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

    // "14--16" inside folder EK14: the leading number repeats the folder's disc
    // number, so the second number is the track. Without that folder it stays
    // free text.
    static const QRegularExpression discNumberTrack(
        QStringLiteral(R"(^(\d{1,6})\s*-{1,2}\s*(\d{1,3})$)"));
    match = discNumberTrack.match(parsed.cleaned);
    if (match.hasMatch() && !fromFolder.isEmpty()) {
        QString folderDigits = fromFolder;
        folderDigits.remove(QRegularExpression(QStringLiteral(R"(\D)")));
        if (!folderDigits.isEmpty()
            && folderDigits.toULongLong() == match.captured(1).toULongLong()) {
            setDisc(parsed, fromFolder, QStringLiteral("folder"));
            parsed.track = match.captured(2).toInt();
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

    // "Track 9", and "09 - Track 9" where the leading number repeats the track.
    static const QRegularExpression labelledTrack(
        QStringLiteral(R"(^(?:(\d{1,4})\s*-\s*)?Track\s*-?\s*(\d{1,4})\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    match = labelledTrack.match(parsed.cleaned);
    if (match.hasMatch() && (match.captured(1).isEmpty()
                             || match.captured(1).toInt() == match.captured(2).toInt())) {
        parsed.track = match.captured(2).toInt();
        useFolder();
        parsed.kind = ParsedName::Kind::TrackOnly;
        return parsed;
    }

    // "Track 5 MEET ME ON THE CORNER": a track label followed by a name.
    static const QRegularExpression labelledTrackName(
        QStringLiteral(R"(^Track\s*-?\s*(\d{1,4})\s+(\D.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    match = labelledTrackName.match(parsed.cleaned);
    if (match.hasMatch()) {
        parsed.track = match.captured(1).toInt();
        parsed.fields = splitFields(match.captured(2));
        useFolder();
        parsed.kind = parsed.discId.isEmpty() ? ParsedName::Kind::TrackFields
                                              : ParsedName::Kind::DiscTrackFields;
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

} // namespace

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
    if (parsed.underscoreSplit) {
        result.insert(QStringLiteral("underscoreSplit"), true);
        result.insert(QStringLiteral("underscoreAfterCode"), parsed.underscoreAfterCode);
        result.insert(QStringLiteral("unsplit"), parsed.unsplit);
    }
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
