// user/qpa_pureunix/pureunixbackingstore.h — QPlatformBackingStore for the
// "pureunix" QPA plugin. Real Qt raster paint engine draws into mImage
// (an ordinary QImage) exactly like any other raster-backed platform;
// flush() is the one PureUnix-specific piece, sending the changed pixels
// to `pude` over the wire protocol (user/pureunix_qpa_protocol.h) instead
// of blitting to a local framebuffer.
#ifndef PUREUNIX_QPA_BACKINGSTORE_H
#define PUREUNIX_QPA_BACKINGSTORE_H

#include <qpa/qplatformbackingstore.h>
#include <QtGui/QImage>

class QPureUnixIntegration;

class QPureUnixBackingStore : public QPlatformBackingStore
{
public:
    QPureUnixBackingStore(QWindow *window, QPureUnixIntegration *integration);

    QPaintDevice *paintDevice() override { return &m_image; }
    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override;
    void resize(const QSize &size, const QRegion &staticContents) override;

private:
    QPureUnixIntegration *m_integration;
    QImage m_image;
};

#endif
