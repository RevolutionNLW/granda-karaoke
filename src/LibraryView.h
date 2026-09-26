#pragma once

#include <QWidget>

#include <functional>
#include <utility>

class LibraryController;
class LibraryResultsModel;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;

class LibraryView : public QWidget {
    Q_OBJECT

public:
    using FolderChooser = std::function<QString(QWidget*)>;

    explicit LibraryView(LibraryController* controller, QWidget* parent = nullptr);

    void activate();
    void showMessage(const QString& message);
    void setFolderChooser(FolderChooser chooser) { m_folderChooser = std::move(chooser); }

    QLineEdit* searchBox() const { return m_searchBox; }
    QListView* resultsList() const { return m_results; }
    QPushButton* backButton() const { return m_backButton; }
    QPushButton* chooseFolderButton() const { return m_chooseFolderButton; }
    QPushButton* changeFolderButton() const { return m_changeFolderButton; }
    QPushButton* singButton() const { return m_singButton; }
    QPushButton* addToPlaylistButton() const { return m_addToPlaylistButton; }
    QLabel* statusLabel() const { return m_statusLabel; }
    QLabel* hintLabel() const { return m_hintLabel; }
    QLabel* messageLabel() const { return m_messageLabel; }
    int songResultCount() const;
    qint64 selectedSongId() const;
    void setPlaylistAvailable(bool available);

signals:
    void backRequested();
    void singRequested(qint64 songId);
    void addRequested(qint64 songId);

public slots:
    void refreshSearch();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void updateState();
    void chooseFolder();
    void singSelected();

private:
    void moveSelection(int delta);
    void updateSelectionActions();

    LibraryController* m_controller;
    QPushButton* m_backButton;
    QLabel* m_statusLabel;
    QStackedWidget* m_content;
    QWidget* m_setupPage;
    QWidget* m_searchPage;
    QLabel* m_setupLabel;
    QPushButton* m_chooseFolderButton;
    QLineEdit* m_searchBox;
    QLabel* m_hintLabel;
    QListView* m_results;
    LibraryResultsModel* m_resultsModel;
    QLabel* m_messageLabel;
    QPushButton* m_changeFolderButton;
    QPushButton* m_singButton;
    QPushButton* m_addToPlaylistButton;
    class QTimer* m_debounce;
    FolderChooser m_folderChooser;
    bool m_playlistAvailable = false;
};
