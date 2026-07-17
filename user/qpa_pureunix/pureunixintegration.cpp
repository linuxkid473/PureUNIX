#include "pureunixintegration.h"
#include "pureunixscreen.h"
#include "pureunixwindow.h"
#include "pureunixbackingstore.h"

extern "C" {
#include "../pureunix_qpa_protocol.h"
}

#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <QtGui/private/qunixeventdispatcher_qpa_p.h>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QtCore/QSocketNotifier>
#include <QtCore/QByteArray>
#include <QtCore/QDebug>

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

// Incremental, resumable parser for the pude -> client pipe
// (PUREUNIX_QPA_FD_READ) -- messages can legitimately arrive split across
// more than one readyRead() callback (the header and payload are separate
// pipe writes on PUDE's side, and even a single write() isn't guaranteed
// to be delivered as one whole read() on the other end), so this buffers
// whatever's newly available and only ever consumes as many *complete*
// header+payload messages as have actually arrived so far, exactly like
// user/pude_qtclient.c's own mirror-image parser does for the opposite
// direction (see that file's own comment for the shared reasoning).
class QPureUnixIntegration::ReadBuffer
{
public:
    void append(const char *data, int len) { buf.append(data, len); }

    // Calls `fn(type, payload)` once per complete message found, removing
    // each from the front of the buffer as it's consumed.
    template <typename Fn>
    void drain(Fn fn)
    {
        for (;;) {
            if (buf.size() < (int)sizeof(pu_qpa_msg_header_t)) {
                return;
            }
            pu_qpa_msg_header_t hdr;
            memcpy(&hdr, buf.constData(), sizeof(hdr));
            int total = (int)(sizeof(hdr) + hdr.len);
            if (buf.size() < total) {
                return; // payload not fully arrived yet
            }
            QByteArray payload = buf.mid(sizeof(hdr), (int)hdr.len);
            buf.remove(0, total);
            fn(hdr.type, payload);
        }
    }

private:
    QByteArray buf;
};

QPureUnixIntegration::QPureUnixIntegration(const QStringList &parameters)
{
    Q_UNUSED(parameters);
    m_readBuffer.reset(new ReadBuffer);

    // Both fds are inherited exactly as-is from `pude` (user/pude_qtclient.c
    // dup2()s them before execve(), the same "fixed fd numbers a forked
    // child inherits" pattern PUTerm's own pty slave already uses onto
    // 0/1/2 -- see user/pureunix_qpa_protocol.h). A plain fcntl() probe
    // tells us whether we're actually running under `pude` at all (a
    // standalone `-platform pureunix` test run with nothing on the other
    // end simply gets a disconnected integration -- every sendMessage()
    // below becomes a safe no-op rather than a crash).
    m_readFd = PUREUNIX_QPA_FD_READ;
    m_writeFd = PUREUNIX_QPA_FD_WRITE;
    m_connected = fcntl(m_readFd, F_GETFD) >= 0 && fcntl(m_writeFd, F_GETFD) >= 0;
}

QPureUnixIntegration::~QPureUnixIntegration()
{
}

void QPureUnixIntegration::initialize()
{
    QPlatformIntegration::initialize();

    // Real screen geometry: `pude` (user/pude_qtclient.c) passes its own
    // actual desktop resolution (queried once at boot via SYS_FB_GETINFO,
    // see docs/pude.md) through these two env vars right before execve()
    // -- the smallest way to convey it without a dedicated protocol
    // message, since it never changes for the lifetime of one client
    // process. Falls back to a reasonable default for a standalone
    // `-platform pureunix` test run with no `pude` on the other end.
    int screenW = 1024, screenH = 768;
    if (const char *w = std::getenv("PUREUNIX_QPA_SCREEN_W")) {
        screenW = std::atoi(w);
    }
    if (const char *h = std::getenv("PUREUNIX_QPA_SCREEN_H")) {
        screenH = std::atoi(h);
    }
    QWindowSystemInterface::handleScreenAdded(new QPureUnixScreen(QSize(screenW, screenH)));

    if (m_connected) {
        auto *notifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read);
        QObject::connect(notifier, &QSocketNotifier::activated, [this](QSocketDescriptor, QSocketNotifier::Type) {
            char chunk[4096];
            for (;;) {
                int n = (int)::read(m_readFd, chunk, sizeof(chunk));
                if (n <= 0) {
                    break;
                }
                m_readBuffer->append(chunk, n);
            }
            m_readBuffer->drain([this](uint32_t type, const QByteArray &payload) {
                handleIncomingMessage(type, payload);
            });
        });
    }
}

bool QPureUnixIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case QPlatformIntegration::NonFullScreenWindows:
    case QPlatformIntegration::WindowManagement:
    case QPlatformIntegration::MultipleWindows:
    case QPlatformIntegration::ThreadedPixmaps:
        return true;
    // Deliberately NOT claiming PaintEvents: that capability tells
    // QGuiApplicationPrivate::processExposeEvent() that *this plugin*
    // will emit real QWindowSystemInterfacePrivate::Paint events itself,
    // which this plugin never does -- only handleExposeEvent()
    // (QPureUnixWindow::setVisible()/handleProtocolMessage()'s
    // PU_QPA_S2C_RESIZE case). Leaving this false is what makes Qt
    // synthesize QPaintEvent automatically from those expose events
    // (see qguiapplication.cpp's shouldSynthesizePaintEvents), which is
    // the only reason QRasterWindow::paintEvent() ever fires here --
    // real bug found the hard way: claiming PaintEvents=true left every
    // window a real native chrome shell with a permanently-black client
    // area, since paintEvent() was simply never called.
    default:
        return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow *QPureUnixIntegration::createPlatformWindow(QWindow *window) const
{
    return new QPureUnixWindow(window, const_cast<QPureUnixIntegration *>(this));
}

QPlatformBackingStore *QPureUnixIntegration::createPlatformBackingStore(QWindow *window) const
{
    return new QPureUnixBackingStore(window, const_cast<QPureUnixIntegration *>(this));
}

QAbstractEventDispatcher *QPureUnixIntegration::createEventDispatcher() const
{
    return new QUnixEventDispatcherQPA;
}

QPlatformFontDatabase *QPureUnixIntegration::fontDatabase() const
{
    if (!m_fontDatabase) {
        const_cast<QPureUnixIntegration *>(this)->m_fontDatabase.reset(new QGenericUnixFontDatabase);
    }
    return m_fontDatabase.data();
}

void QPureUnixIntegration::sendMessage(uint32_t type, const void *payload, uint32_t len) const
{
    if (!m_connected) {
        return;
    }
    pu_qpa_msg_header_t hdr;
    hdr.type = type;
    hdr.len = len;
    // Small, real blocking writes (docs/qt-port.md Phase 6's own
    // reasoning: input/control messages here are tiny relative to the
    // real 4096-byte pipe buffer, so this can't meaningfully stall
    // `pude` in practice -- the one deliberately-accepted simplification
    // this phase makes, same spirit as the rest of this port's "smallest
    // production-quality version, not overengineered" primitives).
    ::write(m_writeFd, &hdr, sizeof(hdr));
    if (len > 0) {
        ::write(m_writeFd, payload, len);
    }
}

void QPureUnixIntegration::handleIncomingMessage(uint32_t type, const QByteArray &payload)
{
    if (!m_activeWindow) {
        return;
    }
    m_activeWindow->handleProtocolMessage(type, payload);
}
