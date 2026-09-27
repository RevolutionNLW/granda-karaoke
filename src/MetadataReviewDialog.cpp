#include "MetadataReviewDialog.h"

#include "LibraryController.h"
#include "LyricsView.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>

namespace {

constexpr int kRowLimit = 2000;

struct FilterChoice {
    const char* label;
    ReviewFilter filter;
};

constexpr FilterChoice kFilters[] = {
    {"Unresolved", ReviewFilter::Unresolved},
    {"Low confidence", ReviewFilter::Low},
    {"Medium confidence", ReviewFilter::Medium},
    {"Conflicts", ReviewFilter::Conflicts},
    {"Manual corrections", ReviewFilter::Manual},
    {"All songs", ReviewFilter::All},
};

QString escaped(const QVariant& value)
{
    return value.toString().toHtmlEscaped();
}

QString row(const QString& label, const QString& value)
{
    return QStringLiteral("<tr><td valign=top><b>%1</b></td><td>%2</td></tr>")
        .arg(label.toHtmlEscaped(), value);
}

QString nameText(const QVariant& artist, const QVariant& title)
{
    const QString a = artist.toString().trimmed();
    const QString t = title.toString().trimmed();
    return (a.isEmpty() ? t : a + QStringLiteral(" — ") + t).toHtmlEscaped();
}

QString provenanceText(const QString& source)
{
    if (source == QLatin1String("disc_rule"))
        return QStringLiteral("file names on the same disc/label");
    if (source == QLatin1String("filename"))
        return QStringLiteral("this file name");
    if (source == QLatin1String("sidecar_track_list"))
        return QStringLiteral("a track list beside the song");
    if (source == QLatin1String("exact_duplicate"))
        return QStringLiteral("an identical copy elsewhere in the collection");
    if (source == QLatin1String("cdg_ocr"))
        return QStringLiteral("the title screen (local text recognition)");
    if (source == QLatin1String("combined_evidence"))
        return QStringLiteral("several sources that agree");
    if (source == QLatin1String("id3"))
        return QStringLiteral("the MP3 tags only");
    if (source == QLatin1String("manual"))
        return QStringLiteral("your correction");
    if (source == QLatin1String("fallback"))
        return QStringLiteral("nothing (disc and track only)");
    return source;
}

QString evidenceHtml(const QVariantMap& item)
{
    const QString source = item.value(QStringLiteral("source")).toString();
    QStringList parts;
    if (source == QLatin1String("exact_duplicate")) {
        for (const QVariant& donorValue : item.value(QStringLiteral("donors")).toList()) {
            const QVariantMap donor = donorValue.toMap();
            parts.append(QStringLiteral("%1 — %2 (%3%4)<br><small>%5</small>")
                             .arg(escaped(donor.value(QStringLiteral("artist"))),
                                  escaped(donor.value(QStringLiteral("title"))),
                                  escaped(donor.value(QStringLiteral("confidence"))),
                                  donor.value(QStringLiteral("audioEqual")).toBool()
                                      ? QStringLiteral(", identical audio")
                                      : QStringLiteral(", identical lyrics only"),
                                  escaped(donor.value(QStringLiteral("path")))));
        }
        return QStringLiteral("<b>Identical copy</b>: ") + parts.join(QStringLiteral("<br>"));
    }
    if (source == QLatin1String("sidecar_track_list")) {
        return QStringLiteral("<b>Track list</b>: %1 (line %2): %3")
            .arg(escaped(item.value(QStringLiteral("path"))),
                 escaped(item.value(QStringLiteral("line"))),
                 item.value(QStringLiteral("fields")).toStringList().join(QStringLiteral(" — "))
                     .toHtmlEscaped());
    }
    if (source == QLatin1String("cdg_ocr")) {
        QString text = QStringLiteral("<b>Title screen</b>: \"%1\"")
                           .arg(escaped(item.value(QStringLiteral("text"))));
        if (item.value(QStringLiteral("validated")).toBool()) {
            text += QStringLiteral(" → %1%2").arg(escaped(item.value(QStringLiteral("title"))),
                item.value(QStringLiteral("corrected")).toBool()
                    ? QStringLiteral(" (spelling matched to a known title)") : QString());
            if (!item.value(QStringLiteral("cueArtist")).toString().isEmpty())
                text += QStringLiteral("; singer shown: \"%1\"")
                            .arg(escaped(item.value(QStringLiteral("cueArtist"))));
        } else {
            text += QStringLiteral(" (unverified title-screen reading: not a title known in this "
                                   "collection, so not used automatically; searchable)");
        }
        text += QStringLiteral("<br><small>engine %1</small>").arg(escaped(item.value(QStringLiteral("engine"))));
        return text;
    }
    return QStringLiteral("<b>%1</b>: %2 — %3 (%4)")
        .arg(provenanceText(source).toHtmlEscaped(), escaped(item.value(QStringLiteral("artist"))),
             escaped(item.value(QStringLiteral("title"))),
             escaped(item.value(QStringLiteral("confidence"))));
}

// The one unconfirmed title-screen reading, if there is exactly one. Several
// different readings are ambiguous: they stay in the evidence only.
QString detectedTitleCandidate(const QVariantMap& evidence)
{
    QStringList readings;
    for (const QVariant& value : evidence.value(QStringLiteral("evidence")).toList()) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("source")).toString() != QLatin1String("cdg_ocr")
            || item.value(QStringLiteral("validated")).toBool())
            continue;
        const QString text = item.value(QStringLiteral("text")).toString().simplified();
        if (!text.isEmpty() && !readings.contains(text))
            readings.append(text);
    }
    return readings.size() == 1 ? readings.first() : QString();
}

} // namespace

