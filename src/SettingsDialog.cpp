#include "SettingsDialog.h"

#include "AppPreferences.h"
#include "AudioOutputs.h"
#include "KaraokePlayer.h"
#include "LibraryController.h"
#include "LibraryView.h"
#include "Logging.h"
#include "SongSettings.h"
#include "playlist/PlaylistStore.h"
#include "ui/Controls.h"
#include "ui/Shortcuts.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSlider>
#include <QStandardPaths>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSysInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <gst/gst.h>

namespace {

constexpr int ActionIdRole = Qt::UserRole + 1;

// Logs how long a piece of Settings work took (fks.timing).
class Timed {
public:
    explicit Timed(const char* what) : m_what(what) { m_timer.start(); }
    ~Timed()
    {
        qCInfo(lcTiming).noquote() << "Settings:" << m_what << m_timer.nsecsElapsed() / 1000000.0 << "ms";
    }

private:
    const char* m_what;
    QElapsedTimer m_timer;
};

QLabel* makeLabel(const QString& text, QWidget* parent, const QString& objectName = {})
{
    auto* label = new QLabel(text, parent);
    if (!objectName.isEmpty())
        label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

QPushButton* makeButton(const QString& text, QWidget* parent, const QString& objectName = {})
{
    auto* button = new QPushButton(text, parent);
    if (!objectName.isEmpty())
        button->setObjectName(objectName);
    button->setAutoDefault(false);
    return button;
}

// A heading that starts a group of settings.
void addSection(QVBoxLayout* layout, const QString& title, QWidget* parent)
{
    if (layout->count() > 0)
        theme::addSpacing(layout, 18);
    layout->addWidget(makeLabel(title, parent, QStringLiteral("settingsSection")));
}

// A quiet explanation under a setting.
void addHint(QVBoxLayout* layout, const QString& text, QWidget* parent)
{
    layout->addWidget(makeLabel(text, parent, QStringLiteral("settingsHint")));
}

// A label on the left and its control(s) on the right.
QHBoxLayout* addRow(QVBoxLayout* layout, const QString& label, QWidget* parent)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    auto* title = makeLabel(label, parent);
    title->setWordWrap(false);
    row->addWidget(title);
    row->addStretch();
    layout->addLayout(row);
    return row;
}

QString sortName(LibrarySort sort)
{
    switch (sort) {
    case LibrarySort::ArtistAsc: return QStringLiteral("Artist A → Z");
    case LibrarySort::ArtistDesc: return QStringLiteral("Artist Z → A");
    case LibrarySort::TitleAsc: return QStringLiteral("Song A → Z");
    case LibrarySort::TitleDesc: return QStringLiteral("Song Z → A");
    case LibrarySort::MostPlayed: return QStringLiteral("Most Played");
    case LibrarySort::RecentlyPlayed: return QStringLiteral("Recently Played");
    case LibrarySort::LabelAsc: return QStringLiteral("Label A → Z");
    }
    return {};
}

} // namespace

SettingsDialog::SettingsDialog(Context context, QWidget* parent)
    : QDialog(parent)
    , m_context(std::move(context))
{
    const Timed timed("construct dialog (total)");
    Q_ASSERT(m_context.preferences);
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("Settings"));
    setModal(true);

    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("settingsNav"));
    theme::setFixedWidth(m_nav, 190);
    m_pageTitle = makeLabel(QString(), this, QStringLiteral("settingsTitle"));
    m_pages = new QStackedWidget(this);

    // Pages are made when first shown, so opening Settings stays quick.
    m_builders = {
        {QStringLiteral("General"), &SettingsDialog::buildGeneral},
        {QStringLiteral("Appearance"), &SettingsDialog::buildAppearance},
        {QStringLiteral("Playback"), &SettingsDialog::buildPlayback},
        {QStringLiteral("Audio"), &SettingsDialog::buildAudio},
        {QStringLiteral("Library"), &SettingsDialog::buildLibrary},
        {QStringLiteral("Playlists"), &SettingsDialog::buildPlaylists},
        {QStringLiteral("Shortcuts"), &SettingsDialog::buildShortcuts},
        {QStringLiteral("Metadata"), &SettingsDialog::buildMetadata},
        {QStringLiteral("Advanced"), &SettingsDialog::buildAdvanced},
        {QStringLiteral("About"), &SettingsDialog::buildAbout},
    };
    for (const auto& [name, builder] : std::as_const(m_builders)) {
        m_nav->addItem(name);
        m_pages->addWidget(new QWidget(m_pages));  // a stand-in until made
    }
    m_built.fill(false, m_builders.size());
    // Library status follows the scan, at most a few times a second and
    // only while its page is shown.
    m_libraryStatusTimer = new QTimer(this);
    m_libraryStatusTimer->setSingleShot(true);
    m_libraryStatusTimer->setInterval(250);
    connect(m_libraryStatusTimer, &QTimer::timeout, this, [this] {
        if (currentPage() == QLatin1String("Library"))
            refreshLibraryStatus();
    });

    auto* close = makeButton(QStringLiteral("Close"), this);
    close->setDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* footer = new QHBoxLayout;
    auto* saved = makeLabel(m_context.preferences->isPersistent()
                                ? QStringLiteral("Changes are saved as you make them.")
                                : QStringLiteral("Settings can't be saved right now: changes last "
                                                 "until the program closes."),
                            this, QStringLiteral("settingsHint"));
    connect(m_context.preferences, &AppPreferences::saveFailed, saved, [saved] {
        saved->setText(QStringLiteral("That change could not be saved, so it was not made. "
                                      "Please try again."));
        saved->setStyleSheet(theme::dangerStyle());
    });
    // Wraps only when the window is too narrow for one line.
    footer->addWidget(saved, 1);
    footer->addWidget(close);

    auto* right = new QVBoxLayout;
    right->setContentsMargins(22, 18, 22, 16);
    right->setSpacing(12);
    right->addWidget(m_pageTitle);
    right->addWidget(m_pages, 1);
    right->addLayout(footer);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_nav);
    layout->addLayout(right, 1);

    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
        if (lcTiming().isInfoEnabled()) {
            auto* shown = new QElapsedTimer;
            shown->start();
            const QString name = m_nav->item(row) ? m_nav->item(row)->text() : QString();
            QTimer::singleShot(0, this, [shown, name] {
                qCInfo(lcTiming).noquote() << "Settings: show page" << name
                                           << shown->nsecsElapsed() / 1000000.0 << "ms (until idle)";
                delete shown;
            });
        }
        ensureBuilt(row);
        m_pages->setCurrentIndex(row);
        m_pageTitle->setText(m_nav->item(row) ? m_nav->item(row)->text() : QString());
        const QString page = currentPage();
        if (page == QLatin1String("Library"))
            refreshLibraryStatus();
        else if (page == QLatin1String("Metadata")) {
            refreshMetadataCounts();
            refreshSongKeyStatus();
        }
    });
    m_nav->setCurrentRow(0);
    connect(m_context.preferences, &AppPreferences::changed, this, [this] { refreshControls(); });
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] { refreshScale(); });
    refreshControls();

    theme::setMinimumWidth(this, 760);
    theme::setMinimumHeight(this, 520);
    theme::rescale(this);
    fitToScreen();
    connect(theme::notifier(), &theme::Notifier::changed, this, &SettingsDialog::fitToScreen);
}

void SettingsDialog::fitToScreen()
{
    const Timed timed("fit to screen");
    // Never larger than the screen, even at 150%: the pages scroll instead.
    const QScreen* screen = parentWidget() ? parentWidget()->screen() : this->screen();
    const QSize room = screen ? screen->availableGeometry().size() * 9 / 10 : QSize(4000, 4000);
    setMinimumSize(minimumSize().boundedTo(room));
    resize(QSize(theme::px(900), theme::px(640)).boundedTo(room).expandedTo(minimumSize()));
}

void SettingsDialog::ensureBuilt(int index)
{
    if (index < 0 || index >= m_builders.size() || m_built.at(index))
        return;
    m_built[index] = true;
    m_buildingIndex = index;
    const qsizetype firstNew = m_refreshers.size();
    (this->*m_builders.at(index).second)();
    // The new page's controls show the stored settings, at the current scale.
    for (qsizetype i = firstNew; i < m_refreshers.size(); ++i)
        m_refreshers.at(i)();
    theme::rescale(m_pages->widget(index));
}

