#include "pureunixbackingstore.h"
#include "pureunixintegration.h"

extern "C" {
#include "../pureunix_qpa_protocol.h"
}

#include <QtGui/QWindow>
#include <QtCore/QByteArray>

#include <cstring>

QPureUnixBackingStore::QPureUnixBackingStore(QWindow *window, QPureUnixIntegration *integration)
    : QPlatformBackingStore(window)
    , m_integration(integration)
{
}

void QPureUnixBackingStore::resize(const QSize &size, const QRegion &staticContents)
{
    Q_UNUSED(staticContents);
    if (m_image.size() != size) {
        // Format_ARGB32 (not _Premultiplied): user/pureunix_qpa_protocol.h's
        // PU_QPA_C2S_DAMAGE payload is documented as plain, non-premultiplied
        // ARGB32 bytes -- matches what `pude`'s own SDL_Surface side expects
        // to just memcpy row-by-row (user/pude_qtclient.c), no conversion
        // needed on either end.
        m_image = QImage(size, QImage::Format_ARGB32);
        m_image.fill(Qt::white);
    }
}

void QPureUnixBackingStore::flush(QWindow *window, const QRegion &region, const QPoint &offset)
{
    Q_UNUSED(window);
    Q_UNUSED(offset);
    if (m_image.isNull()) {
        return;
    }
    // Sends the changed region's *bounding* rectangle rather than each of
    // QRegion's individual sub-rectangles -- matches this codebase's own
    // existing level of damage-tracking sophistication (`pude`'s own
    // compositor does a full-frame redraw every changed iteration with no
    // finer-grained damage-rect concept at all, per docs/qt-port.md's
    // Phase 1 audit) rather than inventing a more precise scheme just for
    // this one plugin; real, working, and simple, not a regression
    // against anything more sophisticated that already existed.
    QRect rect = region.boundingRect().intersected(m_image.rect());
    if (rect.isEmpty()) {
        return;
    }
    QImage sub = m_image.copy(rect);
    if (sub.format() != QImage::Format_ARGB32) {
        sub = sub.convertToFormat(QImage::Format_ARGB32);
    }
    // sub.bits() rows are already tightly packed after copy()/
    // convertToFormat() only when bytesPerLine() == width()*4 -- QImage
    // doesn't guarantee that in general, so this walks scanlines
    // explicitly rather than assuming one contiguous memcpy is safe.
    QByteArray pixels;
    pixels.resize(rect.width() * rect.height() * 4);
    for (int y = 0; y < rect.height(); y++) {
        memcpy(pixels.data() + y * rect.width() * 4, sub.constScanLine(y), (size_t)rect.width() * 4);
    }

    pu_qpa_damage_t dmg;
    dmg.x = rect.x();
    dmg.y = rect.y();
    dmg.w = rect.width();
    dmg.h = rect.height();

    // The header/damage-struct and the pixel payload are two logically
    // distinct pieces sent as one message on the wire (see
    // user/pureunix_qpa_protocol.h's PU_QPA_C2S_DAMAGE comment: the
    // payload is pu_qpa_damage_t immediately followed by the raw pixel
    // bytes) -- QPureUnixIntegration::sendMessage() only frames a single
    // contiguous payload, so this concatenates them first rather than
    // adding a second wire concept just for this one caller.
    QByteArray payload;
    payload.resize(sizeof(dmg) + pixels.size());
    memcpy(payload.data(), &dmg, sizeof(dmg));
    memcpy(payload.data() + sizeof(dmg), pixels.constData(), (size_t)pixels.size());
    m_integration->sendMessage(PU_QPA_C2S_DAMAGE, payload.constData(), (uint32_t)payload.size());
}
