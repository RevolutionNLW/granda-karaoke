#include "PlaylistView.h"

#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "library/SongKeys.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"
#include "ui/Controls.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QVBoxLayout>

#include <functional>

namespace {

constexpr int ItemIdRole = Qt::UserRole + 1;
constexpr int BaseTextRole = Qt::UserRole + 2;
constexpr int TitleRole = Qt::UserRole + 3;
constexpr int ArtistRole = Qt::UserRole + 4;
constexpr int DiscRole = Qt::UserRole + 5;
constexpr int PlayingRole = Qt::UserRole + 6;
constexpr int SongIdRole = Qt::UserRole + 7;
constexpr int KeyRole = Qt::UserRole + 8;  // the song's original key, as the library shows it
constexpr auto PlaylistItemMimeType = "application/x-fks-playlist-item-id";

// Where the playlist's columns sit in a row (shared by the rows and the
// column captions above them): number, song, artist and, when there is room,
// the key and the disc.
struct PlaylistColumns {
    QRect number;
    QRect song;
    QRect artist;
    QRect key;
    QRect disc;
};

PlaylistColumns playlistColumns(const QRect& row, bool withKey)
{
    PlaylistColumns columns;
    const int gap = theme::px(12);
    const QRect inner = row.adjusted(gap, 0, -gap, 0);
    columns.number = QRect(inner.left(), row.top(), theme::px(30), row.height());
    const int left = columns.number.right() + theme::px(10);
    const int available = inner.right() - left;
    const int disc = available >= theme::px(440) ? theme::px(104) : 0;
    // The key is short ("C", "F#m", "Bbm"), so its column is narrow; it is
    // left out rather than squeeze the song and artist in a narrow pane.
    const int key = withKey && available - disc >= theme::px(260) ? theme::px(38) : 0;
    const int names = available - disc - (key ? key + gap : 0);
    const int songWidth = names * 54 / 100;
    columns.song = QRect(left, row.top(), songWidth - gap, row.height());
    columns.artist = QRect(left + songWidth, row.top(),
                           names - songWidth - (disc ? gap : 0), row.height());
    if (key)
        columns.key = QRect(inner.right() - disc - (disc ? gap : 0) - key, row.top(), key, row.height());
    if (disc)
        columns.disc = QRect(inner.right() - disc, row.top(), disc, row.height());
    return columns;
}

// The playlist's rows: number (or the play mark for the song now playing),
// song, artist and disc. The now-playing mark is separate from the selection,
// which is gold only in the pane being used.
class PlaylistRowDelegate final : public QStyledItemDelegate {
public:
    PlaylistRowDelegate(QObject* parent, std::function<bool()> active, std::function<bool()> keys)
        : QStyledItemDelegate(parent)
        , m_active(std::move(active))
        , m_keys(std::move(keys))
    {
        m_mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(40, option.fontMetrics.height() + theme::rowPadding());
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool strong = selected && m_active();
        painter->fillRect(option.rect, selected
            ? (strong ? theme::color::selection : theme::color::selectionIdle)
            : (index.row() % 2 && theme::alternateRows() ? theme::color::rowAlternate
                                                          : theme::color::rowBase));
        if (strong)
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), theme::px(3),
                                    option.rect.height()), theme::color::accent);
        const PlaylistColumns columns = playlistColumns(option.rect, m_keys());
        const QColor main = strong ? theme::color::selectionText : theme::color::text;
        const QColor secondary = strong ? theme::color::selectionText
                                        : theme::color::secondaryText;
        const bool playing = index.data(PlayingRole).toBool();
        if (playing) {
            const int size = theme::px(12);
            const QRect mark(columns.number.left(), columns.number.center().y() - size / 2,
                             size, size);
            ui::glyphIcon(ui::Glyph::Play, theme::color::accent).paint(painter, mark);
        } else {
            painter->setPen(secondary);
            painter->drawText(columns.number, Qt::AlignLeft | Qt::AlignVCenter,
                              QString::number(index.row() + 1));
        }
        const auto draw = [painter](const QRect& rect, const QString& text, const QFont& font,
                                    const QColor& color) {
            if (rect.width() <= 0)
                return;
            painter->setFont(font);
            painter->setPen(color);
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                              painter->fontMetrics().elidedText(text, Qt::ElideRight,
                                                                rect.width()));
        };
        QFont songFont = option.font;
        songFont.setBold(playing);
        draw(columns.song, index.data(TitleRole).toString(), songFont, main);
        draw(columns.artist, index.data(ArtistRole).toString(), option.font, main);
        draw(columns.key, index.data(KeyRole).toString(), option.font, secondary);
        QFont mono = m_mono;
        mono.setPixelSize(theme::px(13));
        draw(columns.disc, index.data(DiscRole).toString(), mono, secondary);
        painter->restore();
    }