int SettingsDialog::pageIndex(const QString& name) const
{
    for (int i = 0; i < m_builders.size(); ++i) {
        if (m_builders.at(i).first == name)
            return i;
    }
    return -1;
}

QWidget* SettingsDialog::addPage(const QString& name, QVBoxLayout** content)
{
    Q_UNUSED(name)
    auto* scroll = new QScrollArea(m_pages);
    scroll->setObjectName(QStringLiteral("settingsPage"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("settingsPageBody"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 12, 0);
    layout->setSpacing(8);
    scroll->setWidget(page);
    // In place of the stand-in.
    QWidget* standIn = m_pages->widget(m_buildingIndex);
    m_pages->insertWidget(m_buildingIndex, scroll);
    m_pages->removeWidget(standIn);
    standIn->deleteLater();
    *content = layout;
    return page;
}

QStringList SettingsDialog::pageNames() const
{
    QStringList names;
    for (const auto& [name, builder] : m_builders)
        names.append(name);
    return names;
}

void SettingsDialog::showPage(const QString& name)
{
    const int row = pageIndex(name);
    if (row >= 0)
        m_nav->setCurrentRow(row);
}

QLabel* SettingsDialog::scaleValueLabel() { ensureBuilt(pageIndex(QStringLiteral("Appearance"))); return m_scaleValue; }
QPushButton* SettingsDialog::scaleDownButton() { ensureBuilt(pageIndex(QStringLiteral("Appearance"))); return m_scaleDown; }
QPushButton* SettingsDialog::scaleUpButton() { ensureBuilt(pageIndex(QStringLiteral("Appearance"))); return m_scaleUp; }
QPushButton* SettingsDialog::scaleResetButton() { ensureBuilt(pageIndex(QStringLiteral("Appearance"))); return m_scaleReset; }
QLabel* SettingsDialog::scaleLimitedLabel() { ensureBuilt(pageIndex(QStringLiteral("Appearance"))); return m_scaleLimited; }
QTreeWidget* SettingsDialog::shortcutList() { ensureBuilt(pageIndex(QStringLiteral("Shortcuts"))); return m_shortcutList; }
QLineEdit* SettingsDialog::shortcutFilter() { ensureBuilt(pageIndex(QStringLiteral("Shortcuts"))); return m_shortcutFilter; }
QKeySequenceEdit* SettingsDialog::shortcutEditor() { ensureBuilt(pageIndex(QStringLiteral("Shortcuts"))); return m_shortcutEditor; }
QLabel* SettingsDialog::shortcutMessage() { ensureBuilt(pageIndex(QStringLiteral("Shortcuts"))); return m_shortcutMessage; }
QLabel* SettingsDialog::metadataCounts() { ensureBuilt(pageIndex(QStringLiteral("Metadata"))); return m_metadataCounts; }
QCheckBox* SettingsDialog::songKeysCheckBox() { ensureBuilt(pageIndex(QStringLiteral("Metadata"))); return m_songKeys; }
QLabel* SettingsDialog::songKeyStatus() { ensureBuilt(pageIndex(QStringLiteral("Metadata"))); return m_songKeyStatus; }

QString SettingsDialog::currentPage() const
{
    return m_nav->currentItem() ? m_nav->currentItem()->text() : QString();
}

void SettingsDialog::refreshControls()
{
    const Timed timed("refresh all controls");
    for (const auto& refresh : std::as_const(m_refreshers))
        refresh();
}

// ---- General -------------------------------------------------------------

QWidget* SettingsDialog::buildGeneral()
{
    const Timed timed("build General");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("General"), &layout);
    AppPreferences* prefs = m_context.preferences;
    const auto flag = [this, prefs, page, layout](const QString& text, const QString& key,
                                                   bool fallback) {
        auto* box = new QCheckBox(text, page);
        layout->addWidget(box);
        connect(box, &QCheckBox::toggled, prefs, [prefs, key](bool on) { prefs->setFlag(key, on); });
        m_refreshers.append([box, prefs, key, fallback] {
            const QSignalBlocker blocker(box);
            box->setChecked(prefs->flag(key, fallback));
        });
        return box;
    };

    addSection(layout, QStringLiteral("Starting up"), page);
    flag(QStringLiteral("Show the splash screen"), pref::ShowSplash, true);
    flag(QStringLiteral("Start in full screen"), pref::StartFullscreen, true);
    QCheckBox* remember = flag(QStringLiteral("Remember the window's size and position"),
                               pref::RememberWindow, true);
    // Only matters when the program does not start in full screen.
    m_refreshers.append([remember, prefs] {
        remember->setEnabled(!prefs->flag(pref::StartFullscreen, true));
    });
    addHint(layout, QStringLiteral("Used when the program does not start in full screen. "
                                   "Takes effect the next time it starts."), page);
    addSection(layout, QStringLiteral("Leaving"), page);
    flag(QStringLiteral("Ask before exiting"), pref::ConfirmExit, false);
    auto* quit = makeButton(QStringLiteral("Quit Application"), page, QStringLiteral("ghostButton"));
    connect(quit, &QPushButton::clicked, this, [this] {
        accept();
        if (m_context.quit)
            m_context.quit();
    });
    layout->addWidget(quit, 0, Qt::AlignLeft);
    addHint(layout, QStringLiteral("Closes Frankie's Karaoke Studio, as the window's close button does."), page);

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore General Defaults"), page, QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [prefs] {
        for (const QString& key : {pref::ShowSplash, pref::StartFullscreen, pref::RememberWindow,
                                   pref::ConfirmExit})
            prefs->reset(key);
    });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    return page;
}

// ---- Appearance ----------------------------------------------------------

QWidget* SettingsDialog::buildAppearance()
{
    const Timed timed("build Appearance");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Appearance"), &layout);
    AppPreferences* prefs = m_context.preferences;

    addSection(layout, QStringLiteral("Interface size"), page);
    QHBoxLayout* scaleRow = addRow(layout, QStringLiteral("Size of text, buttons and lists"), page);
    m_scaleDown = makeButton(QStringLiteral("−"), page, QStringLiteral("stepButton"));
    m_scaleValue = makeLabel(QString(), page, QStringLiteral("settingValue"));
    m_scaleValue->setAlignment(Qt::AlignCenter);
    m_scaleValue->setWordWrap(false);
    m_scaleUp = makeButton(QStringLiteral("+"), page, QStringLiteral("stepButton"));
    m_scaleReset = makeButton(QStringLiteral("Reset to 100%"), page, QStringLiteral("linkButton"));
    scaleRow->addWidget(m_scaleDown);
    scaleRow->addWidget(m_scaleValue);
    scaleRow->addWidget(m_scaleUp);
    scaleRow->addWidget(m_scaleReset);
    const auto setScale = [prefs](int percent) {
        percent = std::clamp(percent, theme::kMinScalePercent, theme::kMaxScalePercent);
        if (percent == 100)
            prefs->reset(pref::ScalePercent);
        else
            prefs->setNumber(pref::ScalePercent, percent);
    };
    connect(m_scaleDown, &QPushButton::clicked, this, [setScale] {
        setScale(theme::scalePercent() - theme::kScaleStepPercent);
    });
    connect(m_scaleUp, &QPushButton::clicked, this, [setScale] {
        setScale(theme::scalePercent() + theme::kScaleStepPercent);
    });
    connect(m_scaleReset, &QPushButton::clicked, this, [setScale] { setScale(100); });
    m_refreshers.append([this] { refreshScale(); });
    addHint(layout, QStringLiteral("Makes the whole program bigger or smaller, straight away. "
                                   "The karaoke lyrics always fill the screen."), page);
    m_scaleLimited = makeLabel(QStringLiteral("This is the largest size that fits this screen."),
                               page, QStringLiteral("settingsHint"));
    m_scaleLimited->setVisible(false);
    layout->addWidget(m_scaleLimited);

    addSection(layout, QStringLiteral("Colours"), page);
    QHBoxLayout* themeRow = addRow(layout, QStringLiteral("Theme"), page);
    themeRow->addWidget(makeLabel(QStringLiteral("Dark"), page, QStringLiteral("settingsHint")));

    addSection(layout, QStringLiteral("Song lists"), page);
    auto* comfortable = new QRadioButton(QStringLiteral("Comfortable rows"), page);
    auto* compact = new QRadioButton(QStringLiteral("Compact rows (more songs on screen)"), page);
    auto* density = new QButtonGroup(page);
    density->addButton(comfortable);
    density->addButton(compact);
    layout->addWidget(comfortable);
    layout->addWidget(compact);
    connect(compact, &QRadioButton::toggled, prefs, [prefs](bool on) {
        if (on)
            prefs->setFlag(pref::CompactRows, true);
        else
            prefs->reset(pref::CompactRows);
    });
    m_refreshers.append([prefs, comfortable, compact] {
        const QSignalBlocker a(comfortable);
        const QSignalBlocker b(compact);
        const bool isCompact = prefs->flag(pref::CompactRows, false);
        compact->setChecked(isCompact);
        comfortable->setChecked(!isCompact);
    });
    const auto flag = [this, prefs, page, layout](const QString& text, const QString& key) {
        auto* box = new QCheckBox(text, page);
        layout->addWidget(box);
        connect(box, &QCheckBox::toggled, prefs, [prefs, key](bool on) { prefs->setFlag(key, on); });
        m_refreshers.append([box, prefs, key] {
            const QSignalBlocker blocker(box);
            box->setChecked(prefs->flag(key, true));
        });
    };
    flag(QStringLiteral("Striped rows"), pref::AlternateRows);
    addSection(layout, QStringLiteral("Library columns"), page);
    flag(QStringLiteral("Show the Label column"), pref::ShowLabelColumn);
    flag(QStringLiteral("Show the Plays column"), pref::ShowPlaysColumn);
    flag(QStringLiteral("Show the Key column"), pref::ShowKeyColumn);

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore Appearance Defaults"), page,
                               QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [prefs] {
        for (const QString& key : {pref::ScalePercent, pref::CompactRows, pref::AlternateRows,
                                   pref::ShowLabelColumn, pref::ShowPlaysColumn, pref::ShowKeyColumn})
            prefs->reset(key);
    });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    return page;
}

