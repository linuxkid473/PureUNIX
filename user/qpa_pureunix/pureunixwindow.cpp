#include "pureunixwindow.h"
#include "pureunixintegration.h"

extern "C" {
#include "../pureunix_qpa_protocol.h"
}

#include <QtGui/QWindow>
#include <QtGui/qpa/qwindowsysteminterface.h>

#include <cstring>

static WId nextWinId()
{
    static WId next = 1;
    return next++;
}

QPureUnixWindow::QPureUnixWindow(QWindow *window, QPureUnixIntegration *integration)
    : QPlatformWindow(window)
    , m_integration(integration)
    , m_winId(nextWinId())
{
    // See this class's own header comment: pude has exactly one real
    // window per client, so only the FIRST QPureUnixWindow ever created
    // (pcmanfm-qt's own QMainWindow, always constructed before any
    // QDialog/QMessageBox) gets to be it. Every later one is "secondary"
    // and skips WINDOW_CREATE/SET_TITLE entirely below -- it still paints
    // (QPureUnixBackingStore's flush() sends PU_QPA_C2S_DAMAGE
    // unconditionally), but into the *same* shared buffer at its own
    // window-local (0,0) origin, i.e. the top-left corner of the primary
    // window's real on-screen area. That's a real, visible simplification
    // (a dialog "overlay" isn't centered or separately decorated) rather
    // than a proper second window, but it's honest and it doesn't corrupt
    // or close the primary window -- seeing docs/pcmanfm-port.md's
    // 2026-07-21 entry for the real bug (a secondary window's own close
    // used to tell pude the whole client had exited) this replaces.
    m_isPrimary = !integration->primaryWindow();
    if (m_isPrimary) {
        integration->setPrimaryWindow(this);

        // Real upstream QPlatformWindow's own constructor already recorded
        // window->geometry() as this window's initial rect (accessible via
        // geometry() below) -- that's exactly the size `pude` should create
        // its own real window at, so it's sent once here rather than waiting
        // for a later setGeometry() call (docs/qt-port.md Phase 6: `pude`
        // owns every *subsequent* resize/move; this is only the initial
        // size hint a real WM needs from any client).
        QRect rect = geometry();
        pu_qpa_window_create_t create;
        create.default_w = rect.width() > 0 ? rect.width() : 400;
        create.default_h = rect.height() > 0 ? rect.height() : 300;
        m_integration->sendMessage(PU_QPA_C2S_WINDOW_CREATE, &create, sizeof(create));

        const QString title = window->title();
        if (!title.isEmpty()) {
            QByteArray utf8 = title.toUtf8();
            m_integration->sendMessage(PU_QPA_C2S_SET_TITLE, utf8.constData(), (uint32_t)utf8.size());
        }
    }

    m_integration->setActiveWindow(this);
    m_created = true;
}

QPureUnixWindow::~QPureUnixWindow()
{
    if (m_created && m_isPrimary) {
        // Only the primary window's own close means the client is
        // actually going away -- see the constructor's comment. A
        // secondary (dialog) window closing must NOT send this: pude
        // treats PU_QPA_C2S_CLOSE as "this client is done" (sets
        // qtclient_state_t.child_alive = false), which used to make the
        // *entire* app disappear from the desktop the instant a
        // QMessageBox/QDialog was dismissed, even though the process
        // itself (confirmed via `ps`) was still very much alive.
        m_integration->sendMessage(PU_QPA_C2S_CLOSE, nullptr, 0);
    }
    if (m_integration) {
        if (m_isPrimary) {
            m_integration->setActiveWindow(nullptr);
            if (m_integration->primaryWindow() == this) {
                m_integration->setPrimaryWindow(nullptr);
            }
        } else {
            // A secondary window closing hands input routing back to the
            // primary window (if it's still around) instead of nulling it
            // out -- otherwise the whole app would silently stop
            // receiving keyboard/mouse input the moment any dialog closed.
            QPureUnixWindow *primary = m_integration->primaryWindow();
            if (m_integration->activeWindow() == this) {
                m_integration->setActiveWindow(primary);
            }
            // The dialog's own DAMAGE painted directly into the shared
            // buffer's top-left corner (see constructor comment); force a
            // full repaint of the primary window now so those stale
            // pixels don't linger on screen after the dialog is gone.
            if (primary) {
                QWindowSystemInterface::handleExposeEvent(
                    primary->window(), QRect(QPoint(0, 0), primary->geometry().size()));
            }
        }
    }
}

