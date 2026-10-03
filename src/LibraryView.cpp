#include "LibraryView.h"

#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "SongKeyPicker.h"
#include "library/SongKeys.h"
#include "ui/Controls.h"
#include "ui/ElidedLabel.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

QPushButton* makeLibraryButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// Paints the song table: one line per karaoke version, the selection (gold in
// the pane in use, grey otherwise), and the now-playing marker, which is
// separate from the selection. A plain model and a painting delegate stay fast
// with tens of thousands of rows.
class SongTableDelegate final : public QStyledItemDelegate {
public:
    SongTableDelegate(QTreeView* view, std::function<bool()> active)
        : QStyledItemDelegate(view)
        , m_view(view)
        , m_active(std::move(active))
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
        using Model = LibraryResultsModel;
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool strong = selected && m_active();
        const QColor background = selected
            ? (strong ? theme::color::selection : theme::color::selectionIdle)
            : (index.row() % 2 && theme::alternateRows() ? theme::color::rowAlternate
                                                          : theme::color::rowBase);
        painter->fillRect(option.rect, background);
        const bool firstVisual = m_view->header()->visualIndex(index.column()) == 0
            || m_view->isFirstColumnSpanned(index.row(), {});
        if (strong && firstVisual)
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), theme::px(3),
                                    option.rect.height()), theme::color::accent);

        QRect text = option.rect.adjusted(theme::px(12), 0, -theme::px(12), 0);
        if (index.data(Model::MoreRole).toBool()) {
            painter->setPen(theme::color::textMuted);
            painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                              index.model()->index(index.row(), Model::SongColumn)
                                  .data().toString());
            painter->restore();
            return;
        }
        if (firstVisual && index.data(Model::PlayingRole).toBool()) {
            const int size = theme::px(12);
            const QRect mark(text.left(), text.center().y() - size / 2, size, size);
            ui::glyphIcon(ui::Glyph::Play, theme::color::accent).paint(painter, mark);
            text.setLeft(text.left() + theme::px(18));
        }
        const int column = index.column();
        const bool secondary = column == Model::LabelColumn || column == Model::DiscColumn
            || column == Model::PlaysColumn || column == Model::KeyColumn;
        painter->setPen(strong ? theme::color::selectionText
                               : secondary ? theme::color::secondaryText : theme::color::text);
        QFont font = option.font;
        if (column == Model::DiscColumn) {
            font = m_mono;
            font.setPixelSize(theme::px(13));
        }
        painter->setFont(font);
        const Qt::Alignment align = (column == Model::PlaysColumn ? Qt::AlignRight : Qt::AlignLeft)
            | Qt::AlignVCenter;
        painter->drawText(text, int(align),
                          painter->fontMetrics().elidedText(index.data().toString(),
                                                            Qt::ElideRight, text.width()));
        painter->restore();
    }

private:
    QTreeView* m_view;
    std::function<bool()> m_active;
    QFont m_mono;
};

} // namespace