void SettingsDialog::refreshScale()
{
    if (!m_scaleValue)
        return;  // the Appearance page is not made yet
    const int percent = theme::scalePercent();
    m_scaleValue->setText(QStringLiteral("%1%").arg(percent));
    m_scaleDown->setEnabled(percent > theme::kMinScalePercent);
    m_scaleUp->setEnabled(percent < std::min(theme::kMaxScalePercent, theme::scaleLimitPercent()));
    m_scaleReset->setEnabled(percent != 100 || theme::chosenScalePercent() != 100);
    // Bigger would not fit this screen (Windows display scaling counts too).
    m_scaleLimited->setVisible(theme::scaleLimitPercent() < theme::kMaxScalePercent
                               && percent >= theme::scaleLimitPercent());
}

// ---- Playback ------------------------------------------------------------

QWidget* SettingsDialog::buildPlayback()
{
    const Timed timed("build Playback");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Playback"), &layout);
    AppPreferences* prefs = m_context.preferences;

    addSection(layout, QStringLiteral("Autoplay"), page);
    QHBoxLayout* autoplayRow = addRow(layout, QStringLiteral("When the program starts"), page);
    auto* autoplay = new ui::ComboBox(page);
    autoplay->addItem(QStringLiteral("Keep it as it was"), QStringLiteral("remember"));
    autoplay->addItem(QStringLiteral("Turn Autoplay on"), QStringLiteral("on"));
    autoplay->addItem(QStringLiteral("Turn Autoplay off"), QStringLiteral("off"));
    autoplayRow->addWidget(autoplay);
    connect(autoplay, &QComboBox::activated, prefs, [prefs, autoplay] {
        const QString value = autoplay->currentData().toString();
        if (value == QLatin1String("remember"))
            prefs->reset(pref::AutoplayAtStartup);
        else
            prefs->setText(pref::AutoplayAtStartup, value);
    });
    m_refreshers.append([prefs, autoplay] {
        const QSignalBlocker blocker(autoplay);
        autoplay->setCurrentIndex(qMax(0, autoplay->findData(
            prefs->text(pref::AutoplayAtStartup, QStringLiteral("remember")))));
    });
    addHint(layout, QStringLiteral("Autoplay only ever moves on through a playlist."), page);

    addSection(layout, QStringLiteral("Songs sung for the first time"), page);
    const auto stepper = [this, prefs, page, layout](const QString& title, const QString& key,
                                                      int fallback, int minimum, int maximum,
                                                      int step, bool percent) {
        QHBoxLayout* row = addRow(layout, title, page);
        auto* down = makeButton(QStringLiteral("−"), page, QStringLiteral("stepButton"));
        auto* value = makeLabel(QString(), page, QStringLiteral("settingValue"));
        value->setAlignment(Qt::AlignCenter);
        value->setWordWrap(false);
        auto* up = makeButton(QStringLiteral("+"), page, QStringLiteral("stepButton"));
        row->addWidget(down);
        row->addWidget(value);
        row->addWidget(up);
        const auto change = [prefs, key, fallback, minimum, maximum](int delta) {
            const int next = std::clamp(prefs->number(key, fallback) + delta, minimum, maximum);
            if (next == fallback)
                prefs->reset(key);
            else
                prefs->setNumber(key, next);
        };
        connect(down, &QPushButton::clicked, this, [change, step] { change(-step); });
        connect(up, &QPushButton::clicked, this, [change, step] { change(step); });
        m_refreshers.append([=] {
            const int current = prefs->number(key, fallback);
            value->setText(percent ? QStringLiteral("%1%").arg(current)
                                   : current > 0 ? QStringLiteral("+%1").arg(current)
                                                 : QString::number(current));
            down->setEnabled(current > minimum);
            up->setEnabled(current < maximum);
        });
    };
    stepper(QStringLiteral("Key"), pref::DefaultKey, 0, kMinKey, kMaxKey, kKeyStep, false);
    stepper(QStringLiteral("Tempo"), pref::DefaultTempo, 100, kMinTempo, kMaxTempo, kTempoStep, true);
    addHint(layout, QStringLiteral("Songs sung before keep their own Key and Tempo."), page);

    addSection(layout, QStringLiteral("During a song"), page);
    const auto flag = [this, prefs, page, layout](const QString& text, const QString& key) {
        auto* box = new QCheckBox(text, page);
        layout->addWidget(box);
        connect(box, &QCheckBox::toggled, prefs, [prefs, key](bool on) { prefs->setFlag(key, on); });
        m_refreshers.append([box, prefs, key] {
            const QSignalBlocker blocker(box);
            box->setChecked(prefs->flag(key, true));
        });
    };
    flag(QStringLiteral("Go back to the controls when a song ends"), pref::ReturnHomeAtEnd);
    addHint(layout, QStringLiteral("If this is off, the lyrics stay on screen until Escape is pressed."), page);
    flag(QStringLiteral("Keep the screen awake while a song plays"), pref::KeepDisplayAwake);

    addHint(layout, QStringLiteral("Forgetting every song's Key and Tempo is on the Advanced page."), page);

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore Playback Defaults"), page,
                               QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [prefs] {
        for (const QString& key : {pref::AutoplayAtStartup, pref::DefaultKey, pref::DefaultTempo,
                                   pref::ReturnHomeAtEnd, pref::KeepDisplayAwake})
            prefs->reset(key);
    });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    return page;
}

void SettingsDialog::forgetAllSongSettings()
{
    if (!m_context.songSettings)
        return;
    const bool confirmed = m_forget
        ? m_forget()
        : confirmForgetAll();
    if (!confirmed)
        return;
    QString backup;
    QString error;
    if (m_context.songSettings->forgetAll(&backup, &error)) {
        m_forgetMessage->setText(backup.isEmpty()
            ? QStringLiteral("Done: every song now starts at its original key and speed.")
            : QStringLiteral("Done. The old settings were kept in %1").arg(backup));
    } else {
        m_forgetMessage->setText(error);
    }
}

// ---- Library -------------------------------------------------------------

