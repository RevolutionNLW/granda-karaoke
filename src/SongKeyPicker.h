#pragma once

#include <QFrame>

class QLabel;
class QPushButton;

// "Set Original Song Key": a small popup for choosing the key a backing track
// is recorded in, before any Key +/- adjustment. The 24 keys are laid out as
// two grids (major, minor) with the program's usual spellings; one click
// chooses and closes. The key chosen before (by hand) is marked; a key worked
// out from the music is only mentioned, never shown as chosen.
class SongKeyPicker : public QFrame {
    Q_OBJECT

public:
    // Keys are 0-23 (music::MusicalKey::index); -1 for none.
    SongKeyPicker(const QString& songText, int manualKeyIndex, int detectedKeyIndex,
                  QWidget* parent = nullptr);

    // Opens above `anchor` (below it when there is no room), on screen.
    void popUpAt(QWidget* anchor);

    QPushButton* keyButton(int keyIndex) const;
    QPushButton* clearButton() const { return m_clear; }
    QLabel* detectedLabel() const { return m_detected; }

signals:
    void keyChosen(int keyIndex);
    void clearRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QList<QPushButton*> m_keys;
    QPushButton* m_clear;
    QLabel* m_detected;
};
