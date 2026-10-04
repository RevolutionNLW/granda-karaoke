#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QMessageBox>
#include <QPointer>

#include <functional>

class AppPreferences;
class ISongSettingsStore;
struct ReviewSummary;
class KaraokePlayer;
class LibraryController;
class LibraryView;
class PlaylistStore;
class QComboBox;
class QKeySequenceEdit;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// Settings: one place for everything the user or the maintainer may want to
// change. Changes are saved and applied as they are made.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    struct Context {
        AppPreferences* preferences = nullptr;
        LibraryController* libraryController = nullptr;
        LibraryView* libraryView = nullptr;
        PlaylistStore* playlistStore = nullptr;
        ISongSettingsStore* songSettings = nullptr;
        KaraokePlayer* player = nullptr;
        QList<QPair<QString, QString>> dataLocations;
        std::function<void()> openSongFile;
        std::function<void()> openNeedsReview;
        // Closes the program (asking first if the user chose so).
        std::function<void()> quit;
    };
    // Asked before a shortcut is moved from one action to another; returns
    // true to move it (tests replace the question).
    using ReassignConfirmation = std::function<bool(
        const QString& keys, const QString& fromAction, const QString& toAction)>;
    // Asked before every song's Key/Tempo is forgotten (tests replace it).
    using ForgetConfirmation = std::function<bool()>;

    explicit SettingsDialog(Context context, QWidget* parent = nullptr);

    QStringList pageNames() const;
    void showPage(const QString& name);
    QString currentPage() const;

    // Gives an action a shortcut (empty = none) directly: refused keys leave
    // everything as it was, and keys used by another action move only if the
    // reassign confirmation says so (no question is shown). The Shortcuts page
    // itself asks with a question instead. Returns whether it was set.
    bool assignShortcut(const QString& id, const QKeySequence& keys, QString* message = nullptr);
    void setReassignConfirmation(ReassignConfirmation confirm) { m_reassign = std::move(confirm); }
    void setForgetConfirmation(ForgetConfirmation confirm) { m_forget = std::move(confirm); }

    // (Each makes its page if it is not made yet.)
    QLabel* scaleValueLabel();
    QPushButton* scaleDownButton();
    QPushButton* scaleUpButton();
    QPushButton* scaleResetButton();
    QLabel* scaleLimitedLabel();
    QTreeWidget* shortcutList();
    QLineEdit* shortcutFilter();
    QKeySequenceEdit* shortcutEditor();
    QLabel* shortcutMessage();
    QLabel* metadataCounts();
    QCheckBox* songKeysCheckBox();
    QLabel* songKeyStatus();
    // Whether a page has been made yet (pages are made when first shown).
    bool isPageBuilt(const QString& name) const { return m_built.value(pageIndex(name)); }
    QMessageBox* shortcutConflictPrompt() const { return m_conflictPrompt; }

public slots:
    void done(int result) override;

private:
    QWidget* addPage(const QString& name, QVBoxLayout** content);
    void ensureBuilt(int index);
    int pageIndex(const QString& name) const;
    void showMetadataCounts(const ReviewSummary& summary);
    QWidget* buildGeneral();
    QWidget* buildAppearance();
    QWidget* buildPlayback();
    QWidget* buildLibrary();
    QWidget* buildAudio();
    QWidget* buildPlaylists();
    QWidget* buildShortcuts();
    QWidget* buildMetadata();
    QWidget* buildAdvanced();
    QWidget* buildAbout();
    void refreshMetadataCounts();
    void refreshMetadataStatus();
    void refreshSongKeyStatus();
    bool confirmForgetAll();
    bool songInProgress() const;
    void refreshScale();
    void fitToScreen();
    void refreshLibraryStatus();
    void refreshShortcutList();
    void loadShortcutEditor();
    void applyShortcutEditor();
    // A change made on the Shortcuts page; asks before taking keys from
    // another action. Only one change is ever in progress.
    void changeShortcut(const QString& id, const QKeySequence& keys);
    bool moveShortcut(const QString& fromId, const QString& toId, const QKeySequence& keys,
                      QString* message);
    void forgetAllSongSettings();
    QString selectedShortcutId() const;

    // Brings every control back in line with the stored settings (after a
    // Restore Defaults, or a change made elsewhere).
    void refreshControls();

    Context m_context;
    QList<QPair<QString, QWidget* (SettingsDialog::*)()>> m_builders;
    QList<bool> m_built;
    int m_buildingIndex = 0;
    class QTimer* m_libraryStatusTimer = nullptr;
    QList<std::function<void()>> m_refreshers;
    ReassignConfirmation m_reassign;
    ForgetConfirmation m_forget;
    QListWidget* m_nav;
    QStackedWidget* m_pages;
    QLabel* m_pageTitle;
    QLabel* m_scaleValue = nullptr;
    QPushButton* m_scaleDown = nullptr;
    QPushButton* m_scaleUp = nullptr;
    QPushButton* m_scaleReset = nullptr;
    QLabel* m_scaleLimited = nullptr;
    QLabel* m_folderLabel = nullptr;
    QLabel* m_libraryStatus = nullptr;
    QLabel* m_lastScan = nullptr;
    QComboBox* m_sortChoice = nullptr;
    QLabel* m_forgetMessage = nullptr;
    QLabel* m_metadataCounts = nullptr;
    QLabel* m_reprocessStatus = nullptr;
    QCheckBox* m_songKeys = nullptr;
    QLabel* m_songKeyStatus = nullptr;
    QTreeWidget* m_shortcutList = nullptr;
    QLineEdit* m_shortcutFilter = nullptr;
    QLabel* m_shortcutEditorTitle = nullptr;
    QKeySequenceEdit* m_shortcutEditor = nullptr;
    QPushButton* m_shortcutClear = nullptr;
    QPushButton* m_shortcutReset = nullptr;
    QLabel* m_shortcutMessage = nullptr;
    // The open "already assigned" question, if any.
    QPointer<QMessageBox> m_conflictPrompt;
};