QWidget* SettingsDialog::buildLibrary()
{
    const Timed timed("build Library");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Library"), &layout);
    LibraryController* controller = m_context.libraryController;

    addSection(layout, QStringLiteral("Music folder"), page);
    m_folderLabel = makeLabel(QString(), page, QStringLiteral("settingsPath"));
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_folderLabel);
    auto* folderButtons = new QHBoxLayout;
    folderButtons->setSpacing(8);
    auto* change = makeButton(QStringLiteral("Change…"), page, QStringLiteral("ghostButton"));
    auto* rescan = makeButton(QStringLiteral("Rescan"), page, QStringLiteral("ghostButton"));
    folderButtons->addWidget(change);
    folderButtons->addWidget(rescan);
    folderButtons->addStretch();
    layout->addLayout(folderButtons);
    m_libraryStatus = makeLabel(QString(), page);
    m_lastScan = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(m_libraryStatus);
    layout->addWidget(m_lastScan);
    addHint(layout, QStringLiteral("The music folder is only ever read: nothing in it is "
                                   "renamed, changed or deleted."), page);
    change->setEnabled(m_context.libraryView != nullptr);
    rescan->setEnabled(controller != nullptr);
    connect(change, &QPushButton::clicked, this, [this] {
        if (m_context.libraryView)
            m_context.libraryView->chooseFolder();
        refreshLibraryStatus();
    });
    connect(rescan, &QPushButton::clicked, this, [this, controller] {
        controller->requestRefreshScan();
        refreshLibraryStatus();
    });
    if (controller) {
        connect(controller, &LibraryController::stateChanged, m_libraryStatusTimer,
                [this] { if (!m_libraryStatusTimer->isActive()) m_libraryStatusTimer->start(); });
        connect(controller, &LibraryController::scanFinished, m_libraryStatusTimer,
                [this] { m_libraryStatusTimer->start(); });
    }

    addSection(layout, QStringLiteral("Order of songs"), page);
    QHBoxLayout* sortRow = addRow(layout, QStringLiteral("Library order"), page);
    m_sortChoice = new ui::ComboBox(page);
    for (const LibrarySort sort : {LibrarySort::ArtistAsc, LibrarySort::ArtistDesc,
                                   LibrarySort::TitleAsc, LibrarySort::TitleDesc,
                                   LibrarySort::MostPlayed, LibrarySort::RecentlyPlayed,
                                   LibrarySort::LabelAsc})
        m_sortChoice->addItem(sortName(sort), int(sort));
    sortRow->addWidget(m_sortChoice);
    m_sortChoice->setEnabled(m_context.libraryView != nullptr);
    // The same choice as the Sort list above the library (it is remembered).
    const auto chooseSort = [this](int sort) {
        if (!m_context.libraryView)
            return;
        QComboBox* main = m_context.libraryView->sortBox();
        main->setCurrentIndex(main->findData(sort));
    };
    connect(m_sortChoice, &QComboBox::activated, this, [this, chooseSort] {
        chooseSort(m_sortChoice->currentData().toInt());
    });
    if (m_context.libraryView) {
        connect(m_context.libraryView->sortBox(), &QComboBox::currentIndexChanged, this,
                [this] { refreshLibraryStatus(); });
    }

    addSection(layout, QStringLiteral("Songs outside the library"), page);
    auto* openFile = makeButton(QStringLiteral("Open a Song File…"), page, QStringLiteral("ghostButton"));
    connect(openFile, &QPushButton::clicked, this, [this] {
        accept();
        if (m_context.openSongFile)
            m_context.openSongFile();
    });
    layout->addWidget(openFile, 0, Qt::AlignLeft);

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore Library Display Defaults"), page,
                               QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [chooseSort] { chooseSort(int(LibrarySort::ArtistAsc)); });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    m_refreshers.append([this] { refreshLibraryStatus(); });
    return page;
}

void SettingsDialog::refreshLibraryStatus()
{
    const Timed timed("library status");
    LibraryController* controller = m_context.libraryController;
    if (!controller || !controller->isAvailable()) {
        m_folderLabel->setText(QStringLiteral("The song library is unavailable."));
        m_libraryStatus->clear();
        m_lastScan->clear();
        return;
    }
    m_folderLabel->setText(controller->hasActiveRoot() ? controller->activeRoot().path
                                                       : QStringLiteral("No music folder chosen yet."));
    QString status = controller->statusText();
    if (controller->isScanning())
        status += status.isEmpty() ? QStringLiteral("Checking the music folder…")
                                   : QStringLiteral(" — checking the music folder…");
    m_libraryStatus->setText(status);
    const qint64 scanned = controller->lastScanCompletedMs();
    m_lastScan->setText(scanned > 0
        ? QStringLiteral("Last full check: %1").arg(
              QLocale().toString(QDateTime::fromMSecsSinceEpoch(scanned), QLocale::ShortFormat))
        : QString());
    if (m_context.libraryView) {
        const QSignalBlocker blocker(m_sortChoice);
        m_sortChoice->setCurrentIndex(m_sortChoice->findData(
            m_context.libraryView->sortBox()->currentData()));
    }
}

// ---- Playlists -----------------------------------------------------------

QWidget* SettingsDialog::buildPlaylists()
{
    const Timed timed("build Playlists");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Playlists"), &layout);
    AppPreferences* prefs = m_context.preferences;
    PlaylistStore* store = m_context.playlistStore;

    addSection(layout, QStringLiteral("Starting up"), page);
    QHBoxLayout* startRow = addRow(layout, QStringLiteral("Playlist shown when the program starts"), page);
    auto* startup = new ui::ComboBox(page);
    startRow->addWidget(startup);
    connect(startup, &QComboBox::activated, prefs, [prefs, startup] {
        const QString value = startup->currentData().toString();
        if (value == QLatin1String("last"))
            prefs->reset(pref::StartupPlaylist);
        else
            prefs->setText(pref::StartupPlaylist, value);
    });
    m_refreshers.append([prefs, store, startup] {
        const QSignalBlocker blocker(startup);
        startup->clear();
        startup->addItem(QStringLiteral("The one used last"), QStringLiteral("last"));
        if (store && store->isOpen()) {
            for (const PlaylistInfo& playlist : store->playlists())
                startup->addItem(playlist.name, QString::number(playlist.id));
        }
        const int index = startup->findData(prefs->text(pref::StartupPlaylist, QStringLiteral("last")));
        startup->setCurrentIndex(qMax(0, index));
    });

    addSection(layout, QStringLiteral("Removing songs"), page);
    auto* confirm = new QCheckBox(QStringLiteral("Ask before removing a song from a playlist"), page);
    layout->addWidget(confirm);
    connect(confirm, &QCheckBox::toggled, prefs, [prefs](bool on) {
        if (on)
            prefs->reset(pref::ConfirmRemoveSong);
        else
            prefs->setFlag(pref::ConfirmRemoveSong, false);
    });
    m_refreshers.append([prefs, confirm] {
        const QSignalBlocker blocker(confirm);
        confirm->setChecked(prefs->flag(pref::ConfirmRemoveSong, true));
    });
    addHint(layout, QStringLiteral("Deleting a whole playlist always asks first. Removing a song "
                                   "never deletes it from the library or the music folder."), page);
    addHint(layout, QStringLiteral("Autoplay is set on the Playback page and in the player bar."), page);

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore Playlist Defaults"), page, QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [prefs] {
        prefs->reset(pref::StartupPlaylist);
        prefs->reset(pref::ConfirmRemoveSong);
    });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    return page;
}

// ---- Shortcuts -----------------------------------------------------------