void QPureUnixWindow::setGeometry(const QRect &rect)
{
    // Real resize/move is entirely `pude`'s own job once the window
    // exists (see the constructor's comment) -- this just keeps
    // QPlatformWindow's own bookkeeping (geometry()/QWindow::geometry())
    // in sync when Qt itself requests a programmatic change (e.g. a
    // fixed-size QDialog asking for its exact size before becoming
    // visible), same as every other QPlatformWindow implementation does.
    QPlatformWindow::setGeometry(rect);
    QWindowSystemInterface::handleGeometryChange(window(), rect);

    // Only the primary window's geometry maps to pude's one real window
    // frame -- see the constructor's "primary vs secondary" comment. A
    // secondary (dialog) window calling this (e.g. a QMessageBox sizing
    // itself to fit its own text before showing) must NOT resize pude's
    // actual on-screen window down to the dialog's own small size, which
    // is exactly what happened before this guard existed: clicking
    // "Computer" in pcmanfm-qt's sidebar shrank the whole file manager
    // window down to a tiny error-dialog-sized box (docs/pcmanfm-port.md's
    // 2026-07-21 entry).
    if (!m_isPrimary) {
        return;
    }

    // Tell PUDE for real (docs/qt-port.md's Phase 5 "genuine architectural
    // blocker" -- see PU_QPA_C2S_RESIZE_REQUEST's own comment,
    // user/pureunix_qpa_protocol.h): a layout that decides it needs more
    // room than the window's initial size calls exactly this, and
    // without PUDE ever learning about it, PUDE kept rendering (and
    // bounds-checking incoming damage against) the *original* size
    // forever. Sent unconditionally rather than only when the size
    // actually changed -- PUDE's own handling is already idempotent
    // (qtclient_state_t's resize_content() no-ops on an unchanged size),
    // so there's no benefit to this file also tracking "did it change".
    pu_qpa_size_t sz;
    sz.w = rect.width();
    sz.h = rect.height();
    m_integration->sendMessage(PU_QPA_C2S_RESIZE_REQUEST, &sz, sizeof(sz));
}

void QPureUnixWindow::setVisible(bool visible)
{
    m_visible = visible;
    if (visible) {
        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(0, 0), geometry().size()));
    }
    QPlatformWindow::setVisible(visible);
}

void QPureUnixWindow::setWindowTitle(const QString &title)
{
    // A secondary (dialog) window's own title (e.g. a QMessageBox's
    // "Error") must not overwrite pude's real titlebar text, which
    // belongs to the primary window -- see the constructor's comment.
    if (!m_isPrimary) {
        return;
    }
    QByteArray utf8 = title.toUtf8();
    m_integration->sendMessage(PU_QPA_C2S_SET_TITLE, utf8.constData(), (uint32_t)utf8.size());
}

void QPureUnixWindow::requestActivateWindow()
{
    QWindowSystemInterface::handleWindowActivated(window(), Qt::OtherFocusReason);
}

// Maps PU_QPA_KEY_* (user/pureunix_qpa_protocol.h) to Qt::Key -- the one
// real translation table this plugin needs (see that header's own
// comment on why the wire format uses this portable enum rather than a
// raw SDL_Scancode).
static int mapSpecialKey(int32_t code)
{
    switch (code) {
    case PU_QPA_KEY_RETURN: return Qt::Key_Return;
    case PU_QPA_KEY_BACKSPACE: return Qt::Key_Backspace;
    case PU_QPA_KEY_TAB: return Qt::Key_Tab;
    case PU_QPA_KEY_ESCAPE: return Qt::Key_Escape;
    case PU_QPA_KEY_UP: return Qt::Key_Up;
    case PU_QPA_KEY_DOWN: return Qt::Key_Down;
    case PU_QPA_KEY_LEFT: return Qt::Key_Left;
    case PU_QPA_KEY_RIGHT: return Qt::Key_Right;
    case PU_QPA_KEY_HOME: return Qt::Key_Home;
    case PU_QPA_KEY_END: return Qt::Key_End;
    case PU_QPA_KEY_PAGEUP: return Qt::Key_PageUp;
    case PU_QPA_KEY_PAGEDOWN: return Qt::Key_PageDown;
    case PU_QPA_KEY_DELETE: return Qt::Key_Delete;
    case PU_QPA_KEY_INSERT: return Qt::Key_Insert;
    case PU_QPA_KEY_F1: return Qt::Key_F1;
    case PU_QPA_KEY_F2: return Qt::Key_F2;
    case PU_QPA_KEY_F3: return Qt::Key_F3;
    case PU_QPA_KEY_F4: return Qt::Key_F4;
    case PU_QPA_KEY_F5: return Qt::Key_F5;
    case PU_QPA_KEY_F6: return Qt::Key_F6;
    case PU_QPA_KEY_F7: return Qt::Key_F7;
    case PU_QPA_KEY_F8: return Qt::Key_F8;
    case PU_QPA_KEY_F9: return Qt::Key_F9;
    case PU_QPA_KEY_F10: return Qt::Key_F10;
    case PU_QPA_KEY_F11: return Qt::Key_F11;
    case PU_QPA_KEY_F12: return Qt::Key_F12;
    default: return 0;
    }
}