private:
    std::function<bool()> m_active;
    std::function<bool()> m_keys;
    QFont m_mono;
};

// The playlist tabs. A short name is always shown whole; a long one is cut
// short (the full name is its tool tip), and the tabs scroll when there are
// more than fit, rather than every name shrinking to a letter or two.
class PlaylistTabBar final : public QTabBar {
public:
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override
    {
        QSize size = QTabBar::tabSizeHint(index);
        size.setWidth(qMin(size.width(), theme::px(200)));
        return size;
    }
    QSize minimumTabSizeHint(int index) const override
    {
        QSize size = tabSizeHint(index);
        size.setWidth(qMin(size.width(), theme::px(140)));
        return size;
    }
};

// Column captions above the playlist rows, lined up with them.
class PlaylistHeader final : public QWidget {
public:
    PlaylistHeader(QListWidget* list, std::function<bool()> keys, QWidget* parent)
        : QWidget(parent)
        , m_list(list)
        , m_keys(std::move(keys))
    {
        setObjectName(QStringLiteral("listHeader"));
        theme::setFixedHeight(this, 34);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), theme::color::rowBase);
        painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(0x2a, 0x2d, 0x32));
        QFont font = this->font();
        font.setPixelSize(theme::px(11));
        font.setBold(true);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        painter.setFont(font);
        painter.setPen(theme::color::textMuted);
        const PlaylistColumns columns = playlistColumns(
            QRect(0, 0, m_list->viewport()->width(), height() - 1), m_keys());
        const auto caption = [&painter](const QRect& rect, const QString& text) {
            if (rect.width() > 0)
                painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        };
        caption(columns.number, QStringLiteral("#"));
        caption(columns.song, QStringLiteral("SONG"));
        caption(columns.artist, QStringLiteral("ARTIST"));
        caption(columns.key, QStringLiteral("KEY"));
        caption(columns.disc, QStringLiteral("DISC ID"));
    }

private:
    QListWidget* m_list;
    std::function<bool()> m_keys;
};

class PlaylistListWidget final : public QListWidget {
public:
    using DropAcceptance = std::function<bool(const QMimeData*)>;

    explicit PlaylistListWidget(DropAcceptance acceptsDrop, QWidget* parent = nullptr)
        : QListWidget(parent)
        , m_acceptsDrop(std::move(acceptsDrop))
    {
    }

    // A quiet line under the songs saying how to add more.
    void setHint(const QString& hint)
    {
        m_hint = hint;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QListWidget::paintEvent(event);
        if (m_hint.isEmpty())
            return;
        const int top = count() > 0 ? visualItemRect(item(count() - 1)).bottom() + 1 : 0;
        const QRect area(theme::px(12), top + theme::px(8), viewport()->width() - theme::px(24),
                         fontMetrics().height() + theme::px(8));
        if (area.bottom() > viewport()->height())
            return;
        QPainter painter(viewport());
        QFont font = this->font();
        font.setPixelSize(theme::px(12));
        painter.setFont(font);
        painter.setPen(QColor(0x75, 0x72, 0x6c));
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(font).elidedText(m_hint, Qt::ElideRight, area.width()));
    }

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
        QMimeData* mime = QListWidget::mimeData(items);
        if (items.size() == 1) {
            const qint64 itemId = items.first()->data(ItemIdRole).toLongLong();
            if (itemId != 0)
                mime->setData(PlaylistItemMimeType, QByteArray::number(itemId));
        }
        return mime;
    }

    bool dropMimeData(int, const QMimeData* mime, Qt::DropAction) override
    {
        return mime && (mime->hasFormat(LibraryResultsModel::SongMimeType)
                        || mime->hasFormat(PlaylistItemMimeType));
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
    QString m_hint;
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

// A playlist song's name as shown: its title, else its disc and track, else
// its file name.
QString entryTitle(const PlaylistEntry& entry)
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
    return title;
}