QWidget* SettingsDialog::buildShortcuts()
{
    const Timed timed("build Shortcuts");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Shortcuts"), &layout);
    AppPreferences* prefs = m_context.preferences;

    m_shortcutFilter = new QLineEdit(page);
    m_shortcutFilter->setObjectName(QStringLiteral("shortcutFilter"));
    m_shortcutFilter->setPlaceholderText(QStringLiteral("Search shortcuts…"));
    m_shortcutFilter->setClearButtonEnabled(true);
    layout->addWidget(m_shortcutFilter);

    m_shortcutList = new QTreeWidget(page);
    m_shortcutList->setObjectName(QStringLiteral("shortcutList"));
    m_shortcutList->setColumnCount(2);
    m_shortcutList->setHeaderLabels({QStringLiteral("ACTION"), QStringLiteral("SHORTCUT")});
    m_shortcutList->setRootIsDecorated(false);
    m_shortcutList->setIndentation(0);
    m_shortcutList->setUniformRowHeights(true);
    m_shortcutList->setAlternatingRowColors(true);
    m_shortcutList->header()->setStretchLastSection(false);
    m_shortcutList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_shortcutList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_shortcutList->header()->setSectionsClickable(false);
    ui::extendHeaderOverScrollBar(m_shortcutList);
    theme::setMinimumHeight(m_shortcutList, 260);
    layout->addWidget(m_shortcutList, 1);

    auto* editor = new QFrame(page);
    editor->setObjectName(QStringLiteral("shortcutEditorBar"));
    auto* editorLayout = new QHBoxLayout(editor);
    editorLayout->setContentsMargins(0, 4, 0, 0);
    editorLayout->setSpacing(8);
    m_shortcutEditorTitle = makeLabel(QString(), editor);
    m_shortcutEditorTitle->setWordWrap(false);
    m_shortcutEditor = new QKeySequenceEdit(editor);
    m_shortcutEditor->setObjectName(QStringLiteral("shortcutEditor"));
    m_shortcutEditor->setMaximumSequenceLength(1);
    theme::setMinimumWidth(m_shortcutEditor, 170);
    m_shortcutClear = makeButton(QStringLiteral("None"), editor, QStringLiteral("ghostButton"));
    m_shortcutClear->setToolTip(QStringLiteral("Give this action no shortcut"));
    m_shortcutReset = makeButton(QStringLiteral("Default"), editor, QStringLiteral("ghostButton"));
    m_shortcutReset->setToolTip(QStringLiteral("Go back to this action's usual shortcut"));
    editorLayout->addWidget(m_shortcutEditorTitle, 1);
    editorLayout->addWidget(m_shortcutEditor);
    editorLayout->addWidget(m_shortcutClear);
    editorLayout->addWidget(m_shortcutReset);
    layout->addWidget(editor);
    m_shortcutMessage = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(m_shortcutMessage);
    addHint(layout, QStringLiteral("Choose an action, click in the box and press the keys. Escape "
                                   "and Enter always work during karaoke and cannot be changed. "
                                   "Letters and Space never act while you type in a text box."), page);

    auto* resetAll = makeButton(QStringLiteral("Reset All Shortcuts"), page, QStringLiteral("ghostButton"));
    layout->addWidget(resetAll, 0, Qt::AlignLeft);

    connect(m_shortcutFilter, &QLineEdit::textChanged, this, [this] { refreshShortcutList(); });
    connect(m_shortcutList, &QTreeWidget::currentItemChanged, this, [this] { loadShortcutEditor(); });
    connect(m_shortcutEditor, &QKeySequenceEdit::editingFinished, this, &SettingsDialog::applyShortcutEditor);
    connect(m_shortcutClear, &QPushButton::clicked, this, [this] {
        const QString id = selectedShortcutId();
        if (!id.isEmpty())
            assignShortcut(id, {});
    });
    connect(m_shortcutReset, &QPushButton::clicked, this, [this] {
        const QString id = selectedShortcutId();
        const shortcuts::Action* action = shortcuts::find(id);
        if (action && !m_conflictPrompt)
            changeShortcut(id, action->defaultKeys);
    });
    connect(resetAll, &QPushButton::clicked, this, [this, prefs] {
        shortcuts::resetAll(*prefs);
        m_shortcutMessage->setText(QStringLiteral("Every shortcut is back to its usual keys."));
    });
    m_refreshers.append([this] { refreshShortcutList(); });
    return page;
}

void SettingsDialog::refreshShortcutList()
{
    const Timed timed("shortcut list");
    const QString keep = selectedShortcutId();
    const QString filter = m_shortcutFilter->text().trimmed();
    const auto matches = [&filter](const QStringList& texts) {
        if (filter.isEmpty())
            return true;
        for (const QString& text : texts) {
            if (text.contains(filter, Qt::CaseInsensitive))
                return true;
        }
        return false;
    };
    const QSignalBlocker blocker(m_shortcutList);
    m_shortcutList->clear();
    QTreeWidgetItem* selected = nullptr;
    // The two built-in keys come first and cannot be selected for editing.
    const QList<QPair<QString, QString>> fixed = {
        {QStringLiteral("Back to the controls (during a song)"), QStringLiteral("Escape")},
        {QStringLiteral("Back to the lyrics (during a song)"), QStringLiteral("Enter")},
    };
    for (const auto& [title, key] : fixed) {
        if (!matches({title, key, QStringLiteral("Built in")}))
            continue;
        auto* item = new QTreeWidgetItem(m_shortcutList, {title, key + QStringLiteral("  (built in)")});
        item->setFlags(Qt::ItemIsEnabled);
        item->setForeground(1, theme::color::textMuted);
    }
    for (const shortcuts::Action& action : shortcuts::actions()) {
        const QString keys = shortcuts::display(shortcuts::current(*m_context.preferences, action.id));
        const QString title = action.group + QStringLiteral(": ") + action.title;
        if (!matches({title, keys}))
            continue;
        auto* item = new QTreeWidgetItem(m_shortcutList, {title, keys.isEmpty() ? QStringLiteral("—") : keys});
        item->setData(0, ActionIdRole, action.id);
        if (action.id == keep)
            selected = item;
    }
    if (selected)
        m_shortcutList->setCurrentItem(selected);
    loadShortcutEditor();
}

QString SettingsDialog::selectedShortcutId() const
{
    const QTreeWidgetItem* item = m_shortcutList ? m_shortcutList->currentItem() : nullptr;
    return item ? item->data(0, ActionIdRole).toString() : QString();
}

void SettingsDialog::loadShortcutEditor()
{
    const QString id = selectedShortcutId();
    const shortcuts::Action* action = shortcuts::find(id);
    const QSignalBlocker blocker(m_shortcutEditor);
    m_shortcutEditor->setEnabled(action != nullptr);
    m_shortcutClear->setEnabled(action != nullptr);
    m_shortcutReset->setEnabled(action != nullptr);
    m_shortcutEditorTitle->setText(action ? action->title : QStringLiteral("Choose an action above"));
    m_shortcutEditor->setKeySequence(action ? shortcuts::current(*m_context.preferences, id)
                                            : QKeySequence());
}

void SettingsDialog::applyShortcutEditor()
{
    // The shortcut box reports "editing finished" when the keys are let go
    // AND again whenever it loses focus (which the question below causes).
    // Only one change is ever in hand, and keys already in effect need nothing.
    if (m_conflictPrompt)
        return;
    const QString id = selectedShortcutId();
    if (id.isEmpty())
        return;
    const QKeySequence keys = m_shortcutEditor->keySequence();
    if (keys == shortcuts::current(*m_context.preferences, id))
        return;
    changeShortcut(id, keys);
}

