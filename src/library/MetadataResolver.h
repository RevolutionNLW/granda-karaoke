#pragma once

#include <QtTypes>
#include <QVariantMap>

class Catalogue;
class QString;

class MetadataResolver {
public:
    // Rebuilds resolved metadata exclusively from parsed_json and raw_tags_json
    // already stored in the catalogue. It never opens a library file.
    static bool resolve(Catalogue& catalogue, qint64 rootId = -1, QString* error = nullptr);
    static QVariantMap evaluateTags(Catalogue& catalogue, int mismatchLimit,
                                    QString* error = nullptr);
};
