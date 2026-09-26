#include "PlaylistView.h"

#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

constexpr int ItemIdRole = Qt::UserRole + 1;
constexpr int BaseTextRole = Qt::UserRole + 2;
constexpr auto PlaylistItemMimeType = "application/x-fks-playlist-item-id";

class PlaylistListWidget final : public QListWidget {
public:
    using DropAcceptance = std::function<bool(const QMimeData*)>;

    explicit PlaylistListWidget(DropAcceptance acceptsDrop, QWidget* parent = nullptr)
        : QListWidget(parent)
        , m_acceptsDrop(std::move(acceptsDrop))
    {
    }

protected:
    QStringList mimeTypes() const override
    {
        QStringList types = QListWidget::mimeTypes();
        if (!types.contains(QString::fromLatin1(LibraryResultsModel::SongMimeType)))
            types.append(QString::fromLatin1(LibraryResultsModel::SongMimeType));
        if (!types.contains(QString::fromLatin1(PlaylistItemMimeType)))
            types.append(QString::fromLatin1(PlaylistItemMimeType));
        return types;
    }

    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        QMimeData* data = QListWidget::mimeData(items);
        if (items.size() == 1) {
            const qint64 itemId = items.first()->data(ItemIdRole).toLongLong();
            if (itemId != 0)
                data->setData(PlaylistItemMimeType, QByteArray::number(itemId));
        }
        return data;
    }

    bool dropMimeData(int, const QMimeData* data, Qt::DropAction) override
    {
        return data && (data->hasFormat(LibraryResultsModel::SongMimeType)
                        || data->hasFormat(PlaylistItemMimeType));
    }

    Qt::DropActions supportedDropActions() const override
    {
        return Qt::CopyAction | Qt::MoveAction;
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        QListWidget::dragEnterEvent(event);
        acceptStoreDrop(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        QListWidget::dragMoveEvent(event);
        acceptStoreDrop(event);
        const int y = event->position().toPoint().y();
        if (event->isAccepted() && hasAutoScroll()
            && (y < autoScrollMargin()
                || y > viewport()->height() - autoScrollMargin())) {
            startAutoScroll();
            doAutoScroll();
        }
    }

private:
    void acceptStoreDrop(QDropEvent* event)
    {
        if (!m_acceptsDrop || !m_acceptsDrop(event->mimeData())) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    DropAcceptance m_acceptsDrop;
};

std::optional<qint64> playlistItemId(const QMimeData* mimeData)
{
    if (!mimeData || !mimeData->hasFormat(PlaylistItemMimeType))
        return std::nullopt;
    bool ok = false;
    const qint64 itemId = mimeData->data(PlaylistItemMimeType).toLongLong(&ok);
    return ok && itemId != 0 ? std::optional<qint64>(itemId) : std::nullopt;
}

std::optional<qint64> librarySongId(const QMimeData* mimeData)
{
    if (!mimeData || !mimeData->hasFormat(LibraryResultsModel::SongMimeType))
        return std::nullopt;
    bool ok = false;
    const qint64 songId = mimeData->data(
        LibraryResultsModel::SongMimeType).toLongLong(&ok);
    return ok && songId != 0 ? std::optional<qint64>(songId) : std::nullopt;
}

QPushButton* makeSmallButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumHeight(36);
    return button;
}

} // namespace