void SettingsDialog::changeShortcut(const QString& id, const QKeySequence& keys)
{
    if (m_conflictPrompt)
        return;
    AppPreferences& prefs = *m_context.preferences;
    const shortcuts::Action* action = shortcuts::find(id);
    if (!action)
        return;
    const QString problem = shortcuts::problem(keys);
    if (!problem.isEmpty()) {
        m_shortcutMessage->setText(problem);
        loadShortcutEditor();  // back to what it was
        return;
    }
    const QString otherId = shortcuts::conflict(prefs, keys, id);
    if (otherId.isEmpty()) {
        QString message;
        assignShortcut(id, keys, &message);
        m_shortcutMessage->setText(message);
        loadShortcutEditor();
        return;
    }
    // Ask, without a nested event loop: the answer arrives in finished().
    // Cancel, Escape and closing the window all leave everything as it was;
    // only Reassign changes anything.
    const shortcuts::Action* other = shortcuts::find(otherId);
    const QString shown = shortcuts::display(keys);
    auto* prompt = new QMessageBox(QMessageBox::Question, QStringLiteral("Shortcut Already Used"),
                                   QStringLiteral("%1 is already assigned to “%2”.\n\n"
                                                  "Reassign it to “%3”?")
                                       .arg(shown, other->title, action->title),
                                   QMessageBox::NoButton, this);
    prompt->setObjectName(QStringLiteral("shortcutConflictPrompt"));
    QPushButton* cancel = prompt->addButton(QMessageBox::Cancel);
    QPushButton* reassign = prompt->addButton(QStringLiteral("Reassign"), QMessageBox::AcceptRole);
    prompt->setDefaultButton(cancel);
    prompt->setEscapeButton(cancel);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    m_conflictPrompt = prompt;
    const QHash<QString, QString> storedBefore = shortcuts::storedChoices(prefs);
    connect(prompt, &QMessageBox::finished, this,
            [this, prompt, reassign, id, otherId, keys, shown, storedBefore] {
        bool move = prompt->clickedButton() == reassign;
        m_conflictPrompt = nullptr;
        const shortcuts::Action* from = shortcuts::find(otherId);
        // Only what was asked about is changed: if any shortcut changed in
        // the meantime, nothing is.
        AppPreferences& current = *m_context.preferences;
        if (move && (shortcuts::storedChoices(current) != storedBefore
                     || shortcuts::conflict(current, keys, id) != otherId)) {
            move = false;
            m_shortcutMessage->setText(QStringLiteral("The shortcuts changed meanwhile; nothing was reassigned."));
            loadShortcutEditor();
            return;
        }
        if (move) {
            QString message;
            moveShortcut(otherId, id, keys, &message);
            m_shortcutMessage->setText(message);
        } else {
            m_shortcutMessage->setText(
                QStringLiteral("%1 is still assigned to “%2”.").arg(shown, from->title));
        }
        loadShortcutEditor();  // shows the outcome (or the previous keys)
    });
    theme::rescale(prompt);
    prompt->open();
}

bool SettingsDialog::moveShortcut(const QString& fromId, const QString& toId,
                                  const QKeySequence& keys, QString* message)
{
    // One change: both are saved together, or neither is.
    const QHash<QString, QString> before = shortcuts::storedChoices(*m_context.preferences);
    if (!shortcuts::move(*m_context.preferences, fromId, toId, keys)) {
        if (message)
            *message = shortcuts::storedChoices(*m_context.preferences) == before
                ? QStringLiteral("The shortcut could not be saved; nothing was changed.")
                : QStringLiteral("The shortcut could not be saved. Please check the list below.");
        return false;
    }
    if (message)
        *message = QStringLiteral("%1 is now assigned to “%2”; “%3” has none.")
                       .arg(shortcuts::display(keys), shortcuts::find(toId)->title,
                            shortcuts::find(fromId)->title);
    return true;
}

bool SettingsDialog::assignShortcut(const QString& id, const QKeySequence& keys, QString* message)
{
    AppPreferences& prefs = *m_context.preferences;
    const shortcuts::Action* action = shortcuts::find(id);
    const auto say = [message, this](const QString& text) {
        if (message)
            *message = text;
        if (m_shortcutMessage)
            m_shortcutMessage->setText(text);
    };
    if (!action)
        return false;
    if (m_conflictPrompt) {
        say(QStringLiteral("Please answer the open question first."));
        return false;
    }
    if (keys == shortcuts::current(prefs, id)) {
        say(QString());
        return true;
    }
    const QString problem = shortcuts::problem(keys);
    if (!problem.isEmpty()) {
        say(problem);
        return false;
    }
    const QString otherId = shortcuts::conflict(prefs, keys, id);
    if (!otherId.isEmpty()) {
        // Direct calls never open a question: the conflict is moved only if
        // the confirmation (see setReassignConfirmation) says so.
        const shortcuts::Action* other = shortcuts::find(otherId);
        const QString shown = shortcuts::display(keys);
        if (!m_reassign || !m_reassign(shown, other->title, action->title)) {
            say(QStringLiteral("%1 is still assigned to “%2”.").arg(shown, other->title));
            return false;
        }
        QString moved;
        const bool ok = moveShortcut(otherId, id, keys, &moved);
        say(moved);
        return ok;
    }
    if (!shortcuts::assign(prefs, id, keys)) {
        say(QStringLiteral("The shortcut could not be saved; nothing was changed."));
        return false;
    }
    say(keys.isEmpty() ? QStringLiteral("“%1” has no shortcut now.").arg(action->title)
                       : QString());
    return true;
}

void SettingsDialog::done(int result)
{
    // Leaving Settings while a question is open cancels it: nothing changes.
    if (m_conflictPrompt) {
        QMessageBox* prompt = m_conflictPrompt;
        m_conflictPrompt = nullptr;
        prompt->disconnect(this);
        prompt->deleteLater();
    }
    QDialog::done(result);
}

// ---- Audio ---------------------------------------------------------------

QWidget* SettingsDialog::buildAudio()
{
    const Timed timed("build Audio");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Audio"), &layout);
    AppPreferences* prefs = m_context.preferences;

    addSection(layout, QStringLiteral("Sound output"), page);
    QHBoxLayout* outputRow = addRow(layout, QStringLiteral("Play the music through"), page);
    auto* output = new ui::ComboBox(page);
    output->setObjectName(QStringLiteral("audioOutput"));
    theme::setMinimumWidth(output, 240);
    auto* refresh = makeButton(QStringLiteral("Refresh"), page, QStringLiteral("linkButton"));
    refresh->setToolTip(QStringLiteral("Look again for speakers and headphones"));
    outputRow->addWidget(output);
    outputRow->addWidget(refresh);
    addHint(layout, QStringLiteral("A new choice starts with the next song, so the song playing "
                                   "now is never interrupted."), page);
    auto* remember = new QCheckBox(QStringLiteral("Use this output every time the program starts"), page);
    layout->addWidget(remember);
    // Looking for outputs can take a moment, so it is done when the page is
    // made and on Refresh only; other changes just show the current choice.
    const auto showChoice = [prefs, output] {
        const QSignalBlocker blocker(output);
        const QString chosen = prefs->text(pref::AudioOutput);
        int index = output->findData(chosen);
        if (index < 0 && !chosen.isEmpty()) {
            output->addItem(QStringLiteral("%1 (not connected)").arg(
                                chosen.startsWith(QLatin1String("id:")) ? QStringLiteral("The chosen output")
                                                                        : chosen),
                            chosen);
            index = output->count() - 1;
        }
        output->setCurrentIndex(qMax(0, index));
    };
    // The system is asked on a worker thread (it can take a moment); the
    // list is kept for the session, and Refresh asks again.
    audio::OutputFinder* finder = audio::OutputFinder::instance();
    const auto fillOutputs = [output, showChoice, finder] {
        const Timed fillTimed("audio device list (fill)");
        {
            const QSignalBlocker blocker(output);
            output->clear();
            output->addItem(QStringLiteral("The computer's usual output"), QString());
            for (const audio::Output& device : finder->outputs())
                output->addItem(device.label, device.id);
        }
        showChoice();
    };
    fillOutputs();
    auto* searching = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    outputRow->addWidget(searching);
    const auto showSearching = [searching, refresh](bool busy) {
        searching->setText(busy ? QStringLiteral("Looking for speakers and headphones\u2026") : QString());
        refresh->setEnabled(!busy);
    };
    connect(finder, &audio::OutputFinder::outputsChanged, this, fillOutputs);
    connect(finder, &audio::OutputFinder::searchingChanged, this, showSearching);
    showSearching(finder->isSearching());
    if (!finder->hasList())
        finder->refresh();
    connect(output, &QComboBox::activated, prefs, [prefs, output] {
        const QString name = output->currentData().toString();
        if (name.isEmpty())
            prefs->reset(pref::AudioOutput);
        else
            prefs->setText(pref::AudioOutput, name);
    });
    connect(refresh, &QPushButton::clicked, finder, &audio::OutputFinder::refresh);
    connect(remember, &QCheckBox::toggled, prefs, [prefs](bool on) {
        if (on)
            prefs->reset(pref::RememberAudioOutput);
        else
            prefs->setFlag(pref::RememberAudioOutput, false);
    });
    m_refreshers.append([prefs, remember, showChoice] {
        showChoice();
        const QSignalBlocker blocker(remember);
        remember->setChecked(prefs->flag(pref::RememberAudioOutput, true));
    });

    addSection(layout, QStringLiteral("Volume"), page);
    QHBoxLayout* volumeRow = addRow(layout, QStringLiteral("Music volume"), page);
    auto* volume = new QSlider(Qt::Horizontal, page);
    volume->setObjectName(QStringLiteral("volumeSlider"));
    volume->setRange(0, 100);
    volume->setSingleStep(5);
    volume->setPageStep(10);
    theme::setMinimumWidth(volume, 220);
    // Room for the whole knob at every scale.
    theme::setMinimumHeight(volume, 22);
    auto* volumeValue = makeLabel(QString(), page, QStringLiteral("settingValue"));
    volumeValue->setWordWrap(false);
    volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeRow->addWidget(volume);
    volumeRow->addWidget(volumeValue);
    // Heard at once while dragging; saved when let go (or at once for a
    // click or a key).
    const auto saveVolume = [prefs, volume] {
        if (volume->value() == 100)
            prefs->reset(pref::Volume);
        else
            prefs->setNumber(pref::Volume, volume->value());
    };
    connect(volume, &QSlider::valueChanged, this, [this, volume, volumeValue, saveVolume](int value) {
        volumeValue->setText(QStringLiteral("%1%").arg(value));
        if (m_context.player)
            m_context.player->setVolumePercent(value);
        if (!volume->isSliderDown())
            saveVolume();
    });
    connect(volume, &QSlider::sliderReleased, this, saveVolume);
    m_refreshers.append([prefs, volume, volumeValue] {
        if (volume->isSliderDown())
            return;
        const int value = prefs->number(pref::Volume, 100);
        const QSignalBlocker blocker(volume);
        volume->setValue(value);
        volumeValue->setText(QStringLiteral("%1%").arg(value));
    });
    addHint(layout, QStringLiteral("The program's own music volume. The computer's volume still applies."), page);

    addSection(layout, QStringLiteral("Check the sound"), page);
    auto* test = makeButton(QStringLiteral("Play a Test Sound"), page, QStringLiteral("ghostButton"));
    layout->addWidget(test, 0, Qt::AlignLeft);
    auto* testMessage = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(testMessage);
    connect(test, &QPushButton::clicked, this, [this, prefs, test, testMessage] {
        if (songInProgress()) {
            testMessage->setText(QStringLiteral("Stop the song first to hear the test sound."));
            return;
        }
        QString error;
        audio::TestTone* tone = audio::TestTone::play(prefs->text(pref::AudioOutput),
                                                      prefs->number(pref::Volume, 100), &error);
        if (!tone) {
            testMessage->setText(error);
            return;
        }
        testMessage->setText(QStringLiteral("You should hear a short, soft tone."));
        test->setEnabled(false);  // one at a time
        connect(tone, &audio::TestTone::finished, test, [test] { test->setEnabled(true); });
    });

    layout->addStretch();
    auto* restore = makeButton(QStringLiteral("Restore Audio Defaults"), page, QStringLiteral("ghostButton"));
    connect(restore, &QPushButton::clicked, this, [prefs] {
        for (const QString& key : {pref::AudioOutput, pref::RememberAudioOutput, pref::Volume})
            prefs->reset(key);
    });
    layout->addWidget(restore, 0, Qt::AlignLeft);
    return page;
}

