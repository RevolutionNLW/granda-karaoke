#include "LibraryView.h"

#include "LibraryController.h"
#include "LibraryResultsModel.h"

#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QPushButton* makeLibraryButton(const QString& text, QWidget* parent, bool mainButton)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumHeight(mainButton ? 76 : 48);
    QFont font = button->font();
    font.setPointSize(mainButton ? 22 : 16);
    font.setBold(mainButton);
    button->setFont(font);
    return button;
}

class SongResultDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(100, 82);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->fillRect(option.rect,
                          selected ? option.palette.highlight()
                                   : option.palette.base());
        const QColor textColor = selected ? option.palette.highlightedText().color()
                                          : option.palette.text().color();
        const QColor secondary = selected ? textColor : option.palette.color(QPalette::Mid);
        const QRect row = option.rect.adjusted(14, 6, -14, -6);
        const int rightWidth = 190;
        const QRect mainRect(row.left(), row.top(), qMax(0, row.width() - rightWidth - 12),
                             row.height());
        const QRect rightRect(mainRect.right() + 12, row.top(), rightWidth, row.height());

        if (index.data(LibraryResultsModel::MoreRole).toBool()) {
            QFont font = option.font;
            font.setPointSize(18);
            painter->setFont(font);
            painter->setPen(secondary);
            painter->drawText(row, Qt::AlignVCenter | Qt::AlignLeft,
                              index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }

        QFont titleFont = option.font;
        titleFont.setPointSize(22);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(textColor);
        const QString title = QFontMetrics(titleFont).elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight, mainRect.width());
        painter->drawText(QRect(mainRect.left(), mainRect.top(), mainRect.width(), 38),
                          Qt::AlignLeft | Qt::AlignVCenter, title);

        QFont artistFont = option.font;
        artistFont.setPointSize(17);
        painter->setFont(artistFont);
        painter->setPen(secondary);
        const QString artist = QFontMetrics(artistFont).elidedText(
            index.data(LibraryResultsModel::ArtistRole).toString(),
            Qt::ElideRight, mainRect.width());
        painter->drawText(QRect(mainRect.left(), mainRect.top() + 39, mainRect.width(), 28),
                          Qt::AlignLeft | Qt::AlignVCenter, artist);

        QFont discFont = option.font;
        discFont.setPointSize(13);
        painter->setFont(discFont);
        painter->drawText(rightRect, Qt::AlignRight | Qt::AlignVCenter,
                          index.data(LibraryResultsModel::DiscRole).toString());
        painter->restore();
    }
};

} // namespace

