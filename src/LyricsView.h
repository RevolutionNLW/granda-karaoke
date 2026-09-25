#pragma once

#include <QImage>
#include <QWidget>

// Child page displaying CDG lyrics; page selection never affects playback.
class LyricsView : public QWidget {
    Q_OBJECT

public:
    explicit LyricsView(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    const QImage& frame() const { return m_frame; }

signals:
    void controlsRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QImage m_frame;
};