// ---- Metadata ------------------------------------------------------------

QWidget* SettingsDialog::buildMetadata()
{
    const Timed timed("build Metadata");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Metadata"), &layout);
    LibraryController* controller = m_context.libraryController;
    const bool available = controller && controller->isAvailable();

    addHint(layout, QStringLiteral("Library maintenance: how well the songs are named, and tools "
                                   "for checking and correcting them."), page);
    addSection(layout, QStringLiteral("Song names"), page);
    m_metadataCounts = makeLabel(QString(), page);
    layout->addWidget(m_metadataCounts);
    auto* review = makeButton(QStringLiteral("Open Needs Review…"), page, QStringLiteral("ghostButton"));
    review->setEnabled(available && m_context.openNeedsReview != nullptr);
    connect(review, &QPushButton::clicked, this, [this] {
        accept();
        if (m_context.openNeedsReview)
            m_context.openNeedsReview();
    });
    layout->addWidget(review, 0, Qt::AlignLeft);
    addHint(layout, QStringLiteral("Corrections made there are kept separately from the catalogue "
                                   "and survive rescans and rebuilds."), page);

    addSection(layout, QStringLiteral("Work the names out again"), page);
    auto* titleScreens = new QCheckBox(
        QStringLiteral("Also read the title screens of unnamed songs (slow; needs the music folder)"), page);
    titleScreens->setVisible(available && controller->titleScreenOcrAvailable());
    layout->addWidget(titleScreens);
    auto* reprocess = makeButton(QStringLiteral("Reprocess Song Names"), page, QStringLiteral("ghostButton"));
    reprocess->setEnabled(available);
    layout->addWidget(reprocess, 0, Qt::AlignLeft);
    m_reprocessStatus = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(m_reprocessStatus);
    connect(reprocess, &QPushButton::clicked, this, [this, controller, titleScreens] {
        if (titleScreens->isVisible() && titleScreens->isChecked())
            controller->requestMetadataReprocessWithTitleScreens();
        else
            controller->requestMetadataReprocess();
        refreshMetadataStatus();
    });

    addSection(layout, QStringLiteral("Song keys"), page);
    addHint(layout, QStringLiteral("Works out each song's musical key from its music, for the Key "
                                   "column. Every song is read from the music drive, about half a "
                                   "second to a second each on a typical computer, so a large "
                                   "library takes many hours (perhaps 8 to 15 for 50,000 songs, "
                                   "depending on the computer and drive); it is done a "
                                   "little at a time, pauses while a song plays and carries on next "
                                   "time. Nothing is written to the music drive. A key is shown only "
                                   "when it is clear; some songs will stay blank."), page);
    m_songKeys = new QCheckBox(QStringLiteral("Work out song keys in the background"), page);
    m_songKeys->setEnabled(available);
    layout->addWidget(m_songKeys);
    {
        AppPreferences* prefs = m_context.preferences;
        connect(m_songKeys, &QCheckBox::toggled, prefs, [prefs](bool on) {
            prefs->setFlag(pref::AnalyseSongKeys, on);
        });
        QCheckBox* box = m_songKeys;
        m_refreshers.append([box, prefs] {
            const QSignalBlocker blocker(box);
            box->setChecked(prefs->flag(pref::AnalyseSongKeys, false));
        });
    }
    m_songKeyStatus = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    m_songKeyStatus->setWordWrap(true);
    layout->addWidget(m_songKeyStatus);
    if (controller) {
        connect(controller, &LibraryController::songKeySummaryChanged, this, [this] {
            if (currentPage() == QLatin1String("Metadata"))
                refreshSongKeyStatus();
        });
    }

    addSection(layout, QStringLiteral("Title-screen text"), page);
    addHint(layout, QStringLiteral("Words read from songs' title screens can be saved to a file and "
                                   "read on another computer (the files are matched by content, "
                                   "not by name)."), page);
    auto* transfer = new QHBoxLayout;
    transfer->setSpacing(8);
    auto* exportButton = makeButton(QStringLiteral("Save to a File…"), page, QStringLiteral("ghostButton"));
    auto* importButton = makeButton(QStringLiteral("Read from a File…"), page, QStringLiteral("ghostButton"));
    exportButton->setEnabled(available);
    importButton->setEnabled(available);
    transfer->addWidget(exportButton);
    transfer->addWidget(importButton);
    transfer->addStretch();
    layout->addLayout(transfer);
    auto* transferMessage = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(transferMessage);
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    connect(exportButton, &QPushButton::clicked, this, [this, controller, transferMessage, documents] {
        if (songInProgress()) {
            transferMessage->setText(QStringLiteral("Stop the song first: this can take a moment."));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Title-Screen Text"),
            QDir(documents).filePath(QStringLiteral("title-screens.json")),
            QStringLiteral("Title-screen text (*.json)"));
        if (path.isEmpty())
            return;
        QString summary;
        QString error;
        transferMessage->setText(controller->exportTitleScreens(path, &summary, &error) ? summary : error);
    });
    connect(importButton, &QPushButton::clicked, this, [this, controller, transferMessage, documents] {
        if (songInProgress()) {
            transferMessage->setText(QStringLiteral("Stop the song first: this can take a moment."));
            return;
        }
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Read Title-Screen Text"), documents,
            QStringLiteral("Title-screen text (*.json)"));
        if (path.isEmpty())
            return;
        if (QFileInfo(path).size() > 64 * 1024 * 1024) {
            transferMessage->setText(QStringLiteral("That file is too large to be title-screen text."));
            return;
        }
        QString summary;
        QString error;
        transferMessage->setText(controller->importTitleScreens(path, &summary, &error) ? summary : error);
    });

    if (controller) {
        connect(controller, &LibraryController::progressChanged, this, [this] {
            if (currentPage() == QLatin1String("Metadata"))
                refreshMetadataStatus();
        });
        connect(controller, &LibraryController::stateChanged, this, [this] {
            if (currentPage() == QLatin1String("Metadata"))
                refreshMetadataStatus();
        });
        // Counted on a worker thread; the page never waits for it.
        connect(controller, &LibraryController::reviewSummaryReady, this,
                [this](const ReviewSummary& summary) { showMetadataCounts(summary); });
        connect(controller, &LibraryController::reviewSummaryFailed, this, [this] {
            if (m_metadataCounts)
                m_metadataCounts->setText(QStringLiteral("The counts could not be worked out just now. "
                                                         "Open this page again to try again."));
        });
        connect(controller, &LibraryController::catalogueChanged, this, [this] {
            if (currentPage() == QLatin1String("Metadata"))
                refreshMetadataCounts();
        });
    }
    refreshMetadataStatus();
    layout->addStretch();
    return page;
}