PlaylistView::PlaylistView(PlaylistStore* store, LibraryController* libraryController,
                           PlaylistPlayback* playback, QWidget* parent)
    : QWidget(parent)
    , m_store(store)
    , m_libraryController(libraryController)
    , m_playback(playback)
{
    auto* title = new QLabel(QStringLiteral("Frankie's Playlists"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    title->setFont(titleFont);

    m_chooser = new QComboBox(this);
    m_chooser->setObjectName(QStringLiteral("playlistChooser"));
    m_chooser->setMinimumHeight(42);

    m_newButton = makeSmallButton(QStringLiteral("New Playlist"), this);
    m_renameButton = makeSmallButton(QStringLiteral("Rename"), this);
    m_deleteButton = makeSmallButton(QStringLiteral("Delete"), this);
    auto* admin = new QHBoxLayout;
    admin->addWidget(m_newButton);
    admin->addWidget(m_renameButton);
    admin->addWidget(m_deleteButton);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignCenter);

    m_items = new PlaylistListWidget(
        [this](const QMimeData* mimeData) { return canAcceptDrop(mimeData); }, this);
    m_items->setObjectName(QStringLiteral("playlistItems"));
    m_items->setSelectionMode(QAbstractItemView::SingleSelection);
    m_items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_items->setFocusPolicy(Qt::StrongFocus);
    m_items->setDragEnabled(true);
    m_items->setAcceptDrops(true);
    m_items->viewport()->setAcceptDrops(true);
    m_items->setDragDropMode(QAbstractItemView::DragDrop);
    m_items->setDefaultDropAction(Qt::CopyAction);
    m_items->setDropIndicatorShown(true);
    m_items->installEventFilter(this);
    m_items->viewport()->installEventFilter(this);

    m_playButton = makeSmallButton(QStringLiteral("Play"), this);
    m_moveUpButton = makeSmallButton(QStringLiteral("Move Up"), this);
    m_moveDownButton = makeSmallButton(QStringLiteral("Move Down"), this);
    m_removeButton = makeSmallButton(QStringLiteral("Remove"), this);
    auto* actions = new QHBoxLayout;
    actions->addWidget(m_playButton);
    actions->addWidget(m_moveUpButton);
    actions->addWidget(m_moveDownButton);
    actions->addWidget(m_removeButton);

    m_autoplayButton = makeSmallButton(QStringLiteral("Autoplay: Off"), this);
    m_autoplayButton->setCheckable(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(title);
    layout->addWidget(m_chooser);
    layout->addLayout(admin);
    layout->addWidget(m_message);
    layout->addWidget(m_items, 1);
    layout->addLayout(actions);
    layout->addWidget(m_autoplayButton);

    connect(m_chooser, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_store && m_store->isOpen() && displayedPlaylistId() != 0)
            m_store->setLastPlaylistId(displayedPlaylistId());
        refreshItems();
        emit displayedPlaylistChanged(displayedPlaylistId() != 0);
    });
    connect(m_items, &QListWidget::currentItemChanged,
            this, [this] { updateButtons(); });
    connect(m_items, &QListWidget::itemDoubleClicked,
            this, [this] { playSelected(); });
    connect(m_newButton, &QPushButton::clicked, this, &PlaylistView::createPlaylist);
    connect(m_renameButton, &QPushButton::clicked, this, &PlaylistView::renamePlaylist);
    connect(m_deleteButton, &QPushButton::clicked, this, &PlaylistView::deletePlaylist);
    connect(m_playButton, &QPushButton::clicked, this, &PlaylistView::playSelected);
    connect(m_moveUpButton, &QPushButton::clicked,
            this, [this] { moveSelected(-1); });
    connect(m_moveDownButton, &QPushButton::clicked,
            this, [this] { moveSelected(1); });
    connect(m_removeButton, &QPushButton::clicked, this, &PlaylistView::removeSelected);
    connect(m_autoplayButton, &QPushButton::toggled, this, [this](bool enabled) {
        m_autoplayButton->setText(enabled ? QStringLiteral("Autoplay: On")
                                          : QStringLiteral("Autoplay: Off"));
        if (m_store && m_store->isOpen()) {
            QString error;
            if (!m_store->setAutoplay(enabled, &error))
                showStoreError(QStringLiteral("Autoplay could not be saved."), error);
        }
    });
    if (m_playback) {
        connect(m_playback, &PlaylistPlayback::contextChanged,
                this, &PlaylistView::updatePlaybackMarkers);
        connect(m_playback, &PlaylistPlayback::playbackStateChanged,
                this, &PlaylistView::updatePlaybackMarkers);
    }
    if (m_libraryController) {
        connect(m_libraryController, &LibraryController::libraryReady,
                this, [this] { refreshItems(); });
        // The final metadata/duplicate pass and offline marking happen after
        // libraryReady, so refresh once more when the whole scan finishes.
        connect(m_libraryController, &LibraryController::scanFinished,
                this, [this](const QVariantMap&) { refreshItems(); });
    }
    refresh();
}

void PlaylistView::activate()
{
    refresh();
}

void PlaylistView::showMessage(const QString& message)
{
    m_message->setText(message);
}

qint64 PlaylistView::displayedPlaylistId() const
{
    return m_chooser->currentData().toLongLong();
}

qint64 PlaylistView::selectedItemId() const
{
    const QListWidgetItem* selected = m_items->currentItem();
    return selected ? selected->data(ItemIdRole).toLongLong() : 0;
}