QPushButton* makeSmallButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
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
    auto* pane = new QFrame(this);
    pane->setObjectName(QStringLiteral("pane"));
    auto* title = new QLabel(QStringLiteral("Frankie's Playlists"), pane);
    title->setObjectName(QStringLiteral("paneTitle"));
    // The same height as the library pane's title row, so the two line up.
    theme::setFixedHeight(title, 26);
    m_countLabel = new QLabel(pane);
    m_countLabel->setObjectName(QStringLiteral("paneCount"));

    // The playlist's own list of names drives everything; it is shown as tabs.
    m_chooser = new QComboBox(pane);
    m_chooser->setObjectName(QStringLiteral("playlistChooser"));
    m_chooser->setFocusPolicy(Qt::NoFocus);
    m_chooser->hide();
    m_tabs = new PlaylistTabBar(pane);
    m_tabs->setObjectName(QStringLiteral("playlistTabs"));
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setUsesScrollButtons(true);
    // Its scroll arrows must never take the keyboard either.
    for (QWidget* arrow : m_tabs->findChildren<QWidget*>())
        arrow->setFocusPolicy(Qt::NoFocus);

    // Creating, renaming and deleting playlists is occasional: small and
    // out of the way.
    m_newButton = makeSmallButton(QStringLiteral("+"), pane);
    m_newButton->setToolTip(QStringLiteral("New playlist"));
    m_renameButton = makeSmallButton(QStringLiteral("Rename"), pane);
    m_deleteButton = makeSmallButton(QStringLiteral("Delete"), pane);
    m_newButton->setObjectName(QStringLiteral("ghostButton"));
    for (QPushButton* button : {m_renameButton, m_deleteButton})
        button->setObjectName(QStringLiteral("linkButton"));
    auto* headerFrame = new QFrame(pane);
    headerFrame->setObjectName(QStringLiteral("paneHeader"));
    auto* header = new QVBoxLayout(headerFrame);
    header->setContentsMargins(16, 12, 12, 10);
    header->setSpacing(10);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(6);
    titleRow->addWidget(title);
    titleRow->addWidget(m_countLabel);
    titleRow->addStretch();
    titleRow->addWidget(m_renameButton);
    titleRow->addWidget(m_deleteButton);
    auto* tabRow = new QHBoxLayout;
    tabRow->setSpacing(6);
    tabRow->addWidget(m_tabs, 1);
    tabRow->addWidget(m_newButton);
    header->addLayout(titleRow);
    header->addLayout(tabRow);

    m_message = new QLabel(pane);
    m_message->setObjectName(QStringLiteral("paneHint"));
    m_message->setWordWrap(true);
    theme::setContentsMargins(m_message, 16, 8, 16, 0);
    m_message->hide();

    auto* items = new PlaylistListWidget(
        [this](const QMimeData* mimeData) { return canAcceptDrop(mimeData); }, pane);
    m_items = items;
    m_items->setObjectName(QStringLiteral("playlistItems"));
    m_items->setSelectionMode(QAbstractItemView::SingleSelection);
    m_items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_items->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_items->setTextElideMode(Qt::ElideRight);
    m_items->setUniformItemSizes(true);
    m_items->setItemDelegate(new PlaylistRowDelegate(m_items, [this] { return m_active; },
                                                     [this] { return m_showKeys; }));
    m_items->setFocusPolicy(Qt::StrongFocus);
    m_items->setDragEnabled(true);
    m_items->setAcceptDrops(true);
    m_items->viewport()->setAcceptDrops(true);
    m_items->setDragDropMode(QAbstractItemView::DragDrop);
    m_items->setDefaultDropAction(Qt::CopyAction);
    m_items->setDropIndicatorShown(true);
    m_items->installEventFilter(this);
    m_items->viewport()->installEventFilter(this);
    auto* columnHeader = new PlaylistHeader(m_items, [this] { return m_showKeys; }, pane);
    m_columnHeader = columnHeader;
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        m_items->doItemsLayout();
        m_items->viewport()->update();
        m_columnHeader->update();
    });

    m_playButton = makeSmallButton(QStringLiteral("Play"), pane);
    m_moveUpButton = makeSmallButton(QStringLiteral("↑ Up"), pane);
    m_moveDownButton = makeSmallButton(QStringLiteral("↓ Down"), pane);
    m_removeButton = makeSmallButton(QStringLiteral("Remove"), pane);
    for (QPushButton* button : {m_playButton, m_moveUpButton, m_moveDownButton, m_removeButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    m_playButton->setIcon(ui::glyphIcon(ui::Glyph::Play, theme::color::accent));
    auto* footer = new QFrame(pane);
    footer->setObjectName(QStringLiteral("paneFooter"));
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(12, 8, 12, 8);
    actions->setSpacing(6);
    actions->addWidget(m_playButton);
    actions->addStretch();
    actions->addWidget(m_moveUpButton);
    actions->addWidget(m_moveDownButton);
    actions->addWidget(m_removeButton);

    m_autoplayButton = new ui::ToggleSwitch(pane);
    m_autoplayButton->setText(QStringLiteral("Autoplay: Off"));
    m_autoplayButton->setObjectName(QStringLiteral("autoplayButton"));
    m_autoplayButton->setToolTip(QStringLiteral("Play the next playlist song by itself"));

    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(1, 1, 1, 1);
    paneLayout->setSpacing(0);
    paneLayout->addWidget(headerFrame);
    paneLayout->addWidget(m_message);
    paneLayout->addWidget(columnHeader);
    paneLayout->addWidget(m_items, 1);
    paneLayout->addWidget(footer);
    m_autoplayRow = new QHBoxLayout;
    m_autoplayRow->setContentsMargins(14, 0, 14, 10);
    m_autoplayRow->addWidget(m_autoplayButton);
    paneLayout->addLayout(m_autoplayRow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pane);

    connect(m_tabs, &QTabBar::tabBarClicked, this, &PlaylistView::interacted);
    for (QPushButton* button : {m_newButton, m_renameButton, m_deleteButton, m_playButton,
                                m_moveUpButton, m_moveDownButton, m_removeButton})
        connect(button, &QPushButton::pressed, this, &PlaylistView::interacted);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int tab) {
        if (tab >= 0 && tab != m_chooser->currentIndex())
            m_chooser->setCurrentIndex(tab);
    });
    connect(m_chooser, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_tabs->currentIndex() != index) {
            const QSignalBlocker blocker(m_tabs);
            m_tabs->setCurrentIndex(index);
        }
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
        // libraryReady; every finished scan, and every name correction, is
        // followed by catalogueChanged.
        connect(m_libraryController, &LibraryController::catalogueChanged,
                this, [this] { refreshItems(); });
        // A key set or cleared by hand, or found by analysis: shown at once,
        // the rows themselves (selection, scroll) left as they are.
        connect(m_libraryController, &LibraryController::songKeysChanged,
                this, &PlaylistView::refreshKeys);
    }
    refresh();
}