LibraryView::LibraryView(LibraryController* controller, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
{
    auto* pane = new QFrame(this);
    pane->setObjectName(QStringLiteral("pane"));

    auto* title = new QLabel(QStringLiteral("Local Song Library"), pane);
    title->setObjectName(QStringLiteral("paneTitle"));
    // The same height as the playlist pane's title row, so the two line up.
    theme::setFixedHeight(title, 26);
    m_statusLabel = new QLabel(pane);
    m_statusLabel->setObjectName(QStringLiteral("paneStatus"));
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* headerFrame = new QFrame(pane);
    headerFrame->setObjectName(QStringLiteral("paneHeader"));
    auto* header = new QHBoxLayout(headerFrame);
    header->setContentsMargins(16, 12, 16, 12);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(m_statusLabel);

    m_content = new QStackedWidget(pane);
    m_setupPage = new QWidget(m_content);
    m_searchPage = new QWidget(m_content);
    m_content->addWidget(m_setupPage);
    m_content->addWidget(m_searchPage);

    m_setupLabel = new QLabel(QStringLiteral("Choose the folder that holds your karaoke songs"),
                              m_setupPage);
    m_setupLabel->setAlignment(Qt::AlignCenter);
    m_setupLabel->setWordWrap(true);
    m_chooseFolderButton = makeLibraryButton(QStringLiteral("Choose Music Folder"), m_setupPage);
    m_chooseFolderButton->setObjectName(QStringLiteral("playButton"));
    auto* setupLayout = new QVBoxLayout(m_setupPage);
    setupLayout->addStretch();
    setupLayout->addWidget(m_setupLabel);
    theme::addSpacing(setupLayout, 12);
    setupLayout->addWidget(m_chooseFolderButton, 0, Qt::AlignHCenter);
    setupLayout->addStretch();

    // The search box and the sort choice beside it. The order applies to
    // browsing and to search results, and is remembered by the library.
    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName(QStringLiteral("searchBar"));
    m_searchBox = new QLineEdit(m_searchBar);
    m_searchBox->setObjectName(QStringLiteral("librarySearchBox"));
    m_searchBox->setPlaceholderText(QStringLiteral("Search songs, artists or disc IDs\u2026"));
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->addAction(ui::glyphIcon(ui::Glyph::Search, theme::color::textMuted),
                           QLineEdit::LeadingPosition);
    m_searchBox->installEventFilter(this);

    m_sortBox = new ui::ComboBox(m_searchBar);
    m_sortBox->setObjectName(QStringLiteral("librarySort"));
    m_sortBox->setFocusPolicy(Qt::NoFocus);
    m_sortBox->setToolTip(QStringLiteral("Order of the songs"));
    const std::pair<const char*, LibrarySort> sorts[] = {
        {"Artist A \xE2\x86\x92 Z", LibrarySort::ArtistAsc},
        {"Artist Z \xE2\x86\x92 A", LibrarySort::ArtistDesc},
        {"Song A \xE2\x86\x92 Z", LibrarySort::TitleAsc},
        {"Song Z \xE2\x86\x92 A", LibrarySort::TitleDesc},
        {"Most Played", LibrarySort::MostPlayed},
        {"Recently Played", LibrarySort::RecentlyPlayed},
        {"Label A \xE2\x86\x92 Z", LibrarySort::LabelAsc},
    };
    for (const auto& [text, sort] : sorts)
        m_sortBox->addItem(QString::fromUtf8(text), int(sort));
    if (m_controller)
        m_sortBox->setCurrentIndex(m_sortBox->findData(int(m_controller->librarySort())));
    auto* sortCaption = new QLabel(QStringLiteral("Sort:"), m_searchBar);
    sortCaption->setObjectName(QStringLiteral("caption"));
    auto* searchRow = new QHBoxLayout(m_searchBar);
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(10);
    searchRow->addWidget(m_searchBox, 1);
    theme::addSpacing(searchRow, 6);
    searchRow->addWidget(sortCaption);
    searchRow->addWidget(m_sortBox);

    m_hintLabel = new ElidedLabel(
        QStringLiteral("Double-click a song to sing it, or drag it to a playlist"), m_searchPage);
    m_hintLabel->setObjectName(QStringLiteral("paneHint"));

    m_results = new QTreeView(m_searchPage);
    m_resultsModel = new LibraryResultsModel(m_results);
    if (m_controller) {
        m_resultsModel->setPlayCountProvider([controller](qint64 songId) {
            return controller->playStats(songId).playCount;
        });
        m_resultsModel->setKeyProvider([controller](qint64 songId) {
            const std::optional<SongKeyInfo> key = controller->songKey(songId);
            return key ? songKeyName(key->shownKeyIndex()) : QString();
        });
    }
    m_results->setModel(m_resultsModel);
    m_results->setObjectName(QStringLiteral("libraryResults"));
    m_results->setFocusPolicy(Qt::NoFocus);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_results->setUniformRowHeights(true);
    m_results->setRootIsDecorated(false);
    m_results->setItemsExpandable(false);
    m_results->setIndentation(0);
    m_results->setAllColumnsShowFocus(true);
    QHeaderView* columns = m_results->header();
    columns->setSectionsClickable(false);
    columns->setSectionsMovable(false);
    columns->setHighlightSections(false);
    columns->setStretchLastSection(false);
    applyColumnWidths();
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        applyColumnWidths();
        m_results->doItemsLayout();
        m_results->viewport()->update();
    });
    columns->setSectionResizeMode(LibraryResultsModel::ArtistColumn, QHeaderView::Stretch);
    columns->setSectionResizeMode(LibraryResultsModel::SongColumn, QHeaderView::Stretch);
    columns->setSectionResizeMode(LibraryResultsModel::LabelColumn, QHeaderView::Fixed);
    columns->setSectionResizeMode(LibraryResultsModel::KeyColumn, QHeaderView::Fixed);
    columns->setSectionResizeMode(LibraryResultsModel::DiscColumn, QHeaderView::Fixed);
    columns->setSectionResizeMode(LibraryResultsModel::PlaysColumn, QHeaderView::Fixed);
    // Artist first, as people look for songs by singer.
    columns->moveSection(columns->visualIndex(LibraryResultsModel::ArtistColumn), 0);
    ui::extendHeaderOverScrollBar(m_results);
    // Clicking a song keeps the keyboard with the search box (never the
    // playlist), so Up/Down/Enter always act on the library.
    m_results->viewport()->installEventFilter(this);
    m_results->setDragEnabled(true);
    m_results->setAcceptDrops(false);
    m_results->setDragDropMode(QAbstractItemView::DragOnly);
    m_results->setDefaultDropAction(Qt::CopyAction);
    m_results->setItemDelegate(new SongTableDelegate(m_results, [this] { return m_active; }));

    m_messageLabel = new QLabel(m_searchPage);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setStyleSheet(theme::dangerStyle());
    // Takes the room left in the footer; never asks for any (the buttons do).
    m_messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_addToPlaylistButton = makeLibraryButton(QStringLiteral("Add to Playlist"), m_searchPage);
    m_addToPlaylistButton->setEnabled(false);
    m_singButton = makeLibraryButton(QStringLiteral("Sing This Song"), m_searchPage);
    m_singButton->setEnabled(false);
    // The song's original key, chosen by hand. A button of its own: the Key
    // cell is only ever shown, so every part of a row behaves the same.
    m_setKeyButton = makeLibraryButton(QStringLiteral("Set Song Key"), m_searchPage);
    m_setKeyButton->setEnabled(false);
    m_setKeyButton->setToolTip(QStringLiteral("Choose the key the selected song's backing track is recorded in"));
    for (QPushButton* button : {m_setKeyButton, m_addToPlaylistButton, m_singButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    auto* footer = new QFrame(m_searchPage);
    footer->setObjectName(QStringLiteral("paneFooter"));
    m_footer = footer;
    footer->installEventFilter(this);
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(16, 8, 12, 8);
    actions->setSpacing(8);
    actions->addWidget(m_hintLabel);
    actions->addWidget(m_messageLabel, 1);
    actions->addWidget(m_setKeyButton);
    actions->addWidget(m_addToPlaylistButton);
    actions->addWidget(m_singButton);

    m_searchLayout = new QVBoxLayout(m_searchPage);
    m_searchLayout->setContentsMargins(0, 0, 0, 0);
    m_searchLayout->setSpacing(0);
    m_searchLayout->addWidget(m_searchBar);
    m_searchLayout->addWidget(m_results, 1);
    m_searchLayout->addWidget(footer);

    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(1, 1, 1, 1);
    paneLayout->setSpacing(0);
    paneLayout->addWidget(headerFrame);
    paneLayout->addWidget(m_content, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pane);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &LibraryView::refreshSearch);
    connect(m_searchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_messageLabel->clear();
        if (text.trimmed() == m_shownQuery) {
            m_debounce->stop();
        } else if (text.trimmed().isEmpty()) {
            m_debounce->stop();
            refreshSearch();
        } else {
            m_debounce->start();
        }
    });
    connect(m_sortBox, &QComboBox::currentIndexChanged, this, [this] {
        if (m_controller)
            m_controller->setLibrarySort(static_cast<LibrarySort>(m_sortBox->currentData().toInt()));
        // A new order starts at its top: the highlighted song is not followed.
        m_results->selectionModel()->clear();
        refreshSearch();
        m_results->scrollToTop();
        updateSelectionActions();
        m_searchBox->setFocus(Qt::OtherFocusReason);
    });
    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this] {
                updateSelectionActions();
                if (selectedSongId() != m_keyErrorSongId)
                    clearKeyError();  // it was about another song
            });
    connect(m_results, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex&) { singSelected(); });
    connect(m_chooseFolderButton, &QPushButton::clicked, this, &LibraryView::chooseFolder);
    for (QPushButton* button : {m_singButton, m_addToPlaylistButton, m_setKeyButton})
        connect(button, &QPushButton::pressed, this, &LibraryView::interacted);
    connect(m_setKeyButton, &QPushButton::clicked, this, [this] { openSongKeyPicker(); });
    connect(m_sortBox, &QComboBox::activated, this, &LibraryView::interacted);
    connect(m_singButton, &QPushButton::clicked, this, &LibraryView::singSelected);
    connect(m_addToPlaylistButton, &QPushButton::clicked, this, [this] {
        const qint64 songId = selectedSongId();
        if (songId != 0 && m_playlistAvailable)
            emit addRequested(songId);
    });
    if (m_controller) {
        connect(m_controller, &LibraryController::stateChanged,
                this, &LibraryView::updateState);
        connect(m_controller, &LibraryController::playStatsChanged,
                m_resultsModel, &LibraryResultsModel::forgetPlayCount);
        connect(m_controller, &LibraryController::songKeysChanged,
                m_resultsModel, &LibraryResultsModel::forgetKeys);
        connect(m_controller, &LibraryController::catalogueChanged,
                this, &LibraryView::refreshSearch);
        connect(m_controller, &LibraryController::libraryReady,
                this, &LibraryView::refreshSearch);
        connect(m_controller, &LibraryController::scanFinished,
                this, [this](const QVariantMap&) { refreshSearch(); });
    }
    updateState();
}