void PlaylistView::refresh()
{
    if (!m_store || !m_store->isOpen()) {
        m_chooser->clear();
        m_items->clear();
        m_message->setText(QStringLiteral("Playlists are unavailable right now."));
        m_chooser->setEnabled(false);
        m_newButton->setEnabled(false);
        m_autoplayButton->setEnabled(false);
        updateButtons();
        emit displayedPlaylistChanged(false);
        return;
    }
    m_chooser->setEnabled(true);
    m_newButton->setEnabled(true);
    m_autoplayButton->setEnabled(true);
    {
        const QSignalBlocker blocker(m_autoplayButton);
        m_autoplayButton->setChecked(m_store->autoplay());
        m_autoplayButton->setText(m_autoplayButton->isChecked()
                                      ? QStringLiteral("Autoplay: On")
                                      : QStringLiteral("Autoplay: Off"));
    }
    const qint64 preferred = displayedPlaylistId() != 0
        ? displayedPlaylistId() : m_store->lastPlaylistId();
    refreshPlaylists(preferred);
}

void PlaylistView::refreshPlaylists(qint64 preferredId)
{
    const QList<PlaylistInfo> all = m_store->playlists();
    const QSignalBlocker blocker(m_chooser);
    m_chooser->clear();
    int select = -1;
    for (const PlaylistInfo& playlist : all) {
        m_chooser->addItem(playlist.name, playlist.id);
        if (playlist.id == preferredId)
            select = m_chooser->count() - 1;
    }
    if (select < 0 && !all.isEmpty())
        select = 0;
    m_chooser->setCurrentIndex(select);
    if (select >= 0)
        m_store->setLastPlaylistId(displayedPlaylistId());
    refreshItems();
    emit displayedPlaylistChanged(select >= 0);
}

QString PlaylistView::displayText(const PlaylistEntry& entry) const
{
    QString title = entry.title.trimmed();
    if (title.isEmpty() && !entry.discId.trimmed().isEmpty()) {
        title = QStringLiteral("Disc %1").arg(entry.discId.trimmed());
        if (entry.track > 0)
            title += QStringLiteral(" Track %1").arg(entry.track);
    }
    if (title.isEmpty())
        title = QFileInfo(entry.mp3RelPath).completeBaseName();
    if (title.isEmpty())
        title = QStringLiteral("Unknown song");
    const QString artist = entry.artist.trimmed();
    return artist.isEmpty() ? title : title + QStringLiteral(" — ") + artist;
}

void PlaylistView::refreshItems(qint64 selectItemId, bool ensureVisible)
{
    if (selectItemId == 0)
        selectItemId = selectedItemId();
    const int scrollValue = m_items->verticalScrollBar()->value();
    m_items->clear();
    const qint64 playlistId = displayedPlaylistId();
    if (!m_store || !m_store->isOpen() || playlistId == 0) {
        if (m_store && m_store->isOpen())
            m_message->setText(QStringLiteral("No playlists yet — press New Playlist"));
        updateButtons();
        return;
    }

    m_message->clear();
    QListWidgetItem* selected = nullptr;
    for (const PlaylistEntry& entry : m_store->items(playlistId)) {
        PlaylistEntry displayEntry = entry;
        if (m_libraryController) {
            const PlaylistSongResolution resolution =
                m_libraryController->resolvePlaylistSong(entry);
            if (resolution.updateStoredSongId)
                m_store->updateSongId(entry.itemId, resolution.songId);
            const auto current = resolution
                ? m_libraryController->songRef(resolution.songId) : std::nullopt;
            if (current) {
                displayEntry.title = current->title;
                displayEntry.artist = current->artist;
                displayEntry.discId = current->discId;
                displayEntry.track = current->track;
            }
        }
        const QString text = displayText(displayEntry);
        auto* row = new QListWidgetItem(text, m_items);
        row->setData(ItemIdRole, entry.itemId);
        row->setData(BaseTextRole, text);
        if (entry.itemId == selectItemId)
            selected = row;
    }
    if (selected) {
        m_items->setCurrentItem(selected);
    } else if (m_items->count() > 0) {
        m_items->setCurrentRow(0);
    }
    if (selected && ensureVisible) {
        m_items->scrollToItem(selected);
    } else if (!selected && m_items->count() > 0) {
        m_items->verticalScrollBar()->setValue(0);
    } else {
        m_items->verticalScrollBar()->setValue(scrollValue);
    }
    updatePlaybackMarkers();
    updateButtons();
}

