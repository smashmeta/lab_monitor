#pragma once

#include <QColor>
#include <QImage>
#include <QWidget>

#include <cstdlib>

namespace lm::ui::test {

/// True when any pixel sits within `tolerance` of `colour` on every channel.
/// Text is drawn with the resolved pen colour, so its core pixels land exactly
/// on it; the tolerance only covers antialiased edges being counted too.
inline bool contains_colour(const QImage& image, const QColor& colour, int tolerance = 8) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - colour.red()) <= tolerance &&
                std::abs(pixel.green() - colour.green()) <= tolerance &&
                std::abs(pixel.blue() - colour.blue()) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

/// Paints a widget as the screen would, through whatever stylesheet is
/// installed on the application.
inline QImage paint(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

}  // namespace lm::ui::test
