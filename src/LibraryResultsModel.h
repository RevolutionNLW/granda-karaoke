#pragma once

#include "library/Catalogue.h"

#include <QAbstractListModel>

#include <vector>

class LibraryResultsModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        SongIdRole = Qt::UserRole + 1,
        ArtistRole,
        DiscRole,
        MoreRole
    };

    static constexpr auto SongMimeType = "application/x-fks-song-id";

    explicit LibraryResultsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QMap<int, QVariant> itemData(const QModelIndex& index) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;

    void setRows(const QList<CatalogueSearchRow>& rows, bool showMoreRow = false);
    int songCount() const { return static_cast<int>(m_rows.size()); }
    int rowForSongId(qint64 songId) const;

private:
    static QString discAndTrack(const CatalogueSearchRow& row);

    std::vector<CatalogueSearchRow> m_rows;
    bool m_showMoreRow = false;
};
