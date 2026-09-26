#include "LibraryResultsModel.h"

#include <QMimeData>

LibraryResultsModel::LibraryResultsModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LibraryResultsModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return songCount() + (m_showMoreRow ? 1 : 0);
}

QVariant LibraryResultsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0
        || index.row() >= rowCount())
        return {};

    if (index.row() == songCount()) {
        if (role == Qt::DisplayRole)
            return QStringLiteral("More songs match - type more words");
        if (role == MoreRole)
            return true;
        return {};
    }

    const CatalogueSearchRow& row = m_rows.at(static_cast<std::size_t>(index.row()));
    switch (role) {
    case Qt::DisplayRole:
        return row.displayTitle;
    case SongIdRole:
        return row.songId;
    case ArtistRole:
        return row.displayArtist;
    case DiscRole:
        return discAndTrack(row);
    case MoreRole:
        return false;
    default:
        return {};
    }
}

QMap<int, QVariant> LibraryResultsModel::itemData(const QModelIndex& index) const
{
    QMap<int, QVariant> roles;
    const int modelRoles[] = {Qt::DisplayRole, SongIdRole, ArtistRole, DiscRole, MoreRole};
    for (const int role : modelRoles) {
        const QVariant value = data(index, role);
        if (value.isValid())
            roles.insert(role, value);
    }
    return roles;
}

Qt::ItemFlags LibraryResultsModel::flags(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() == songCount())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList LibraryResultsModel::mimeTypes() const
{
    return {QString::fromLatin1(SongMimeType)};
}

QMimeData* LibraryResultsModel::mimeData(const QModelIndexList& indexes) const
{
    auto* mimeData = new QMimeData;
    qint64 songId = 0;
    for (const QModelIndex& index : indexes) {
        const qint64 candidate = data(index, SongIdRole).toLongLong();
        if (candidate == 0 || (songId != 0 && candidate != songId))
            return mimeData;
        songId = candidate;
    }
    if (songId != 0)
        mimeData->setData(SongMimeType, QByteArray::number(songId));
    return mimeData;
}

Qt::DropActions LibraryResultsModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

void LibraryResultsModel::setRows(const QList<CatalogueSearchRow>& rows, bool showMoreRow)
{
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const CatalogueSearchRow& row : rows)
        m_rows.push_back(row);
    m_showMoreRow = showMoreRow;
    endResetModel();
}

int LibraryResultsModel::rowForSongId(qint64 songId) const
{
    if (songId == 0)
        return -1;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).songId == songId)
            return static_cast<int>(i);
    }
    return -1;
}

QString LibraryResultsModel::discAndTrack(const CatalogueSearchRow& row)
{
    QString text = row.discId;
    if (row.track > 0) {
        if (!text.isEmpty())
            text += QStringLiteral("  ");
        text += QStringLiteral("Track %1").arg(row.track);
    }
    return text;
}
