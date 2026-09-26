#include "LibraryView.h"

#include "LibraryController.h"

#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int SongIdRole = Qt::UserRole + 1;
constexpr int ArtistRole = Qt::UserRole + 2;
constexpr int DiscRole = Qt::UserRole + 3;
constexpr int MoreRole = Qt::UserRole + 4;

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

        if (index.data(MoreRole).toBool()) {
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
            index.data(ArtistRole).toString(), Qt::ElideRight, mainRect.width());
        painter->drawText(QRect(mainRect.left(), mainRect.top() + 39, mainRect.width(), 28),
                          Qt::AlignLeft | Qt::AlignVCenter, artist);

        QFont discFont = option.font;
        discFont.setPointSize(13);
        painter->setFont(discFont);
        painter->drawText(rightRect, Qt::AlignRight | Qt::AlignVCenter,
                          index.data(DiscRole).toString());
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

    m_hintLabel = new QLabel(QStringLiteral("Type part of a song name or a singer's name"),
                             m_searchPage);
    m_hintLabel->setAlignment(Qt::AlignCenter);
    QFont hintFont = m_hintLabel->font();
    hintFont.setPointSize(20);
    m_hintLabel->setFont(hintFont);

    m_results = new QListWidget(m_searchPage);
    m_results->setObjectName(QStringLiteral("libraryResults"));
    m_results->setFocusPolicy(Qt::NoFocus);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
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
    m_singButton = makeLibraryButton(QStringLiteral("Sing This Song"), m_searchPage, true);
    m_singButton->setMinimumWidth(240);
    m_singButton->setEnabled(false);
    auto* actions = new QHBoxLayout;
    actions->addWidget(m_changeFolderButton);
    actions->addStretch();
    actions->addWidget(m_singButton);

    auto* searchLayout = new QVBoxLayout(m_searchPage);
    searchLayout->setSpacing(10);
    searchLayout->addWidget(m_searchBox);
    searchLayout->addWidget(m_hintLabel, 1);
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
    connect(m_searchBox, &QLineEdit::textChanged, this, [this] {
        m_messageLabel->clear();
        m_debounce->start();
    });
    connect(m_results, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                m_singButton->setEnabled(current && current->data(SongIdRole).toLongLong() != 0);
            });
    connect(m_results, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { singSelected(); });
    connect(m_backButton, &QPushButton::clicked, this, &LibraryView::backRequested);
    connect(m_chooseFolderButton, &QPushButton::clicked, this, &LibraryView::chooseFolder);
    connect(m_changeFolderButton, &QPushButton::clicked, this, &LibraryView::chooseFolder);
    connect(m_singButton, &QPushButton::clicked, this, &LibraryView::singSelected);
    if (m_controller) {
        connect(m_controller, &LibraryController::stateChanged,
                this, &LibraryView::updateState);
        connect(m_controller, &LibraryController::libraryReady,
                this, &LibraryView::refreshSearch);
    }
    updateState();
}

void LibraryView::activate()
{
    m_messageLabel->clear();
    if (m_controller)
        m_controller->recheckRoot();
    updateState();
    m_searchBox->setFocus(Qt::OtherFocusReason);
}

void LibraryView::showMessage(const QString& message)
{
    m_messageLabel->setText(message);
    m_searchBox->setFocus(Qt::OtherFocusReason);
}

int LibraryView::songResultCount() const
{
    int count = 0;
    for (int row = 0; row < m_results->count(); ++row) {
        if (m_results->item(row)->data(SongIdRole).toLongLong() != 0)
            ++count;
    }
    return count;
}

qint64 LibraryView::selectedSongId() const
{
    const QListWidgetItem* item = m_results->currentItem();
    return item ? item->data(SongIdRole).toLongLong() : 0;
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
    m_results->clear();
    m_singButton->setEnabled(false);
    const QString text = m_searchBox->text().trimmed();
    if (text.isEmpty()) {
        m_results->hide();
        m_hintLabel->show();
        return;
    }
    m_hintLabel->hide();
    m_results->show();
    if (!m_controller || !m_controller->hasActiveRoot())
        return;

    QString error;
    const QList<CatalogueSearchRow> rows = m_controller->search(text, 201, &error);
    if (!error.isEmpty()) {
        showMessage(QStringLiteral("The song library could not be searched."));
        return;
    }
    const int shown = qMin(200, rows.size());
    for (int i = 0; i < shown; ++i) {
        const CatalogueSearchRow& row = rows.at(i);
        QString disc = row.discId;
        if (row.track > 0) {
            if (!disc.isEmpty())
                disc += QStringLiteral("  ");
            disc += QStringLiteral("Track %1").arg(row.track);
        }
        addResult(row.songId, row.displayTitle, row.displayArtist, disc);
        if (row.songId == keepSongId)
            m_results->setCurrentRow(i);
    }
    if (rows.size() > 200) {
        auto* more = new QListWidgetItem(QStringLiteral("More songs match - type more words"),
                                         m_results);
        more->setData(MoreRole, true);
        more->setFlags(Qt::NoItemFlags);
    }
}

void LibraryView::addResult(qint64 songId, const QString& title, const QString& artist,
                            const QString& discAndTrack)
{
    auto* item = new QListWidgetItem(title, m_results);
    item->setData(SongIdRole, songId);
    item->setData(ArtistRole, artist);
    item->setData(DiscRole, discAndTrack);
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
    int row = m_results->currentRow();
    if (row < 0)
        row = delta > 0 ? 0 : songs - 1;
    else
        row = qBound(0, row + delta, songs - 1);
    m_results->setCurrentRow(row);
    m_results->scrollToItem(m_results->item(row));
}

void LibraryView::singSelected()
{
    if (songResultCount() == 0)
        return;
    if (m_results->currentRow() < 0)
        m_results->setCurrentRow(0);
    const qint64 songId = selectedSongId();
    if (songId != 0)
        emit singRequested(songId);
}