MetadataReviewDialog::MetadataReviewDialog(LibraryController* controller, QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
{
    setWindowTitle(QStringLiteral("Library Maintenance — Song Names"));
    resize(theme::px(1200), theme::px(760));

    m_filter = new QComboBox(this);
    for (const FilterChoice& choice : kFilters)
        m_filter->addItem(QString::fromLatin1(choice.label), int(choice.filter));
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Filter by name, file, folder or disc"));
    m_status = new QLabel(this);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Show:"), this));
    top->addWidget(m_filter);
    top->addWidget(m_search, 1);
    top->addWidget(m_status);

    m_table = new QTableWidget(0, 6, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Artist"), QStringLiteral("Title"),
                                        QStringLiteral("Confidence"), QStringLiteral("Source"),
                                        QStringLiteral("File"), QStringLiteral("Label")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    const auto sizeColumns = [this] {
        m_table->setColumnWidth(0, theme::px(160));
        m_table->setColumnWidth(1, theme::px(220));
        m_table->setColumnWidth(2, theme::px(130));
        m_table->setColumnWidth(3, theme::px(120));
    };
    sizeColumns();
    connect(theme::notifier(), &theme::Notifier::changed, this, sizeColumns);

    m_detail = new QTextBrowser(this);
    m_artistEdit = new QLineEdit(this);
    m_titleEdit = new QLineEdit(this);
    m_save = new QPushButton(QStringLiteral("Save Correction"), this);
    m_clear = new QPushButton(QStringLiteral("Revert to Automatic"), this);
    m_clear->setToolTip(QStringLiteral(
        "Removes your correction for this song and shows the automatic name again. "
        "Available only when a correction exists."));
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    auto* form = new QFormLayout;
    m_artistEdit->setPlaceholderText(QStringLiteral("Empty keeps the automatic artist"));
    form->addRow(QStringLiteral("Artist:"), m_artistEdit);
    form->addRow(QStringLiteral("Title:"), m_titleEdit);
    auto* editButtons = new QHBoxLayout;
    editButtons->addWidget(m_save);
    editButtons->addWidget(m_clear);
    editButtons->addStretch();
    // Preview: hear and see the selected song through the main player.
    m_playPreview = new QPushButton(QStringLiteral("Play Preview"), this);
    m_stopPreview = new QPushButton(QStringLiteral("Stop Preview"), this);
    m_previewLabel = new QLabel(this);
    m_previewLabel->setWordWrap(true);
    m_previewView = new LyricsView(this);
    m_previewView->unsetCursor();
    theme::setFixedSize(m_previewView, 300, 216);
    auto* previewButtons = new QVBoxLayout;
    previewButtons->addWidget(m_playPreview);
    previewButtons->addWidget(m_stopPreview);
    previewButtons->addWidget(m_previewLabel);
    previewButtons->addStretch();
    auto* preview = new QHBoxLayout;
    preview->addWidget(m_previewView);
    preview->addLayout(previewButtons, 1);

    // An unconfirmed title-screen reading, offered but never used by itself.
    m_detectedRow = new QWidget(this);
    m_detectedTitle = new QLabel(m_detectedRow);
    m_detectedTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_useDetectedTitle = new QPushButton(QStringLiteral("Use Detected Title"), m_detectedRow);
    m_useDetectedTitle->setToolTip(QStringLiteral(
        "Copies the reading into the Title box for you to check. Nothing is saved."));
    auto* detected = new QHBoxLayout(m_detectedRow);
    detected->setContentsMargins(0, 0, 0, 0);
    detected->addWidget(new QLabel(QStringLiteral("Detected title:"), m_detectedRow));
    detected->addWidget(m_detectedTitle);
    detected->addWidget(new QLabel(QStringLiteral("<i>(unverified title-screen reading)</i>"),
                                   m_detectedRow), 1);
    detected->addWidget(m_useDetectedTitle);
    m_detectedRow->hide();

    auto* detailPane = new QWidget(this);
    auto* detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->addLayout(preview);
    detailLayout->addWidget(m_detail, 1);
    detailLayout->addWidget(m_detectedRow);
    detailLayout->addWidget(new QLabel(
        QStringLiteral("Corrections are kept in the application's own database. "
                       "Song files are never changed."), this));
    detailLayout->addLayout(form);
    detailLayout->addLayout(editButtons);
    detailLayout->addWidget(m_message);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(m_table);
    splitter->addWidget(detailPane);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_reprocess = new QPushButton(QStringLiteral("Reprocess Metadata"), this);
    m_titleScreens = new QCheckBox(QStringLiteral("Also read title screens of unresolved songs (slow)"), this);
    m_titleScreens->setEnabled(m_controller && m_controller->titleScreenOcrAvailable());
    if (!m_titleScreens->isEnabled())
        m_titleScreens->setToolTip(QStringLiteral("No local text recognition on this computer"));
    auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_reprocess);
    bottom->addWidget(m_titleScreens);
    bottom->addStretch();
    bottom->addWidget(closeButton);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    // As everywhere in the app, buttons never react to the keyboard; Enter in
    // an edit box saves the correction instead of pressing a default button.
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
        button->setFocusPolicy(Qt::NoFocus);
    }
    connect(m_artistEdit, &QLineEdit::returnPressed, this, &MetadataReviewDialog::saveCorrection);
    connect(m_titleEdit, &QLineEdit::returnPressed, this, &MetadataReviewDialog::saveCorrection);

    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(250);
    connect(debounce, &QTimer::timeout, this, &MetadataReviewDialog::refresh);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(m_filter, &QComboBox::currentIndexChanged, this, &MetadataReviewDialog::refresh);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MetadataReviewDialog::showDetail);
    connect(m_save, &QPushButton::clicked, this, &MetadataReviewDialog::saveCorrection);
    connect(m_clear, &QPushButton::clicked, this, &MetadataReviewDialog::clearCorrection);
    connect(m_reprocess, &QPushButton::clicked, this, &MetadataReviewDialog::reprocess);
    connect(m_playPreview, &QPushButton::clicked, this, [this] {
        if (const qint64 songId = selectedSongId())
            emit previewRequested(songId);
    });
    connect(m_stopPreview, &QPushButton::clicked, this, &MetadataReviewDialog::previewStopRequested);
    connect(m_useDetectedTitle, &QPushButton::clicked, this, [this] {
        m_titleEdit->setText(m_detectedTitle->property("candidate").toString());
        m_titleEdit->setFocus(Qt::OtherFocusReason);
        m_message->setText(QStringLiteral(
            "Check the title (and add the artist), then click Save Correction."));
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    if (m_controller) {
        connect(m_controller, &LibraryController::catalogueChanged, this, &MetadataReviewDialog::refresh);
        connect(m_controller, &LibraryController::libraryReady, this, &MetadataReviewDialog::refresh);
        connect(m_controller, &LibraryController::stateChanged, this, &MetadataReviewDialog::updateStatus);
    }
    setPreviewState(false, {});
    refresh();
}

void MetadataReviewDialog::setPreviewAvailable(bool available)
{
    m_previewAvailable = available;
    updatePreviewButtons();
}

void MetadataReviewDialog::setPreviewState(bool active, const QString& description)
{
    m_previewActive = active;
    m_previewLabel->setText(active ? QStringLiteral("Previewing: %1").arg(description) : QString());
    if (!active)
        m_previewView->setFrame(QImage());
    updatePreviewButtons();
}

void MetadataReviewDialog::showPreviewFrame(const QImage& frame)
{
    m_previewView->setFrame(frame);
}

void MetadataReviewDialog::showPreviewMessage(const QString& message)
{
    m_previewLabel->setText(message);
}

void MetadataReviewDialog::updatePreviewButtons()
{
    m_playPreview->setEnabled(m_previewAvailable && selectedSongId() != 0);
    m_stopPreview->setEnabled(m_previewAvailable && m_previewActive);
}

qint64 MetadataReviewDialog::selectedSongId() const
{
    const QList<QTableWidgetItem*> items = m_table->selectedItems();
    return items.isEmpty() ? 0 : items.first()->data(Qt::UserRole).toLongLong();
}

bool MetadataReviewDialog::selectSong(qint64 songId)
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        if (m_table->item(i, 0)->data(Qt::UserRole).toLongLong() == songId) {
            m_table->selectRow(i);
            return true;
        }
    }
    return false;
}

