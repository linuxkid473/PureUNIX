// user/qpa_pureunix/pureunixintegration.h — QPlatformIntegration for the
// real "pureunix" QPA platform plugin (docs/qt-port.md, Phase 6). Real
// upstream Qt QPA API only; every PureUnix-specific detail (the pipe
// protocol to `pude`, see user/pureunix_qpa_protocol.h) lives inside this
// plugin, never inside Qt itself.
#ifndef PUREUNIX_QPA_INTEGRATION_H
#define PUREUNIX_QPA_INTEGRATION_H

#include <qpa/qplatformintegration.h>
#include <QtCore/QByteArray>
#include <QtCore/QScopedPointer>

QT_BEGIN_NAMESPACE
class QPlatformFontDatabase;
class QPlatformTheme;
QT_END_NAMESPACE

class QPureUnixWindow;

class QPureUnixIntegration : public QPlatformIntegration
{
public:
    QPureUnixIntegration(const QStringList &parameters);
    ~QPureUnixIntegration() override;

    bool hasCapability(QPlatformIntegration::Capability cap) const override;
    QPlatformWindow *createPlatformWindow(QWindow *window) const override;
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override;
    QAbstractEventDispatcher *createEventDispatcher() const override;
    void initialize() override;
    QPlatformFontDatabase *fontDatabase() const override;
    QStringList themeNames() const override;
    QPlatformTheme *createPlatformTheme(const QString &name) const override;

    // ---- pude protocol connection (user/pureunix_qpa_protocol.h) ----
    // A single client process hosts exactly one real PUDE-managed window
    // (docs/qt-port.md Phase 6 scope, and `pude_qtclient.c`'s
    // qtclient_state_t has exactly one content buffer, no window-ID
    // concept at all). A second top-level QWindow (any QDialog/
    // QMessageBox) can't get a second real pude window -- see
    // QPureUnixWindow's own "primary vs secondary" comment for how that's
    // handled without crashing/closing the whole app. Real upstream Qt
    // code never touches any of this directly; it only ever goes through
    // QPlatformWindow/QPlatformBackingStore's own overrides below.
    bool connected() const { return m_connected; }
    int readFd() const { return m_readFd; }
    int writeFd() const { return m_writeFd; }

    // Sends a fully-framed message (header + payload) to `pude` over
    // m_writeFd with a real blocking write. Safe to call even when
    // !connected() (a silent no-op) -- every real caller already only
    // exists because a QPureUnixWindow was constructed, and this lets a
    // Phase 1-style standalone test (no real `pude` on the other end) run
    // without crashing.
    //
    // Blocking here is deliberate and safe, not an oversight -- see this
    // function's own definition in pureunixintegration.cpp for the full
    // reasoning (a real bidirectional-pipe deadlock this was once
    // mistakenly "fixed" for by making this non-blocking too, which broke
    // large-payload throughput instead; the actual fix lives entirely on
    // `pude`'s own side, user/pude_qtclient.c's send_message()).
    void sendMessage(uint32_t type, const void *payload, uint32_t len) const;

    // Registers/unregisters the one active window so incoming pude ->
    // client messages (resize/close/input, read by the QSocketNotifier
    // set up in initialize()) have somewhere to go. Called by
    // QPureUnixWindow's constructor/destructor.
    void setActiveWindow(QPureUnixWindow *win) { m_activeWindow = win; }
    QPureUnixWindow *activeWindow() const { return m_activeWindow; }

    // The one window per client that actually owns the real pude-side
    // window/content buffer (see QPureUnixWindow's own comment). Set once,
    // by whichever QPureUnixWindow is constructed first; cleared when that
    // same window is destroyed (at which point the client is closing down
    // anyway).
    QPureUnixWindow *primaryWindow() const { return m_primaryWindow; }
    void setPrimaryWindow(QPureUnixWindow *win) { m_primaryWindow = win; }

private:
    void handleIncomingMessage(uint32_t type, const QByteArray &payload);

    bool m_connected = false;
    int m_readFd = -1;
    int m_writeFd = -1;
    QPureUnixWindow *m_activeWindow = nullptr;
    QPureUnixWindow *m_primaryWindow = nullptr;
    QScopedPointer<QPlatformFontDatabase> m_fontDatabase;

    class ReadBuffer;
    QScopedPointer<ReadBuffer> m_readBuffer;
};

#endif