void LibraryView::applyColumnWidths()
{
    QHeaderView* columns = m_results->header();
    columns->setMinimumSectionSize(theme::px(40));
    // Label and Disc ID take a share of the width, within limits, so they stay
    // readable in a wide window and leave Artist and Song room in a narrow one.
    const int width = m_results->viewport()->width();
    const auto share = [width](int percent, int least, int most) {
        return std::clamp(width * percent / 100, theme::px(least), theme::px(most));
    };
    columns->resizeSection(LibraryResultsModel::LabelColumn, share(18, 100, 180));
    columns->resizeSection(LibraryResultsModel::DiscColumn, share(15, 106, 140));
    columns->resizeSection(LibraryResultsModel::PlaysColumn, theme::px(64));
    // Room for the widest key ("G#m", "Bbm") and the caption. The Key column
    // gives way when Artist and Song would be left narrower than Label (a
    // small screen at a large interface size); the player bar still shows
    // the key of the song being sung.
    const int keyWidth = theme::px(52);
    columns->resizeSection(LibraryResultsModel::KeyColumn, keyWidth);
    int fixed = keyWidth;
    for (const int column : {int(LibraryResultsModel::LabelColumn), int(LibraryResultsModel::DiscColumn),
                             int(LibraryResultsModel::PlaysColumn)}) {
        if (!m_results->isColumnHidden(column))
            fixed += columns->sectionSize(column);
    }
    const int label = m_results->isColumnHidden(LibraryResultsModel::LabelColumn)
        ? 0 : columns->sectionSize(LibraryResultsModel::LabelColumn);
    const bool room = (width - fixed) / 2 >= label;
    m_results->setColumnHidden(LibraryResultsModel::KeyColumn,
                               !(m_keyColumnWanted && m_keyColumnAvailable && room));
}

