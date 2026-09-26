#include "library/CatalogueTools.h"

#include "library/Catalogue.h"
#include "library/FilenameParser.h"
#include "library/MetadataResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include <utility>

namespace {

QString queryError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

void addResult(QVariantMap& counts, const QString& confidence, bool correct)
{
    QVariantMap row = counts.value(confidence).toMap();
    row.insert(QStringLiteral("total"), row.value(QStringLiteral("total")).toLongLong() + 1);
    if (correct)
        row.insert(QStringLiteral("correct"), row.value(QStringLiteral("correct")).toLongLong() + 1);
    counts.insert(confidence, row);
}

QVariantMap finaliseCounts(const QVariantMap& counts)
{
    QVariantMap result;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        QVariantMap row = it.value().toMap();
        const qlonglong total = row.value(QStringLiteral("total")).toLongLong();
        const qlonglong correct = row.value(QStringLiteral("correct")).toLongLong();
        row.insert(QStringLiteral("accuracyPercent"),
                   total == 0 ? 0.0 : (100.0 * double(correct) / double(total)));
        result.insert(it.key(), row);
    }
    return result;
}

QVariantMap kindDistribution(QSqlDatabase database, QString* error)
{
    QVariantMap result;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT CAST(json_extract(parsed_json,'$.kind') AS INTEGER),count(*) "
            "FROM sources GROUP BY 1 ORDER BY 1"))) {
        if (error)
            *error = queryError(query, QStringLiteral("Could not count parser kinds"));
        return result;
    }
    while (query.next()) {
        const int value = query.value(0).toInt();
        const auto kind = static_cast<ParsedName::Kind>(value);
        result.insert(parsedKindName(kind), query.value(1));
    }
    return result;
}

} // namespace

QVariantMap CatalogueTools::evaluateGold(Catalogue& catalogue, const QString& goldPath,
                                         QString* error)
{
    QVariantMap result;
    QFile gold(goldPath);
    if (!gold.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Could not read gold file: %1").arg(gold.errorString());
        return result;
    }

    QSqlQuery lookup(catalogue.database());
    lookup.prepare(QStringLiteral(
        "SELECT so.artist_raw,so.display_artist,so.confidence,s.parsed_json "
        "FROM files f JOIN sources s ON s.mp3_file_id=f.id "
        "JOIN songs so ON so.id=s.song_id WHERE f.rel_path=? COLLATE NOCASE "
        "ORDER BY s.id LIMIT 1"));

    QVariantMap counts;
    QVariantList mismatches;
    qlonglong labelled = 0;
    qlonglong evaluated = 0;
    qlonglong ambiguous = 0;
    qlonglong missing = 0;
    qlonglong invalid = 0;
    while (!gold.atEnd()) {
        QString line = QString::fromUtf8(gold.readLine());
        if (line.endsWith(QLatin1Char('\n')))
            line.chop(1);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        const qsizetype tab = line.lastIndexOf(QLatin1Char('\t'));
        if (tab < 0) {
            ++invalid;
            continue;
        }
        bool valid = false;
        const int fieldNumber = line.mid(tab + 1).trimmed().toInt(&valid);
        const QString path = line.left(tab);
        if (!valid || fieldNumber < 0 || fieldNumber > 2) {
            ++invalid;
            continue;
        }
        ++labelled;
        if (fieldNumber == 0) {
            ++ambiguous;
            continue;
        }
        lookup.bindValue(0, QDir::fromNativeSeparators(path));
        if (!lookup.exec()) {
            if (error)
                *error = queryError(lookup, QStringLiteral("Gold evaluation query failed"));
            return {};
        }
        if (!lookup.next()) {
            ++missing;
            lookup.finish();
            continue;
        }
        const QString artistRaw = lookup.value(0).toString();
        const QString displayArtist = lookup.value(1).toString();
        const QString confidence = lookup.value(2).toString();
        const QJsonObject parsed = QJsonDocument::fromJson(lookup.value(3).toByteArray()).object();
        const QJsonArray fields = parsed.value(QStringLiteral("fields")).toArray();
        if (fields.size() < fieldNumber) {
            ++invalid;
            lookup.finish();
            continue;
        }
        const QString expected = fields.at(fieldNumber - 1).toString();
        const bool correct = normalizeForSearch(artistRaw) == normalizeForSearch(expected);
        addResult(counts, confidence, correct);
        ++evaluated;
        if (!correct) {
            QVariantMap mismatch;
            mismatch.insert(QStringLiteral("path"), path);
            mismatch.insert(QStringLiteral("confidence"), confidence);
            mismatch.insert(QStringLiteral("fields"), fields.toVariantList());
            mismatch.insert(QStringLiteral("expectedArtist"), expected);
            mismatch.insert(QStringLiteral("chosenArtist"), displayArtist);
            mismatches.append(mismatch);
        }
        lookup.finish();
    }
    result.insert(QStringLiteral("labelled"), labelled);
    result.insert(QStringLiteral("evaluated"), evaluated);
    result.insert(QStringLiteral("ambiguousSkipped"), ambiguous);
    result.insert(QStringLiteral("missingPaths"), missing);
    result.insert(QStringLiteral("invalidRows"), invalid);
    result.insert(QStringLiteral("accuracy"), finaliseCounts(counts));
    result.insert(QStringLiteral("mismatches"), mismatches);
    return result;
}


