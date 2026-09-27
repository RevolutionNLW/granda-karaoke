#include "LibraryResultsModel.h"

#include <QMimeData>

LibraryResultsModel::LibraryResultsModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int LibraryResultsModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return songCount() + (m_showMoreRow ? 1 : 0);
}

int LibraryResultsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant LibraryResultsModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::TextAlignmentRole)
        return int((section == PlaysColumn ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case SongColumn: return QStringLiteral("SONG");
    case ArtistColumn: return QStringLiteral("ARTIST");
    case LabelColumn: return QStringLiteral("LABEL");
    case DiscColumn: return QStringLiteral("DISC ID");
    case PlaysColumn: return QStringLiteral("PLAYS");
    default: return {};
    }
}

QVariant LibraryResultsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.column() < 0 || index.column() >= ColumnCount
        || index.row() < 0 || index.row() >= rowCount())
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
        switch (index.column()) {
        case ArtistColumn: return row.displayArtist;
        case LabelColumn: return row.label;
        case DiscColumn: return compactDiscId(row);
        case PlaysColumn: {
            const int plays = playCount(row.songId);
            return plays > 0 ? QVariant(plays) : QVariant(QString());
        }
        default: return row.displayTitle;
        }
    case LabelRole:
        return row.label;
    case DiscIdRole:
        return compactDiscId(row);
    case PlaysRole:
        return playCount(row.songId);
    case PlayingRole:
        return m_playingSongId != 0 && row.songId == m_playingSongId;
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
    const int modelRoles[] = {Qt::DisplayRole, SongIdRole, ArtistRole, DiscRole, MoreRole,
                              LabelRole, DiscIdRole, PlaysRole, PlayingRole};
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
    m_playCountCache.clear();
    endResetModel();
}

void LibraryResultsModel::forgetPlayCount(qint64 songId)
{
    m_playCountCache.remove(songId);
    const int row = rowForSongId(songId);
    if (row >= 0)
        emit dataChanged(index(row, PlaysColumn), index(row, PlaysColumn), {Qt::DisplayRole, PlaysRole});
}

void LibraryResultsModel::setPlayingSongId(qint64 songId)
{
    if (songId == m_playingSongId)
        return;
    const int before = rowForSongId(m_playingSongId);
    m_playingSongId = songId;
    for (const int row : {before, rowForSongId(songId)}) {
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {PlayingRole});
    }
}

int LibraryResultsModel::playCount(qint64 songId) const
{
    if (!m_playCounts || songId == 0)
        return 0;
    const auto cached = m_playCountCache.constFind(songId);
    if (cached != m_playCountCache.constEnd())
        return *cached;
    const int plays = m_playCounts(songId);
    m_playCountCache.insert(songId, plays);
    return plays;
}

QString LibraryResultsModel::compactDiscId(const CatalogueSearchRow& row)
{
    const QString disc = row.discId.trimmed();
    if (row.track <= 0)
        return disc;
    const QString track = QStringLiteral("%1").arg(row.track, 2, 10, QLatin1Char('0'));
    return disc.isEmpty() ? QStringLiteral("Track %1").arg(row.track) : disc + QLatin1Char('-') + track;
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