void LibraryView::setKeyColumnAvailable(bool available)
{
    if (available == m_keyColumnAvailable)
        return;
    m_keyColumnAvailable = available;
    applyColumnWidths();
}

void LibraryView::setColumnVisible(int column, bool visible)
{
    if (column == LibraryResultsModel::KeyColumn) {
        m_keyColumnWanted = visible;
        applyColumnWidths();
        return;
    }
    m_results->setColumnHidden(column, !visible);
    applyColumnWidths();
}

void LibraryView::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    m_results->viewport()->update();
}

void LibraryView::setPlayingSongId(qint64 songId)
{
    m_resultsModel->setPlayingSongId(songId);
}

QWidget* LibraryView::takeSearchBar()
{
    m_searchLayout->removeWidget(m_searchBar);
    return m_searchBar;
}

void LibraryView::activate()
{
    m_messageLabel->clear();
    if (m_controller)
        m_controller->recheckRoot();
    updateState();
    if (m_controller && m_controller->hasActiveRoot())
        refreshSearch();
    m_searchBox->setFocus(Qt::OtherFocusReason);
}

void LibraryView::showMessage(const QString& message)
{
    m_messageLabel->setText(message);
    m_searchBox->setFocus(Qt::OtherFocusReason);
}

void LibraryView::showKeyError(qint64 songId, const QString& message)
{
    m_keyError = message;
    m_keyErrorSongId = songId;
    showMessage(message);
}

