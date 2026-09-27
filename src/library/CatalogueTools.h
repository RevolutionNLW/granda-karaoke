#pragma once

#include <QString>
#include <QVariantMap>

class Catalogue;

class CatalogueTools {
public:
    static QVariantMap evaluateGold(Catalogue& catalogue, const QString& goldPath,
                                    QString* error = nullptr);
    static QVariantMap reparse(Catalogue& catalogue, QString* error = nullptr);
    // Re-parses every stored raw file name (database only) and records the
    // parser version. The raw names themselves are never changed.
    // Title-screen text travels between installations as JSON keyed by CDG
    // content (a quick digest and size), never by path. Neither file may be
    // inside a library root.
    static QVariantMap exportTitleScreens(Catalogue& catalogue, const QString& path,
                                          QString* error = nullptr);
    static QVariantMap importTitleScreens(Catalogue& catalogue, const QString& path,
                                          QString* error = nullptr);
    static bool reparseStoredNames(Catalogue& catalogue, qint64* count = nullptr,
                                   QString* error = nullptr);
};