LibraryView::LibraryView(LibraryController* controller, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
{
    m_backButton = makeLibraryButton(QStringLiteral("Back"), this, false);
    m_backButton->setMinimumWidth(120);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSize(14);
    m_statusLabel->setFont(statusFont);
    auto* header = new QHBoxLayout;
    header->addWidget(m_backButton);
    header->addStretch();
    header->addWidget(m_statusLabel);

    m_content = new QStackedWidget(this);
    m_setupPage = new QWidget(m_content);
    m_searchPage = new QWidget(m_content);
    m_content->addWidget(m_setupPage);
    m_content->addWidget(m_searchPage);

    m_setupLabel = new QLabel(QStringLiteral("Choose the folder that holds your karaoke songs"),
                              m_setupPage);
    m_setupLabel->setAlignment(Qt::AlignCenter);
    m_setupLabel->setWordWrap(true);
    QFont setupFont = m_setupLabel->font();
    setupFont.setPointSize(24);
    m_setupLabel->setFont(setupFont);
    m_chooseFolderButton = makeLibraryButton(QStringLiteral("Choose Music Folder"),
                                             m_setupPage, true);
    auto* setupLayout = new QVBoxLayout(m_setupPage);
    setupLayout->addStretch();
    setupLayout->addWidget(m_setupLabel);
    setupLayout->addSpacing(20);
    setupLayout->addWidget(m_chooseFolderButton, 0, Qt::AlignHCenter);
    setupLayout->addStretch();

    m_searchBox = new QLineEdit(m_searchPage);
    m_searchBox->setObjectName(QStringLiteral("librarySearchBox"));
    m_searchBox->setPlaceholderText(QStringLiteral("Type a song or singer"));
    m_searchBox->setMinimumHeight(62);
    QFont searchFont = m_searchBox->font();
    searchFont.setPointSize(26);
    m_searchBox->setFont(searchFont);
    m_searchBox->installEventFilter(this);

    m_hintLabel = new QLabel(QStringLiteral("Type to search, or scroll to browse"),
                             m_searchPage);
    m_hintLabel->setAlignment(Qt::AlignCenter);
    QFont hintFont = m_hintLabel->font();
    hintFont.setPointSize(20);
    m_hintLabel->setFont(hintFont);

    m_results = new QListView(m_searchPage);
    m_resultsModel = new LibraryResultsModel(m_results);
    m_results->setModel(m_resultsModel);
    m_results->setObjectName(QStringLiteral("libraryResults"));
    m_results->setFocusPolicy(Qt::NoFocus);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_results->setUniformItemSizes(true);
    m_results->setDragEnabled(true);
    m_results->setAcceptDrops(false);
    m_results->setDragDropMode(QAbstractItemView::DragOnly);
    m_results->setDefaultDropAction(Qt::CopyAction);
    m_results->setItemDelegate(new SongResultDelegate(m_results));

    m_messageLabel = new QLabel(m_searchPage);
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
    QFont messageFont = m_messageLabel->font();
    messageFont.setPointSize(18);
    m_messageLabel->setFont(messageFont);

    m_changeFolderButton = makeLibraryButton(QStringLiteral("Change Folder"),
                                             m_searchPage, false);
    m_addToPlaylistButton = makeLibraryButton(QStringLiteral("Add to Playlist"),
                                              m_searchPage, false);
    m_addToPlaylistButton->setEnabled(false);
    m_singButton = makeLibraryButton(QStringLiteral("Sing This Song"), m_searchPage, true);
    m_singButton->setMinimumWidth(240);
    m_singButton->setEnabled(false);
    auto* actions = new QHBoxLayout;
    actions->addWidget(m_changeFolderButton);
    actions->addStretch();
    actions->addWidget(m_addToPlaylistButton);
    actions->addWidget(m_singButton);

    auto* searchLayout = new QVBoxLayout(m_searchPage);
    searchLayout->setSpacing(10);
    searchLayout->addWidget(m_searchBox);
    searchLayout->addWidget(m_hintLabel);
    searchLayout->addWidget(m_results, 1);
    searchLayout->addWidget(m_messageLabel);
    searchLayout->addLayout(actions);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(m_content, 1);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &LibraryView::refreshSearch);
    connect(m_searchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_messageLabel->clear();
        if (text.trimmed().isEmpty()) {
            m_debounce->stop();
            refreshSearch();
        } else {
            m_debounce->start();
        }
    });
    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this] { updateSelectionActions(); });
    connect(m_results, &QListView::doubleClicked, this,
            [this](const QModelIndex&) { singSelected(); });
    connect(m_backButton, &QPushButton::clicked, this, &LibraryView::backRequested);
    connect(m_chooseFolderButton, &QPushButton::clicked, this, &LibraryView::chooseFolder);
    connect(m_changeFolderButton, &QPushButton::clicked, this, &LibraryView::chooseFolder);
    connect(m_singButton, &QPushButton::clicked, this, &LibraryView::singSelected);
    connect(m_addToPlaylistButton, &QPushButton::clicked, this, [this] {
        const qint64 songId = selectedSongId();
        if (songId != 0 && m_playlistAvailable)
            emit addRequested(songId);
    });
    if (m_controller) {
        connect(m_controller, &LibraryController::stateChanged,
                this, &LibraryView::updateState);
        connect(m_controller, &LibraryController::catalogueChanged,
                this, &LibraryView::refreshSearch);
        connect(m_controller, &LibraryController::libraryReady,
                this, &LibraryView::refreshSearch);
        connect(m_controller, &LibraryController::scanFinished,
                this, [this](const QVariantMap&) { refreshSearch(); });
    }
    updateState();
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
        return;
    }
    m_statusLabel->setText(m_controller->statusText());
    const bool configured = m_controller->hasActiveRoot();
    m_chooseFolderButton->show();
    m_setupLabel->setText(QStringLiteral("Choose the folder that holds your karaoke songs"));
    m_content->setCurrentWidget(configured ? m_searchPage : m_setupPage);
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
    m_results->show();
    m_hintLabel->setVisible(text.isEmpty());
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
    const int keepRow = m_resultsModel->rowForSongId(keepSongId);
    m_results->setCurrentIndex(keepRow >= 0 ? m_resultsModel->index(keepRow, 0)
                                            : QModelIndex());
    updateSelectionActions();
}

bool LibraryView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_searchBox && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            emit backRequested();
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
}
