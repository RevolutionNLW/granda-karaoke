#pragma once

#include <QString>
#include <QVariantMap>

class Catalogue;

class CatalogueTools {
public:
    static QVariantMap evaluateGold(Catalogue& catalogue, const QString& goldPath,
                                    QString* error = nullptr);
    static QVariantMap reparse(Catalogue& catalogue, QString* error = nullptr);
};
