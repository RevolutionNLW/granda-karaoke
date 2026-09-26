#pragma once

#include "playlist/PlaylistTypes.h"

#include <QWidget>

#include <functional>
#include <optional>

class LibraryController;
class PlaylistPlayback;
class PlaylistStore;
class QComboBox;
class QLabel;
class QListWidget;
class QMimeData;
class QPushButton;

class PlaylistView : public QWidget {
    Q_OBJECT

public:
    using NamePrompt = std::function<std::optional<QString>(
        QWidget*, const QString& title, const QString& currentName)>;
    using DeleteConfirmation = std::function<bool(QWidget*, const QString& name)>;
    using RemoveConfirmation = std::function<bool(
        QWidget*, const QString& songText, const QString& playlistName)>;

    explicit PlaylistView(PlaylistStore* store, LibraryController* libraryController,
                          PlaylistPlayback* playback, QWidget* parent = nullptr);

    void activate();
    void showMessage(const QString& message);
    qint64 displayedPlaylistId() const;
    qint64 selectedItemId() const;
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }
    void setDeleteConfirmation(DeleteConfirmation confirm)
    {
        m_deleteConfirmation = std::move(confirm);
    }
    void setRemoveConfirmation(RemoveConfirmation confirm)
    {
        m_removeConfirmation = std::move(confirm);
    }

    QComboBox* playlistChooser() const { return m_chooser; }
    QListWidget* itemList() const { return m_items; }
    QLabel* messageLabel() const { return m_message; }
    QPushButton* newButton() const { return m_newButton; }
    QPushButton* renameButton() const { return m_renameButton; }
    QPushButton* deleteButton() const { return m_deleteButton; }
    QPushButton* playButton() const { return m_playButton; }
    QPushButton* moveUpButton() const { return m_moveUpButton; }
    QPushButton* moveDownButton() const { return m_moveDownButton; }
    QPushButton* removeButton() const { return m_removeButton; }
    QPushButton* autoplayButton() const { return m_autoplayButton; }

public slots:
    void refresh();
    void addSong(qint64 songId);

signals:
    void playRequested(PlaylistEntry entry, bool autoplay);
    void displayedPlaylistChanged(bool available);
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshPlaylists(qint64 preferredId = 0);
    void refreshItems(qint64 selectItemId = 0, bool ensureVisible = false);
    void updateButtons();
    void updatePlaybackMarkers();
    void createPlaylist();
    void renamePlaylist();
    void deletePlaylist();
    void playSelected();
    void moveSelected(int delta);
    void removeSelected();
    bool canAcceptDrop(const QMimeData* mimeData) const;
    QString displayText(const PlaylistEntry& entry) const;
    std::optional<QString> askForName(const QString& title,
                                      const QString& currentName = {});
    void showStoreError(const QString& fallback, const QString& detail = {});

    PlaylistStore* m_store;
    LibraryController* m_libraryController;
    PlaylistPlayback* m_playback;
    QComboBox* m_chooser;
    QLabel* m_message;
    QListWidget* m_items;
    QPushButton* m_newButton;
    QPushButton* m_renameButton;
    QPushButton* m_deleteButton;
    QPushButton* m_playButton;
    QPushButton* m_moveUpButton;
    QPushButton* m_moveDownButton;
    QPushButton* m_removeButton;
    QPushButton* m_autoplayButton;
    NamePrompt m_namePrompt;
    DeleteConfirmation m_deleteConfirmation;
    RemoveConfirmation m_removeConfirmation;
};
