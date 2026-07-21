// user/qpa_pureunix/pureunixwindow.h — QPlatformWindow for the "pureunix"
// QPA plugin. One instance == one `pude`-hosted window (real chrome/
// move/resize/focus/z-order all owned by `pude`, see user/pude_qtclient.c
// -- this class only ever talks the wire protocol in
// user/pureunix_qpa_protocol.h, never draws its own decorations).
#ifndef PUREUNIX_QPA_WINDOW_H
#define PUREUNIX_QPA_WINDOW_H

#include <qpa/qplatformwindow.h>
#include <QtCore/QByteArray>
#include <cstdint>

class QPureUnixIntegration;

class QPureUnixWindow : public QPlatformWindow
{
public:
    QPureUnixWindow(QWindow *window, QPureUnixIntegration *integration);
    ~QPureUnixWindow() override;

    void setGeometry(const QRect &rect) override;
    void setVisible(bool visible) override;
    void setWindowTitle(const QString &title) override;
    WId winId() const override { return m_winId; }
    void requestActivateWindow() override;
    bool isExposed() const override { return m_visible; }

    // Called by QPureUnixIntegration when a message arrives on the pude ->
    // client pipe addressed to the (single, currently-active) window --
    // see that class's own handleIncomingMessage().
    void handleProtocolMessage(uint32_t type, const QByteArray &payload);

private:
    QPureUnixIntegration *m_integration;
    WId m_winId;
    bool m_visible = false;
    bool m_created = false;

    // `pude` (user/pude_qtclient.c) has exactly one content buffer/window
    // frame per client process -- no window-ID concept exists in the wire
    // protocol at all (docs/qt-port.md Phase 6 scope). The first window
    // this process ever creates (pcmanfm-qt's own QMainWindow) is the
    // "primary" one that owns that single real pude-side window; every
    // window after that (QMessageBox/QDialog/etc.) is "secondary" and
    // renders as a same-buffer overlay instead of a real separate pude
    // window -- see the constructor/destructor's own comments for why,
    // and third_party/pcmanfm-qt's docs entry for the real bug this fixes
    // (a secondary window's own close used to make pude think the whole
    // client had exited).
    bool m_isPrimary = false;
};

#endif