void SettingsDialog::refreshSongKeyStatus()
{
    LibraryController* controller = m_context.libraryController;
    if (!m_songKeyStatus || !controller || !controller->isAvailable())
        return;
    // Counted on a worker thread the first time; the page never waits.
    if (!controller->songKeySummary())
        controller->requestSongKeySummary();
    m_songKeyStatus->setText(controller->songKeyStatusText());
}

void SettingsDialog::refreshMetadataCounts()
{
    const Timed timed("metadata counts (request)");
    LibraryController* controller = m_context.libraryController;
    if (!m_metadataCounts)
        return;
    if (!controller || !controller->isAvailable()) {
        m_metadataCounts->setText(QStringLiteral("The song library is unavailable."));
        return;
    }
    if (const auto summary = controller->cachedReviewSummary()) {
        showMetadataCounts(*summary);
        return;
    }
    m_metadataCounts->setText(QStringLiteral("Counting\u2026"));
    controller->requestReviewSummary();
}

void SettingsDialog::showMetadataCounts(const ReviewSummary& summary)
{
    if (!m_metadataCounts)
        return;
    m_metadataCounts->setText(
        QStringLiteral("Sure: %L1 \u2022 Probable: %L2 \u2022 Doubtful: %L3 \u2022 Unknown: %L4\n"
                       "Disagreements: %L5 \u2022 Corrected by hand: %L6")
            .arg(summary.high).arg(summary.medium).arg(summary.low).arg(summary.unresolved)
            .arg(summary.conflicts).arg(summary.manual));
}

void SettingsDialog::refreshMetadataStatus()
{
    LibraryController* controller = m_context.libraryController;
    if (!controller || !m_reprocessStatus)
        return;
    if (controller->isReprocessing()) {
        m_reprocessStatus->setText(QStringLiteral("Working the song names out again\u2026 This can take "
                                                  "a few minutes and pauses while a song plays."));
    } else if (controller->isScanning()) {
        m_reprocessStatus->setText(QStringLiteral("The music folder is being checked; reprocessing "
                                                  "will start after it."));
    } else {
        m_reprocessStatus->setText(QStringLiteral("Reprocessing pauses while a song plays."));
    }
}

// ---- Advanced ------------------------------------------------------------

QWidget* SettingsDialog::buildAdvanced()
{
    const Timed timed("build Advanced");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("Advanced"), &layout);

    addSection(layout, QStringLiteral("Where the program keeps its files"), page);
    QString dataFolder;
    for (const auto& [title, path] : std::as_const(m_context.dataLocations)) {
        if (dataFolder.isEmpty())
            dataFolder = path;
        layout->addWidget(makeLabel(title, page));
        auto* label = makeLabel(path.isEmpty() ? QStringLiteral("(not available)") : path, page,
                                QStringLiteral("settingsPath"));
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
    if (m_context.dataLocations.isEmpty())
        addHint(layout, QStringLiteral("Not available here."), page);
    auto* openFolder = makeButton(QStringLiteral("Show the Data Folder"), page, QStringLiteral("ghostButton"));
    openFolder->setEnabled(!dataFolder.isEmpty());
    connect(openFolder, &QPushButton::clicked, this, [dataFolder] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataFolder));
    });
    layout->addWidget(openFolder, 0, Qt::AlignLeft);
    addHint(layout, QStringLiteral("The song catalogue can be rebuilt from the music folder at any "
                                   "time. Playlists, corrections, Key and Tempo memory and play "
                                   "history are kept in their own files and are never rebuilt."), page);

    addSection(layout, QStringLiteral("Remembered Key and Tempo"), page);
    addHint(layout, QStringLiteral("Every song keeps the Key and Tempo last used for it. Forgetting "
                                   "them sends every song back to its original key and speed."), page);
    auto* forget = makeButton(QStringLiteral("Forget Every Song's Key and Tempo…"), page,
                              QStringLiteral("dangerButton"));
    connect(forget, &QPushButton::clicked, this, &SettingsDialog::forgetAllSongSettings);
    forget->setEnabled(m_context.songSettings != nullptr);
    layout->addWidget(forget, 0, Qt::AlignLeft);
    m_forgetMessage = makeLabel(QString(), page, QStringLiteral("settingsHint"));
    layout->addWidget(m_forgetMessage);

    layout->addStretch();
    return page;
}

bool SettingsDialog::songInProgress() const
{
    const KaraokePlayer* player = m_context.player;
    return player && (player->state() == KaraokePlayer::State::Playing
                      || player->state() == KaraokePlayer::State::Paused);
}

bool SettingsDialog::confirmForgetAll()
{
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Forget Every Song's Key and Tempo"),
                    QStringLiteral("This resets the Key and Tempo remembered for every song. "
                                   "Each song will start at its original key and speed again."),
                    QMessageBox::NoButton, this);
    box.setInformativeText(QStringLiteral("A backup copy of the current settings is saved first "
                                          "(next to them, in the program's data folder), so they "
                                          "can be restored by hand if needed."));
    QPushButton* confirm = box.addButton(QStringLiteral("Forget All"), QMessageBox::DestructiveRole);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    theme::rescale(&box);
    box.exec();
    return box.clickedButton() == confirm;
}

// ---- About ---------------------------------------------------------------

QWidget* SettingsDialog::buildAbout()
{
    const Timed timed("build About");
    QVBoxLayout* layout = nullptr;
    QWidget* page = addPage(QStringLiteral("About"), &layout);
    layout->addWidget(makeLabel(QStringLiteral("Frankie's Karaoke Studio"), page, QStringLiteral("aboutTitle")));
    const QString version = QCoreApplication::applicationVersion();
    layout->addWidget(makeLabel(version.isEmpty() ? QStringLiteral("Development build")
                                                  : QStringLiteral("Version %1").arg(version),
                                page));
    theme::addSpacing(layout, 10);
    gchar* gstreamer = gst_version_string();
    const QString gstreamerVersion = QString::fromUtf8(gstreamer);
    g_free(gstreamer);
#ifdef QT_DEBUG
    const QString build = QStringLiteral("Debug");
#else
    const QString build = QStringLiteral("Release");
#endif
    const QList<QPair<QString, QString>> details = {
        {QStringLiteral("Computer"), QSysInfo::prettyProductName() + QStringLiteral(" (")
                                         + QSysInfo::currentCpuArchitecture() + QLatin1Char(')')},
        {QStringLiteral("Qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("Audio engine"), gstreamerVersion},
        {QStringLiteral("Build"), build},
    };
    for (const auto& [name, value] : details) {
        QHBoxLayout* row = addRow(layout, name, page);
        auto* label = makeLabel(value, page, QStringLiteral("settingsHint"));
        label->setWordWrap(false);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(label);
    }
    layout->addStretch();
    return page;
}
