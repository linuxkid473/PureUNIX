// user/qpa_pureunix/pureunixscreen.h — QPlatformScreen for the "pureunix"
// QPA plugin. Reports `pude`'s real desktop geometry (see
// pureunixintegration.cpp's initialize()) -- window *placement* itself is
// entirely `pude`'s own job (docs/qt-port.md Phase 6), this is only used
// by Qt for things like available-geometry/DPI calculations.
#ifndef PUREUNIX_QPA_SCREEN_H
#define PUREUNIX_QPA_SCREEN_H

#include <qpa/qplatformscreen.h>

class QPureUnixScreen : public QPlatformScreen
{
public:
    explicit QPureUnixScreen(const QSize &size) : m_geometry(QPoint(0, 0), size) {}

    QRect geometry() const override { return m_geometry; }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }

private:
    QRect m_geometry;
};

#endif