QString PlaylistView::rowKeyText(int row) const
{
    const QListWidgetItem* item = m_items->item(row);
    return item ? item->data(KeyRole).toString() : QString();
}

void PlaylistView::setKeyColumnShown(bool shown)
{
    if (shown == m_showKeys)
        return;
    m_showKeys = shown;
    refreshKeys();
    m_items->viewport()->update();
    m_columnHeader->update();
}

void PlaylistView::refreshKeys()
{
    // Looked up only while the column is shown, all rows in one go.
    if (!m_showKeys || !m_libraryController || m_items->count() == 0)
        return;
    QList<qint64> songIds;
    for (int row = 0; row < m_items->count(); ++row) {
        if (const qint64 songId = m_items->item(row)->data(SongIdRole).toLongLong(); songId > 0)
            songIds.append(songId);
    }
    // The library's own rule: the key set by hand, else a confident detected one.
    const QHash<qint64, SongKeyInfo> keys = m_libraryController->songKeys(songIds);
    for (int row = 0; row < m_items->count(); ++row) {
        QListWidgetItem* item = m_items->item(row);
        const auto key = keys.constFind(item->data(SongIdRole).toLongLong());
        const QString text = key != keys.constEnd() ? songKeyName(key->shownKeyIndex()) : QString();
        if (item->data(KeyRole).toString() != text)
            item->setData(KeyRole, text);
    }
}

void PlaylistView::showAdjacentPlaylist(int step)
{
    const int tab = m_tabs->currentIndex() + step;
    if (tab >= 0 && tab < m_tabs->count()) {
        m_tabs->setCurrentIndex(tab);
        emit interacted();
    }
}

void PlaylistView::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    m_items->viewport()->update();
}

QPushButton* PlaylistView::takeAutoplayButton()
{
    m_autoplayRow->removeWidget(m_autoplayButton);
    // The empty row must not leave a gap under the playlist's buttons.
    m_autoplayRow->setContentsMargins(0, 0, 0, 0);
    m_autoplayRow->setProperty("fksLayoutMargins", QVariant::fromValue(QMargins()));
    return m_autoplayButton;
}

