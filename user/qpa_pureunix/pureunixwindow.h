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
};

#endif