void PlaylistView::updatePlaybackMarkers()
{
    const auto context = m_playback ? m_playback->context()
                                    : std::optional<PlaybackContext>();
    const KaraokePlayer::State state = m_playback
        ? m_playback->playerState() : KaraokePlayer::State::Empty;
    const bool active = state == KaraokePlayer::State::Playing
        || state == KaraokePlayer::State::Paused;
    const qint64 playlistId = displayedPlaylistId();
    for (int row = 0; row < m_items->count(); ++row) {
        QListWidgetItem* item = m_items->item(row);
        QString text = item->data(BaseTextRole).toString();
        if (active && context && context->playlistId == playlistId
            && context->itemId == item->data(ItemIdRole).toLongLong())
            text.prepend(QStringLiteral("▶ "));
        item->setText(text);
    }
}

void PlaylistView::updateButtons()
{
    const bool hasPlaylist = m_store && m_store->isOpen()
        && displayedPlaylistId() != 0;
    const bool hasItem = hasPlaylist && selectedItemId() != 0;
    m_renameButton->setEnabled(hasPlaylist);
    m_deleteButton->setEnabled(hasPlaylist);
    m_playButton->setEnabled(hasItem);
    m_moveUpButton->setEnabled(hasItem && m_items->currentRow() > 0);
    m_moveDownButton->setEnabled(hasItem && m_items->currentRow() >= 0
                                 && m_items->currentRow() < m_items->count() - 1);
    m_removeButton->setEnabled(hasItem);
}

std::optional<QString> PlaylistView::askForName(const QString& title,
                                                const QString& currentName)
{
    if (m_namePrompt)
        return m_namePrompt(this, title, currentName);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, title,
        QStringLiteral("Playlist name:"), QLineEdit::Normal, currentName, &accepted);
    return accepted ? std::optional<QString>(name) : std::nullopt;
}

void PlaylistView::createPlaylist()
{
    const auto name = askForName(QStringLiteral("New Playlist"));
    if (!name)
        return;
    qint64 id = 0;
    QString error;
    if (!m_store->createPlaylist(*name, &id, &error)) {
        showStoreError(QStringLiteral("The playlist could not be created."), error);
        return;
    }
    refreshPlaylists(id);
}

void PlaylistView::renamePlaylist()
{
    const auto current = m_store->playlist(displayedPlaylistId());
    if (!current)
        return;
    const auto name = askForName(QStringLiteral("Rename Playlist"), current->name);
    if (!name)
        return;
    QString error;
    if (!m_store->renamePlaylist(current->id, *name, &error)) {
        showStoreError(QStringLiteral("The playlist could not be renamed."), error);
        return;
    }
    refreshPlaylists(current->id);
}