void PlaylistView::activate()
{
    refresh();
}

void PlaylistView::setMessageText(const QString& text)
{
    // The message line takes no room while there is nothing to say.
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
}

void PlaylistView::showMessage(const QString& message)
{
    setMessageText(message);
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
        while (m_tabs->count() > 0)
            m_tabs->removeTab(0);
        m_items->clear();
        setMessageText(QStringLiteral("Playlists are unavailable right now."));
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
    const QSignalBlocker tabBlocker(m_tabs);
    m_chooser->clear();
    while (m_tabs->count() > 0)
        m_tabs->removeTab(0);
    int select = -1;
    for (const PlaylistInfo& playlist : all) {
        m_chooser->addItem(playlist.name, playlist.id);
        m_tabs->addTab(playlist.name);
        m_tabs->setTabToolTip(m_tabs->count() - 1, playlist.name);
        if (playlist.id == preferredId)
            select = m_chooser->count() - 1;
    }
    if (select < 0 && !all.isEmpty())
        select = 0;
    m_chooser->setCurrentIndex(select);
    m_tabs->setCurrentIndex(select);
    if (select >= 0)
        m_store->setLastPlaylistId(displayedPlaylistId());
    refreshItems();
    emit displayedPlaylistChanged(select >= 0);
}

QString PlaylistView::displayText(const PlaylistEntry& entry) const
{
    const QString title = entryTitle(entry);
    const QString artist = entry.artist.trimmed();
    return artist.isEmpty() ? title : title + QStringLiteral(" \u2014 ") + artist;
}

void PlaylistView::refreshItems(qint64 selectItemId, bool ensureVisible)
{
    if (selectItemId == 0)
        selectItemId = selectedItemId();
    const int scrollValue = m_items->verticalScrollBar()->value();
    m_items->clear();
    m_countLabel->clear();
    static_cast<PlaylistListWidget*>(m_items)->setHint({});
    const qint64 playlistId = displayedPlaylistId();
    if (!m_store || !m_store->isOpen() || playlistId == 0) {
        if (m_store && m_store->isOpen())
            setMessageText(QStringLiteral("No playlists yet \u2014 press New"));
        updateButtons();
        return;
    }

    setMessageText({});
    QListWidgetItem* selected = nullptr;
    for (const PlaylistEntry& entry : m_store->items(playlistId)) {
        PlaylistEntry displayEntry = entry;
        qint64 songId = 0;
        if (m_libraryController) {
            const PlaylistSongResolution resolution =
                m_libraryController->resolvePlaylistSong(entry);
            songId = resolution.songId;
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
        displayEntry.artist = displayEntry.artist.trimmed();
        row->setData(TitleRole, entryTitle(displayEntry));
        row->setData(ArtistRole, displayEntry.artist);
        QString disc = displayEntry.discId.trimmed();
        if (!disc.isEmpty() && displayEntry.track > 0)
            disc += QStringLiteral("-%1").arg(displayEntry.track, 2, 10, QLatin1Char('0'));
        row->setData(DiscRole, disc);
        row->setData(SongIdRole, songId);
        if (entry.itemId == selectItemId)
            selected = row;
    }
    refreshKeys();
    m_countLabel->setText(QStringLiteral("(%1)").arg(m_items->count()));
    static_cast<PlaylistListWidget*>(m_items)->setHint(m_items->count() > 0
        ? QStringLiteral("Drag songs here from the library, or double-click a song to play it.")
        : QStringLiteral("Drag songs here from the library."));
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
        const bool playing = active && context && context->playlistId == playlistId
            && context->itemId == item->data(ItemIdRole).toLongLong();
        if (playing)
            text.prepend(QStringLiteral("▶ "));
        item->setText(text);
        item->setData(PlayingRole, playing);
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
    const bool confirmed = !m_confirmRemove || (m_removeConfirmation
        ? m_removeConfirmation(this, songText, playlist->name)
        : QMessageBox::question(this, QStringLiteral("Remove Song"),
              QStringLiteral("Remove \"%1\" from \"%2\"? The song stays in your library.")
                  .arg(songText, playlist->name),
              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes);
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
    if (watched == m_items->viewport() && event->type() == QEvent::Resize)
        m_columnHeader->update();
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
        emit interacted();
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
    setMessageText(fallback);
}
