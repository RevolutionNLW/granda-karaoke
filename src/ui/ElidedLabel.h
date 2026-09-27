#pragma once

#include <QLabel>

// A one-line label that shortens its text with "…" when it does not fit,
// instead of clipping it or widening the window. text() stays the full text.
class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