void PlaylistView::deletePlaylist()
{
    const auto current = m_store->playlist(displayedPlaylistId());
    if (!current)
        return;
    const bool confirmed = m_deleteConfirmation
        ? m_deleteConfirmation(this, current->name)
        : QMessageBox::question(this, QStringLiteral("Delete Playlist"),
              QStringLiteral("Delete \"%1\"? Songs and files will not be deleted.")
                  .arg(current->name),
              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    if (!confirmed)
        return;
    QString error;
    if (!m_store->deletePlaylist(current->id, &error)) {
        showStoreError(QStringLiteral("The playlist could not be deleted."), error);
        return;
    }
    refreshPlaylists();
}

void PlaylistView::addSong(qint64 songId)
{
    const qint64 playlistId = displayedPlaylistId();
    if (!m_store || !m_store->isOpen() || playlistId == 0 || !m_libraryController)
        return;
    QString error;
    const auto song = m_libraryController->songRef(songId, &error);
    if (!song) {
        showStoreError(QStringLiteral("This song could not be added right now."), error);
        return;
    }
    qint64 itemId = 0;
    if (!m_store->addItem(playlistId, *song, &itemId, &error)) {
        showStoreError(QStringLiteral("This song could not be added right now."), error);
        return;
    }
    refreshItems(itemId, true);
}

void PlaylistView::playSelected()
{
    const auto entry = m_store ? m_store->item(selectedItemId()) : std::nullopt;
    if (entry)
        emit playRequested(*entry, false);
}

void PlaylistView::moveSelected(int delta)
{
    const qint64 itemId = selectedItemId();
    if (!m_store || itemId == 0)
        return;
    QString error;
    const bool moved = delta < 0 ? m_store->moveItemUp(itemId, &error)
                                 : m_store->moveItemDown(itemId, &error);
    if (!moved && !error.isEmpty()) {
        showStoreError(QStringLiteral("The song could not be moved."), error);
        return;
    }
    if (moved)
        refreshItems(itemId, true);
}

void PlaylistView::removeSelected()
{
    const qint64 itemId = selectedItemId();
    if (!m_store || itemId == 0)
        return;
    const auto entry = m_store->item(itemId);
    if (!entry)
        return;
    const auto playlist = m_store->playlist(entry->playlistId);
    if (!playlist)
        return;
    const int oldRow = m_items->currentRow();
    const QListWidgetItem* selected = m_items->currentItem();
    const QString songText = selected
        ? selected->data(BaseTextRole).toString() : displayText(*entry);
    const bool confirmed = m_removeConfirmation
        ? m_removeConfirmation(this, songText, playlist->name)
        : QMessageBox::question(this, QStringLiteral("Remove Song"),
              QStringLiteral("Remove \"%1\" from \"%2\"? The song stays in your library.")
                  .arg(songText, playlist->name),
              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    if (!confirmed || !m_store->item(itemId))
        return;
    QString error;
    if (!m_store->removeItem(itemId, &error)) {
        showStoreError(QStringLiteral("The song could not be removed."), error);
        return;
    }
    refreshItems();
    if (m_items->count() > 0)
        m_items->setCurrentRow(qMin(oldRow, m_items->count() - 1));
}

bool PlaylistView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_items->viewport()
        && event->type() == QEvent::Drop) {
        auto acceptCopy = [](QDropEvent* drop) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
        };
        auto* drop = static_cast<QDropEvent*>(event);
        const qint64 playlistId = displayedPlaylistId();
        const auto draggedItem = playlistItemId(drop->mimeData());
        const auto draggedSong = drop->mimeData()->hasFormat(PlaylistItemMimeType)
            ? std::nullopt : librarySongId(drop->mimeData());
        if (!canAcceptDrop(drop->mimeData())) {
            drop->ignore();
            return true;
        }

        int targetRow = m_items->count();
        const QModelIndex targetIndex = m_items->indexAt(drop->position().toPoint());
        if (targetIndex.isValid()) {
            const QRect targetRect = m_items->visualRect(targetIndex);
            targetRow = targetIndex.row()
                + (drop->position().y() < targetRect.top() + targetRect.height() / 2.0
                       ? 0 : 1);
        }

        QString error;
        qint64 selectedId = 0;
        bool changed = false;
        if (draggedItem) {
            const auto current = m_store->item(*draggedItem);
            if (!current || current->playlistId != playlistId) {
                drop->ignore();
                return true;
            }
            if (current->position < targetRow)
                --targetRow;
            targetRow = qBound(0, targetRow, qMax(0, m_items->count() - 1));
            selectedId = *draggedItem;
            changed = m_store->moveItem(selectedId, targetRow, &error);
            if (!changed && error.isEmpty())
                changed = true;
        } else {
            const auto song = m_libraryController->songRef(*draggedSong, &error);
            if (song)
                changed = m_store->insertItem(playlistId, targetRow, *song,
                                              &selectedId, &error);
        }
        if (!changed) {
            if (!error.isEmpty())
                showStoreError(QStringLiteral("The song could not be dropped."), error);
            drop->ignore();
            return true;
        }
        refreshItems(selectedId, true);
        acceptCopy(drop);
        return true;
    }
    if (watched == m_items && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up) {
            moveSelected(-1);
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            moveSelected(1);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            playSelected();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            emit backRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool PlaylistView::canAcceptDrop(const QMimeData* mimeData) const
{
    const qint64 playlistId = displayedPlaylistId();
    const bool hasPlaylistItem = mimeData
        && mimeData->hasFormat(PlaylistItemMimeType);
    const auto draggedItem = playlistItemId(mimeData);
    const auto draggedSong = hasPlaylistItem
        ? std::nullopt : librarySongId(mimeData);
    const auto itemEntry = m_store && m_store->isOpen() && draggedItem
        ? m_store->item(*draggedItem) : std::nullopt;
    return m_store && m_store->isOpen() && playlistId != 0
        && ((itemEntry && itemEntry->playlistId == playlistId)
            || (draggedSong && m_libraryController));
}

void PlaylistView::showStoreError(const QString& fallback, const QString& detail)
{
    Q_UNUSED(detail)
    m_message->setText(fallback);
}