void LibraryView::clearKeyError()
{
    // Only the key's own message: another one shown since stays.
    if (!m_keyError.isEmpty() && m_messageLabel->text() == m_keyError)
        m_messageLabel->clear();
    m_keyError.clear();
    m_keyErrorSongId = 0;
}

int LibraryView::songResultCount() const
{
    return m_resultsModel->songCount();
}

qint64 LibraryView::selectedSongId() const
{
    return m_results->currentIndex().data(LibraryResultsModel::SongIdRole).toLongLong();
}

void LibraryView::setPlaylistAvailable(bool available)
{
    m_playlistAvailable = available;
    m_addToPlaylistButton->setEnabled(available && selectedSongId() != 0);
}

void LibraryView::updateState()
{
    if (!m_controller || !m_controller->isAvailable()) {
        m_statusLabel->setText(QStringLiteral("Song library is unavailable"));
        m_setupLabel->setText(QStringLiteral(
            "The song library could not be opened. You can still use Open Song."));
        m_chooseFolderButton->hide();
        m_content->setCurrentWidget(m_setupPage);
        m_searchBar->setEnabled(false);
        return;
    }
    m_statusLabel->setText(m_controller->statusText());
    const bool configured = m_controller->hasActiveRoot();
    m_chooseFolderButton->show();
    m_setupLabel->setText(QStringLiteral("Choose the folder that holds your karaoke songs"));
    m_content->setCurrentWidget(configured ? m_searchPage : m_setupPage);
    m_searchBar->setEnabled(configured);
}

void LibraryView::chooseFolder()
{
    const QString folder = m_folderChooser
        ? m_folderChooser(this)
        : QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Music Folder"));
    if (folder.isEmpty()) {
        m_searchBox->setFocus(Qt::OtherFocusReason);
        return;
    }
    QString error;
    if (!m_controller || !m_controller->chooseRoot(folder, &error)) {
        m_content->setCurrentWidget(m_searchPage);
        showMessage(QStringLiteral("The song library could not use that folder."));
        return;
    }
    updateState();
    m_searchBox->setFocus(Qt::OtherFocusReason);
}

void LibraryView::refreshSearch()
{
    const qint64 keepSongId = selectedSongId();
    const QString text = m_searchBox->text().trimmed();
    m_shownQuery = text;
    m_results->show();
    m_hintWanted = text.isEmpty();
    updateHint();
    if (!m_controller || !m_controller->hasActiveRoot()) {
        m_resultsModel->setRows({});
        updateSelectionActions();
        return;
    }

    QString error;
    QList<CatalogueSearchRow> rows = text.isEmpty()
        ? m_controller->browse(&error)
        : m_controller->search(text, 201, &error);
    if (!error.isEmpty()) {
        m_resultsModel->setRows({});
        updateSelectionActions();
        showMessage(QStringLiteral("The song library could not be searched."));
        return;
    }
    const bool showMore = !text.isEmpty() && rows.size() > 200;
    if (showMore)
        rows.resize(200);
    m_resultsModel->setRows(rows, showMore);
    if (showMore)
        m_results->setFirstColumnSpanned(m_resultsModel->songCount(), {}, true);
    const int keepRow = m_resultsModel->rowForSongId(keepSongId);
    m_results->setCurrentIndex(keepRow >= 0 ? m_resultsModel->index(keepRow, 0)
                                            : QModelIndex());
    updateSelectionActions();
}