static Qt::MouseButton mapButton(uint8_t b)
{
    switch (b) {
    case 0: return Qt::LeftButton;
    case 1: return Qt::MiddleButton;
    case 2: return Qt::RightButton;
    default: return Qt::NoButton;
    }
}

void QPureUnixWindow::handleProtocolMessage(uint32_t type, const QByteArray &payload)
{
    // Held-mouse-buttons state persists across calls (Qt's own
    // handleMouseEvent() wants the *full current set*, not just which
    // one changed) -- a plain function-local static is safe here since
    // there is only ever one active window/one client process (see
    // docs/qt-port.md Phase 6 scope).
    static Qt::MouseButtons heldButtons = Qt::NoButton;

    switch (type) {
    case PU_QPA_S2C_RESIZE: {
        if (payload.size() < (int)sizeof(pu_qpa_size_t)) return;
        pu_qpa_size_t sz;
        memcpy(&sz, payload.constData(), sizeof(sz));
        QRect rect(geometry().topLeft(), QSize(sz.w, sz.h));
        QPlatformWindow::setGeometry(rect);
        QWindowSystemInterface::handleGeometryChange(window(), rect);
        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(0, 0), rect.size()));
        break;
    }
    case PU_QPA_S2C_CLOSE: {
        m_created = false; // pude already knows -- don't echo PU_QPA_C2S_CLOSE back
        QWindowSystemInterface::handleCloseEvent(window());
        break;
    }
    case PU_QPA_S2C_KEY: {
        if (payload.size() < (int)sizeof(pu_qpa_key_t)) return;
        pu_qpa_key_t k;
        memcpy(&k, payload.constData(), sizeof(k));
        Qt::KeyboardModifiers mods = Qt::NoModifier;
        if (k.shift) mods |= Qt::ShiftModifier;
        if (k.ctrl) mods |= Qt::ControlModifier;
        int qtKey = mapSpecialKey(k.key_code);
        QString text;
        if (qtKey == 0) {
            if (k.ascii_char != 0) {
                qtKey = Qt::Key(QChar(QLatin1Char((char)k.ascii_char)).toUpper().unicode());
                if (k.down) {
                    text = QString(QLatin1Char((char)k.ascii_char));
                }
            }
        }
        QEvent::Type evType = k.down ? QEvent::KeyPress : QEvent::KeyRelease;
        QWindowSystemInterface::handleKeyEvent(window(), evType, qtKey, mods, text);
        break;
    }
    case PU_QPA_S2C_MOUSE_MOVE: {
        if (payload.size() < (int)sizeof(pu_qpa_point_t)) return;
        pu_qpa_point_t p;
        memcpy(&p, payload.constData(), sizeof(p));
        QPointF local(p.x, p.y);
        QWindowSystemInterface::handleMouseEvent(window(), local, local, heldButtons,
                                                  Qt::NoButton, QEvent::MouseMove);
        break;
    }
    case PU_QPA_S2C_MOUSE_BUTTON: {
        if (payload.size() < (int)sizeof(pu_qpa_mouse_button_t)) return;
        pu_qpa_mouse_button_t m;
        memcpy(&m, payload.constData(), sizeof(m));
        Qt::MouseButton btn = mapButton(m.button);
        if (m.down) {
            heldButtons |= btn;
        } else {
            heldButtons &= ~btn;
        }
        QPointF local(m.x, m.y);
        QWindowSystemInterface::handleMouseEvent(window(), local, local, heldButtons, btn,
                                                  m.down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease);
        break;
    }
    case PU_QPA_S2C_MOUSE_WHEEL: {
        if (payload.size() < (int)sizeof(pu_qpa_wheel_t)) return;
        pu_qpa_wheel_t w;
        memcpy(&w, payload.constData(), sizeof(w));
        QPointF local(w.x, w.y);
        QWindowSystemInterface::handleWheelEvent(window(), local, local, QPoint(0, 0),
                                                  QPoint(0, w.delta * 120));
        break;
    }
    case PU_QPA_S2C_FOCUS: {
        if (payload.size() < 1) return;
        bool gained = payload.at(0) != 0;
        if (gained) {
            QWindowSystemInterface::handleWindowActivated(window(), Qt::ActiveWindowFocusReason);
        } else {
            QWindowSystemInterface::handleWindowActivated(nullptr, Qt::ActiveWindowFocusReason);
        }
        break;
    }
    default:
        break;
    }
}
