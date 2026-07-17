// user/qpa_pureunix/pureunixintegration.h — QPlatformIntegration for the
// real "pureunix" QPA platform plugin (docs/qt-port.md, Phase 6). Real
// upstream Qt QPA API only; every PureUnix-specific detail (the pipe
// protocol to `pude`, see user/pureunix_qpa_protocol.h) lives inside this
// plugin, never inside Qt itself.
#ifndef PUREUNIX_QPA_INTEGRATION_H
#define PUREUNIX_QPA_INTEGRATION_H

#include <qpa/qplatformintegration.h>
#include <QtCore/QScopedPointer>

QT_BEGIN_NAMESPACE
class QPlatformFontDatabase;
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

    // ---- pude protocol connection (user/pureunix_qpa_protocol.h) ----
    // A single client process hosts exactly one top-level PUDE-managed
    // window for now (docs/qt-port.md Phase 6 scope) -- multiple
    // concurrent windows per client is a real, natural follow-up, not
    // attempted here. Real upstream Qt code never touches any of this
    // directly; it only ever goes through QPlatformWindow/
    // QPlatformBackingStore's own overrides below.
    bool connected() const { return m_connected; }
    int readFd() const { return m_readFd; }
    int writeFd() const { return m_writeFd; }

    // Sends a fully-framed message (header + payload) to `pude` over
    // m_writeFd. Safe to call even when !connected() (a silent no-op) --
    // every real caller already only exists because a QPureUnixWindow was
    // constructed, and this lets a Phase 1-style standalone test (no real
    // `pude` on the other end) run without crashing.
    void sendMessage(uint32_t type, const void *payload, uint32_t len) const;

    // Registers/unregisters the one active window so incoming pude ->
    // client messages (resize/close/input, read by the QSocketNotifier
    // set up in initialize()) have somewhere to go. Called by
    // QPureUnixWindow's constructor/destructor.
    void setActiveWindow(QPureUnixWindow *win) { m_activeWindow = win; }

private:
    void handleIncomingMessage(uint32_t type, const QByteArray &payload);

    bool m_connected = false;
    int m_readFd = -1;
    int m_writeFd = -1;
    QPureUnixWindow *m_activeWindow = nullptr;
    QScopedPointer<QPlatformFontDatabase> m_fontDatabase;

    class ReadBuffer;
    QScopedPointer<ReadBuffer> m_readBuffer;
};

#endif