void LibraryView::updateHint()
{
    // The hint shortens itself as the footer narrows; when only a few letters
    // would be left (a small window at a large interface size) it steps
    // aside, so the buttons always have their full width.
    if (!m_footer) {
        m_hintLabel->setVisible(m_hintWanted);
        return;
    }
    int room = m_footer->contentsRect().width();
    if (const QLayout* layout = m_footer->layout()) {
        const QMargins margins = layout->contentsMargins();
        room -= margins.left() + margins.right();
        for (QPushButton* button : {m_setKeyButton, m_addToPlaylistButton, m_singButton}) {
            if (!button->isHidden())
                room -= button->sizeHint().width() + layout->spacing();
        }
        room -= layout->spacing();  // between the hint and the message
    }
    m_hintLabel->setVisible(m_hintWanted && room >= theme::px(48));
}

bool LibraryView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_footer && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        updateHint();
    if (watched == m_results->viewport() && event->type() == QEvent::Resize)
        applyColumnWidths();
    if (watched == m_results->viewport() && event->type() == QEvent::MouseButtonPress
        && m_searchBox->isEnabled())
        m_searchBox->setFocus(Qt::MouseFocusReason);
    if (watched == m_searchBox && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            // Escape only ever clears the search.
            m_searchBox->clear();
            return true;
        }
        if (key->key() == Qt::Key_Up) {
            moveSelection(-1);
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            moveSelection(1);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            singSelected();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibraryView::moveSelection(int delta)
{
    const int songs = songResultCount();
    if (songs == 0)
        return;
    int row = m_results->currentIndex().row();
    if (row < 0)
        row = delta > 0 ? 0 : songs - 1;
    else
        row = qBound(0, row + delta, songs - 1);
    const QModelIndex index = m_resultsModel->index(row, 0);
    m_results->setCurrentIndex(index);
    m_results->scrollTo(index);
}

void LibraryView::singSelected()
{
    if (songResultCount() == 0)
        return;
    if (!m_results->currentIndex().isValid())
        m_results->setCurrentIndex(m_resultsModel->index(0, 0));
    const qint64 songId = selectedSongId();
    if (songId != 0)
        emit singRequested(songId);
}

void LibraryView::updateSelectionActions()
{
    const bool song = selectedSongId() != 0;
    m_singButton->setEnabled(song);
    m_addToPlaylistButton->setEnabled(song && m_playlistAvailable);
    m_setKeyButton->setEnabled(song && m_controller);
}

SongKeyPicker* LibraryView::openSongKeyPicker()
{
    const qint64 songId = selectedSongId();
    if (songId == 0 || !m_controller)
        return nullptr;
    const QModelIndex row = m_results->currentIndex().siblingAtColumn(0);
    const QString artist = row.data(LibraryResultsModel::ArtistRole).toString().trimmed();
    const QString title = row.data(Qt::DisplayRole).toString().trimmed();
    const QString song = artist.isEmpty() ? title : artist + QStringLiteral(" \u2013 ") + title;
    const std::optional<SongKeyInfo> key = m_controller->songKeyDetails(songId);
    clearKeyError();  // trying again
    auto* picker = new SongKeyPicker(song, key ? key->manualKeyIndex : -1,
                                     key ? key->detectedKeyIndex() : -1, this);
    const auto save = [this, songId](std::optional<int> keyIndex) {
        QString error;
        if (m_controller->setManualOriginalKey(songId, keyIndex, &error))
            clearKeyError();
        else
            showKeyError(songId, QStringLiteral("The song key could not be saved. %1").arg(error));
    };
    connect(picker, &SongKeyPicker::keyChosen, this, [save](int keyIndex) { save(keyIndex); });
    connect(picker, &SongKeyPicker::clearRequested, this, [save] { save(std::nullopt); });
    picker->popUpAt(m_setKeyButton);
    return picker;
}
