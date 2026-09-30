#pragma once

#include "library/Catalogue.h"

#include <QAbstractTableModel>
#include <QHash>

#include <functional>
#include <vector>

// Library rows for the song table: one row per karaoke version. Every column
// answers the song roles; column 0's display text is the song title.
class LibraryResultsModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Role {
        SongIdRole = Qt::UserRole + 1,
        ArtistRole,
        DiscRole,
        MoreRole,
        LabelRole,
        DiscIdRole,  // compact disc-track, e.g. "SF123-04"
        PlaysRole,   // times sung (0 when never)
        PlayingRole, // this version is the song now playing
        KeyRole      // the song's musical key, e.g. "F#m" (empty when not known)
    };
    // Columns in model order; the table shows Artist first.
    enum Column { SongColumn, ArtistColumn, LabelColumn, KeyColumn, DiscColumn, PlaysColumn, ColumnCount };
    using PlayCountProvider = std::function<int(qint64 songId)>;
    using KeyProvider = std::function<QString(qint64 songId)>;

    static constexpr auto SongMimeType = "application/x-fks-song-id";

    explicit LibraryResultsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QMap<int, QVariant> itemData(const QModelIndex& index) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;

    void setRows(const QList<CatalogueSearchRow>& rows, bool showMoreRow = false);
    int songCount() const { return static_cast<int>(m_rows.size()); }
    int rowForSongId(qint64 songId) const;
    // Play counts are looked up only for rows on screen, and remembered
    // until forgetPlayCounts().
    void setPlayCountProvider(PlayCountProvider provider) { m_playCounts = std::move(provider); }
    void forgetPlayCount(qint64 songId);
    // Keys likewise: looked up for rows on screen, remembered until forgetKeys().
    void setKeyProvider(KeyProvider provider) { m_keys = std::move(provider); }
    void forgetKeys();
    void setPlayingSongId(qint64 songId);

private:
    static QString discAndTrack(const CatalogueSearchRow& row);
    static QString compactDiscId(const CatalogueSearchRow& row);
    int playCount(qint64 songId) const;
    QString songKey(qint64 songId) const;

    std::vector<CatalogueSearchRow> m_rows;
    bool m_showMoreRow = false;
    PlayCountProvider m_playCounts;
    mutable QHash<qint64, int> m_playCountCache;
    KeyProvider m_keys;
    mutable QHash<qint64, QString> m_keyCache;
    qint64 m_playingSongId = 0;
};