void MetadataReviewDialog::refresh()
{
    const qint64 selected = selectedSongId();
    const auto filter = static_cast<ReviewFilter>(m_filter->currentData().toInt());
    QString error;
    const QList<ReviewRow> rows = m_controller
        ? m_controller->reviewList(filter, m_search->text(), kRowLimit, &error) : QList<ReviewRow>();
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(0);
    m_table->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const ReviewRow& row = rows.at(i);
        const QStringList cells = {row.displayArtist, row.displayTitle,
                                   row.confidence + (row.conflict ? QStringLiteral(" · conflict") : QString()),
                                   row.manual ? QStringLiteral("manual") : row.source, row.relPath,
                                   row.label};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setData(Qt::UserRole, row.songId);
            m_table->setItem(i, column, item);
        }
    }
    const qint64 total = m_controller ? m_controller->reviewCount(filter, &error) : 0;
    m_status->setText(error.isEmpty()
        ? (total > rows.size() ? QStringLiteral("%L1 songs (first %L2 shown)").arg(total).arg(rows.size())
                               : QStringLiteral("%L1 songs").arg(rows.size()))
        : QStringLiteral("The song library is unavailable"));
    if (selected == 0 || !selectSong(selected)) {
        if (m_table->rowCount() > 0)
            m_table->selectRow(0);
    }
    showDetail();
    updateStatus();
}

