#pragma once

#include "library/Catalogue.h"

#include <QDialog>

class LibraryController;
class LyricsView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextBrowser;

// Library maintenance for the person who looks after the collection, not for
// the singer: review songs whose automatic names are uncertain, see the
// evidence behind them, correct artist/title by hand (stored only in the
// application's own database; song files are never changed) and reprocess
// the automatic metadata in the background.
class MetadataReviewDialog : public QDialog {
    Q_OBJECT

public:
    explicit MetadataReviewDialog(LibraryController* controller, QWidget* parent = nullptr);

    QComboBox* filterBox() const { return m_filter; }
    QLineEdit* searchBox() const { return m_search; }
    QTableWidget* songTable() const { return m_table; }
    QTextBrowser* detailView() const { return m_detail; }
    QLineEdit* artistEdit() const { return m_artistEdit; }
    QLineEdit* titleEdit() const { return m_titleEdit; }
    QPushButton* saveButton() const { return m_save; }
    QPushButton* clearButton() const { return m_clear; }
    QPushButton* reprocessButton() const { return m_reprocess; }
    QCheckBox* titleScreensBox() const { return m_titleScreens; }
    QLabel* statusLabel() const { return m_status; }
    QPushButton* playPreviewButton() const { return m_playPreview; }
    QPushButton* stopPreviewButton() const { return m_stopPreview; }
    QLabel* previewLabel() const { return m_previewLabel; }
    LyricsView* previewView() const { return m_previewView; }
    QLabel* detectedTitleLabel() const { return m_detectedTitle; }
    QPushButton* useDetectedTitleButton() const { return m_useDetectedTitle; }
    qint64 selectedSongId() const;
    bool selectSong(qint64 songId);

    // Preview playback belongs to the main window; this screen only asks for
    // it and shows it. Without a player the preview controls stay disabled.
    void setPreviewAvailable(bool available);
    void setPreviewState(bool active, const QString& description);
    void showPreviewFrame(const QImage& frame);
    void showPreviewMessage(const QString& message);

public slots:
    void refresh();

signals:
    void previewRequested(qint64 songId);
    void previewStopRequested();

private:
    void showDetail();
    void saveCorrection();
    void clearCorrection();
    void reprocess();
    void updateStatus();
    void updatePreviewButtons();

    LibraryController* m_controller;
    QComboBox* m_filter;
    QLineEdit* m_search;
    QTableWidget* m_table;
    QTextBrowser* m_detail;
    QLineEdit* m_artistEdit;
    QLineEdit* m_titleEdit;
    QPushButton* m_save;
    QPushButton* m_clear;
    QPushButton* m_reprocess;
    QCheckBox* m_titleScreens;
    QLabel* m_status;
    QLabel* m_message;
    QPushButton* m_playPreview;
    QPushButton* m_stopPreview;
    QLabel* m_previewLabel;
    LyricsView* m_previewView;
    QWidget* m_detectedRow;
    QLabel* m_detectedTitle;
    QPushButton* m_useDetectedTitle;
    bool m_previewAvailable = false;
    bool m_previewActive = false;
};