QVariantMap CatalogueTools::reparse(Catalogue& catalogue, QString* error)
{
    QVariantMap result;
    QSqlDatabase database = catalogue.database();
    result.insert(QStringLiteral("before"), kindDistribution(database, error));
    if (error && !error->isEmpty())
        return {};

    QSqlQuery rows(database);
    if (!rows.exec(QStringLiteral(
            "SELECT s.id,s.parsed_json,COALESCE(f.rel_dir,''),"
            "COALESCE(f.rel_path,s.zip_mp3_member,'') "
            "FROM sources s LEFT JOIN files f ON f.id=s.mp3_file_id ORDER BY s.id"))) {
        if (error)
            *error = queryError(rows, QStringLiteral("Could not load stored filenames"));
        return {};
    }
    struct StoredName { qint64 id; QString rawPath; QString rawFileName; QString relDir; };
    QList<StoredName> names;
    while (rows.next()) {
        const QJsonObject old = QJsonDocument::fromJson(rows.value(1).toByteArray()).object();
        const QString rawPath = old.value(QStringLiteral("rawPath")).toString(rows.value(3).toString());
        QString rawFileName = old.value(QStringLiteral("rawFileName")).toString();
        if (rawFileName.isEmpty())
            rawFileName = QFileInfo(rawPath).fileName();
        QString relDir = rows.value(2).toString();
        if (relDir.isEmpty()) {
            relDir = QFileInfo(rawPath).path();
            if (relDir == QLatin1String("."))
                relDir.clear();
        }
        names.append({rows.value(0).toLongLong(), rawPath, rawFileName, relDir});
    }
    rows.finish();
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin stored-name reparse: %1")
                         .arg(database.lastError().text());
        return {};
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE sources SET parsed_json=? WHERE id=?"));
    for (const StoredName& name : std::as_const(names)) {
        const ParsedName parsed = parseSongName(name.relDir, name.rawFileName);
        update.bindValue(0, QString::fromUtf8(
            QJsonDocument(parsedNameJson(parsed, name.rawPath, name.rawFileName))
                .toJson(QJsonDocument::Compact)));
        update.bindValue(1, name.id);
        if (!update.exec()) {
            database.rollback();
            if (error)
                *error = queryError(update, QStringLiteral("Could not store reparsed filename"));
            return {};
        }
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit stored-name reparse: %1")
                         .arg(database.lastError().text());
        return {};
    }
    if (!MetadataResolver::resolve(catalogue, -1, error))
        return {};
    result.insert(QStringLiteral("reparsed"), names.size());
    result.insert(QStringLiteral("after"), kindDistribution(database, error));
    return result;
}