void MetadataReviewDialog::showDetail()
{
    const qint64 songId = selectedSongId();
    m_save->setEnabled(songId != 0);
    m_clear->setEnabled(false);
    m_detectedRow->hide();
    m_detectedTitle->clear();
    m_detectedTitle->setProperty("candidate", QString());
    updatePreviewButtons();
    if (songId == 0 || !m_controller) {
        m_detail->clear();
        m_artistEdit->clear();
        m_titleEdit->clear();
        return;
    }
    QString error;
    const QVariantMap detail = m_controller->reviewDetail(songId, &error);
    if (!error.isEmpty()) {
        m_detail->setPlainText(error);
        return;
    }
    bool manual = false;
    for (const char* key : {"manualArtist", "manualTitle", "manualLabel", "manualSeries",
                            "manualDiscId", "manualTrack"})
        manual = manual || !detail.value(QLatin1String(key)).isNull();
    m_clear->setEnabled(manual);
    // An unresolved song's "Disc ... - Track ..." is not a name to keep.
    const bool unresolved = detail.value(QStringLiteral("confidence")).toString()
        == QLatin1String("unresolved");
    m_artistEdit->setText(detail.value(QStringLiteral("artist")).toString());
    m_titleEdit->setText(unresolved ? QString() : detail.value(QStringLiteral("title")).toString());
    const QVariantMap evidence = detail.value(QStringLiteral("evidence")).toMap();
    const QString candidate = detectedTitleCandidate(evidence);
    if (!candidate.isEmpty()) {
        m_detectedTitle->setText(QStringLiteral("<b>%1</b>").arg(candidate.toHtmlEscaped()));
        m_detectedTitle->setProperty("candidate", candidate);
        m_detectedTitle->setToolTip(QStringLiteral(
            "Unverified title-screen reading: it does not match a title known in this collection"));
        m_detectedRow->show();
    }
    const QVariantMap tags = detail.value(QStringLiteral("tags")).toMap();
    const QVariantMap parsed = detail.value(QStringLiteral("parsed")).toMap();
    QString html = QStringLiteral("<table cellspacing=4>");
    html += row(QStringLiteral("Shown as"), nameText(detail.value(QStringLiteral("artist")),
                                                     detail.value(QStringLiteral("title"))));
    html += row(QStringLiteral("Confidence"), escaped(detail.value(QStringLiteral("confidence"))));
    html += row(QStringLiteral("Named from"),
                provenanceText(detail.value(QStringLiteral("source")).toString()).toHtmlEscaped());
    html += row(QStringLiteral("Automatic proposal"),
                QStringLiteral("%1 (%2, from %3)")
                    .arg(nameText(detail.value(QStringLiteral("autoDisplayArtist")),
                                  detail.value(QStringLiteral("autoTitle"))),
                         escaped(detail.value(QStringLiteral("autoConfidence"))),
                         provenanceText(detail.value(QStringLiteral("autoSource")).toString())
                             .toHtmlEscaped()));
    QStringList trusted;
    if (!detail.value(QStringLiteral("manualArtist")).isNull() || !detail.value(QStringLiteral("manualTitle")).isNull())
        trusted << nameText(detail.value(QStringLiteral("manualArtist")), detail.value(QStringLiteral("manualTitle")));
    for (const auto& [key, text] : {std::pair{"manualLabel", "label"}, std::pair{"manualSeries", "series"},
                                    std::pair{"manualDiscId", "disc"}, std::pair{"manualTrack", "track"}}) {
        if (!detail.value(QLatin1String(key)).isNull())
            trusted << QStringLiteral("%1 %2").arg(QLatin1String(text), escaped(detail.value(QLatin1String(key))));
    }
    html += row(detail.value(QStringLiteral("manualOrigin")).toString() == QLatin1String("import")
                    ? QStringLiteral("Imported metadata") : QStringLiteral("Your correction"),
                manual ? trusted.join(QStringLiteral("; ")) : QStringLiteral("none"));
    const QStringList conflicts = evidence.value(QStringLiteral("conflicts")).toStringList();
    if (!conflicts.isEmpty())
        html += row(QStringLiteral("Conflicts"), conflicts.join(QStringLiteral(", ")).toHtmlEscaped());
    html += row(QStringLiteral("File"), escaped(detail.value(QStringLiteral("mp3RelPath"))));
    html += row(QStringLiteral("Lyrics file"), escaped(detail.value(QStringLiteral("cdgRelPath"))));
    const QString labelText = detail.value(QStringLiteral("label")).toString();
    const QString seriesText = detail.value(QStringLiteral("series")).toString();
    html += row(QStringLiteral("Label / series"), labelText.isEmpty()
        ? QStringLiteral("unknown")
        : QStringLiteral("%1%2 (from the %3)").arg(labelText.toHtmlEscaped(),
              seriesText.isEmpty() ? QString() : QStringLiteral(" / ") + seriesText.toHtmlEscaped(),
              detail.value(QStringLiteral("labelSource")).toString() == QLatin1String("folder")
                  ? QStringLiteral("folder") : QStringLiteral("disc code")));
    html += row(QStringLiteral("Music folder"), escaped(detail.value(QStringLiteral("rootPath"))));
    const QString disc = detail.value(QStringLiteral("discId")).toString();
    const int track = detail.value(QStringLiteral("track")).toInt();
    html += row(QStringLiteral("Disc / track"),
                QStringLiteral("%1 / %2").arg(disc.isEmpty() ? QStringLiteral("—") : disc.toHtmlEscaped(),
                                               track > 0 ? QString::number(track) : QStringLiteral("—")));
    html += row(QStringLiteral("Name fields"),
                parsed.value(QStringLiteral("fields")).toStringList().join(QStringLiteral(" | "))
                    .toHtmlEscaped());
    html += row(QStringLiteral("MP3 tags"),
                QStringLiteral("artist \"%1\", title \"%2\", album \"%3\"")
                    .arg(escaped(tags.value(QStringLiteral("artist"))),
                         escaped(tags.value(QStringLiteral("title"))),
                         escaped(tags.value(QStringLiteral("album")))));
    QStringList evidenceLines;
    const QVariantList items = evidence.value(QStringLiteral("evidence")).toList();
    for (int i = 1; i < items.size(); ++i)  // the first item is the proposal above
        evidenceLines.append(evidenceHtml(items.at(i).toMap()));
    if (!evidenceLines.isEmpty())
        html += row(QStringLiteral("Evidence"), evidenceLines.join(QStringLiteral("<br>")));
    html += row(QStringLiteral("Rules"),
                QStringLiteral("resolver %1, %2").arg(escaped(detail.value(QStringLiteral("resolverVersion"))),
                                                       escaped(evidence.value(QStringLiteral("rule")))));
    html += QStringLiteral("</table>");
    m_detail->setHtml(html);
}

