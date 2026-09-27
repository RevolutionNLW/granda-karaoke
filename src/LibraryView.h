#pragma once

#include <QWidget>

#include <functional>
#include <utility>

class LibraryController;
class LibraryResultsModel;
class QComboBox;
class QLabel;
class QLineEdit;
class QTreeView;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

class LibraryView : public QWidget {
    Q_OBJECT

public:
    using FolderChooser = std::function<QString(QWidget*)>;

    explicit LibraryView(LibraryController* controller, QWidget* parent = nullptr);

    void activate();
    void showMessage(const QString& message);
    void setFolderChooser(FolderChooser chooser) { m_folderChooser = std::move(chooser); }

    QLineEdit* searchBox() const { return m_searchBox; }
    QComboBox* sortBox() const { return m_sortBox; }
    QTreeView* resultsList() const { return m_results; }
    QPushButton* chooseFolderButton() const { return m_chooseFolderButton; }
    QPushButton* singButton() const { return m_singButton; }
    QPushButton* addToPlaylistButton() const { return m_addToPlaylistButton; }
    QLabel* statusLabel() const { return m_statusLabel; }
    QLabel* hintLabel() const { return m_hintLabel; }
    QLabel* messageLabel() const { return m_messageLabel; }
    int songResultCount() const;
    qint64 selectedSongId() const;
    void setPlaylistAvailable(bool available);
    // The search box and sort choice, as one bar. The view shows it above its
    // results until a host window takes it to place elsewhere; the view keeps
    // driving it either way.
    QWidget* takeSearchBar();
    // The pane in use shows its selection in gold; the other keeps its
    // selection, shown quietly.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // The song now being sung (0 for none), marked with a play symbol.
    void setPlayingSongId(qint64 songId);
    // Label and Plays can be hidden (Settings); Artist, Song and Disc stay.
    void setColumnVisible(int column, bool visible);

signals:
    void singRequested(qint64 songId);
    // The library was used with the mouse (e.g. its sort or buttons).
    void interacted();
    void addRequested(qint64 songId);

public slots:
    void refreshSearch();
    void chooseFolder();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void updateState();
    void singSelected();

private:
    void moveSelection(int delta);
    void applyColumnWidths();
    void updateSelectionActions();

    LibraryController* m_controller;
    QLabel* m_statusLabel;
    QStackedWidget* m_content;
    QWidget* m_setupPage;
    QWidget* m_searchPage;
    QVBoxLayout* m_searchLayout;
    QLabel* m_setupLabel;
    QPushButton* m_chooseFolderButton;
    QWidget* m_searchBar;
    QLineEdit* m_searchBox;
    QComboBox* m_sortBox;
    QLabel* m_hintLabel;
    QTreeView* m_results;
    LibraryResultsModel* m_resultsModel;
    QLabel* m_messageLabel;
    QPushButton* m_singButton;
    QPushButton* m_addToPlaylistButton;
    class QTimer* m_debounce;
    FolderChooser m_folderChooser;
    bool m_playlistAvailable = false;
    bool m_active = true;
    // The (trimmed) search text the results show, so typing only spaces
    // does not search again and lose the list's place.
    QString m_shownQuery;
};