void MetadataReviewDialog::saveCorrection()
{
    const qint64 songId = selectedSongId();
    if (songId == 0 || !m_controller)
        return;
    const QString artist = m_artistEdit->text().simplified();
    const QString title = m_titleEdit->text().simplified();
    if (title.isEmpty()) {
        m_message->setText(QStringLiteral("A correction needs a title."));
        return;
    }
    QString error;
    // An empty artist keeps the automatic artist; the title is always given.
    const std::optional<QString> artistValue = artist.isEmpty() ? std::nullopt
                                                                : std::optional<QString>(artist);
    if (!m_controller->setManualOverride(songId, artistValue, title, &error)) {
        m_message->setText(QStringLiteral("The correction could not be saved: %1").arg(error));
        return;
    }
    m_message->setText(QStringLiteral("Saved. The song now shows as %1 — %2.").arg(artist, title));
    refresh();
    selectSong(songId);
}

void MetadataReviewDialog::clearCorrection()
{
    const qint64 songId = selectedSongId();
    if (songId == 0 || !m_controller)
        return;
    QString error;
    if (!m_controller->clearManualOverride(songId, &error)) {
        m_message->setText(QStringLiteral("The correction could not be removed: %1").arg(error));
        return;
    }
    m_message->setText(QStringLiteral("The automatic name is used again."));
    refresh();
    selectSong(songId);
}

void MetadataReviewDialog::reprocess()
{
    if (!m_controller)
        return;
    if (m_titleScreens->isChecked() && m_titleScreens->isEnabled())
        m_controller->requestMetadataReprocessWithTitleScreens();
    else
        m_controller->requestMetadataReprocess();
    updateStatus();
}

void MetadataReviewDialog::updateStatus()
{
    if (!m_controller)
        return;
    const bool busy = m_controller->isScanning();
    m_reprocess->setEnabled(!busy);
    m_reprocess->setText(busy ? m_controller->progressText() : QStringLiteral("Reprocess Metadata"));
}
