#ifndef PUREUNIX_QPA_PROTOCOL_H
#define PUREUNIX_QPA_PROTOCOL_H

/* Wire protocol between an external Qt GUI client process (statically
 * linked against the real, unmodified upstream Qt Gui/Widgets libraries
 * plus the new "pureunix" QPA platform plugin, user/qpa_pureunix/) and
 * `pude` (user/pude_qtclient.c's app_class_t adapter) — the same "spawn
 * with two pipes, PUDE-side adapter is just another app_class_t, no
 * changes to PUDE's core WM loop" design docs/qt-port.md's Phase 1 audit
 * (section 5) already laid out, symmetric to how PUTerm already wires a
 * pty pair to its own forked shell child (user/pude_term.c).
 *
 * This single header is included by BOTH sides (user/pude_qtclient.c,
 * plain C, and every C++ file under user/qpa_pureunix/) so the wire format can
 * never silently drift between them — plain fixed-width types only, no
 * Qt/SDL headers included here (each side already has its own real
 * vocabulary — SDL_Scancode on PUDE's side already matches
 * user/pude_app.h's on_key callback exactly; the QPA plugin maps that
 * same numeric scancode to Qt::Key itself, the one real translation this
 * protocol needs, matching the same spirit as PUTerm's own
 * scancode->byte encode_key()).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Fixed fd numbers the client inherits at exec() time — dup2'd there by
 * PUDE (user/pude_qtclient.c) before execve(), exactly like PUTerm's pty
 * slave dup2'd onto 0/1/2 (user/pude_term.c's spawn_shell()). Chosen
 * distinct from 0/1/2 so a Qt client's own stdio (crash diagnostics,
 * qDebug() output while developing) is left alone. */
#define PUREUNIX_QPA_FD_READ  3 /* PUDE -> client: input/resize/close */
#define PUREUNIX_QPA_FD_WRITE 4 /* client -> PUDE: window create/damage/title/close */

/* Every message on either pipe starts with this fixed 8-byte header;
 * `len` is the payload size in bytes immediately following it (0 for a
 * message with no payload — still sent for uniformity, so the framing
 * parser on either side never needs a special "headerless" case). */
typedef struct pu_qpa_msg_header {
    uint32_t type;
    uint32_t len;
} pu_qpa_msg_header_t;

/* ---- client -> PUDE (PUREUNIX_QPA_FD_WRITE) ---- */
enum {
    /* payload: pu_qpa_window_create_t. Sent once, right after the QPA
     * plugin's QPlatformWindow is constructed. */
    PU_QPA_C2S_WINDOW_CREATE = 1,
    /* payload: pu_qpa_damage_t, immediately followed by w*h*4 raw bytes
     * of ARGB32 (0xAARRGGBB, native byte order — QImage::Format_ARGB32's
     * own in-memory layout on this little-endian target) pixel data for
     * exactly that sub-rectangle of the window's client area. */
    PU_QPA_C2S_DAMAGE = 2,
    /* payload: a title string, NOT NUL-terminated on the wire (length is
     * exactly the header's `len`) — the reader appends its own NUL. */
    PU_QPA_C2S_SET_TITLE = 3,
    /* payload: none. The client's own QPlatformWindow destructor sends
     * this right before the process exits on its own (distinct from
     * PU_QPA_S2C_CLOSE below, which is the WM-initiated direction). */
    PU_QPA_C2S_CLOSE = 4,
};

/* ---- PUDE -> client (PUREUNIX_QPA_FD_READ) ---- */
enum {
    PU_QPA_S2C_RESIZE       = 1, /* payload: pu_qpa_size_t (new client-area size) */
    PU_QPA_S2C_CLOSE        = 2, /* payload: none -- user clicked the WM close button */
    PU_QPA_S2C_KEY          = 3, /* payload: pu_qpa_key_t */
    PU_QPA_S2C_MOUSE_MOVE   = 4, /* payload: pu_qpa_point_t */
    PU_QPA_S2C_MOUSE_BUTTON = 5, /* payload: pu_qpa_mouse_button_t */
    PU_QPA_S2C_MOUSE_WHEEL  = 6, /* payload: pu_qpa_wheel_t */
    PU_QPA_S2C_FOCUS        = 7, /* payload: uint8_t (1 = gained focus, 0 = lost) */
};

typedef struct { int32_t w, h; } pu_qpa_size_t;
typedef struct { int32_t x, y, w, h; } pu_qpa_damage_t;
typedef struct { int32_t x, y; } pu_qpa_point_t;

/* button: 0=left, 1=middle, 2=right (SDL_BUTTON_LEFT/MIDDLE/RIGHT - 1,
 * already the numbering user/pude_app.h's on_mouse_down/up pass through
 * from PUDE's own SDL2 event loop). */
typedef struct { int32_t x, y; uint8_t button; uint8_t down; } pu_qpa_mouse_button_t;

/* delta: positive = away from the user (scroll up), matching SDL2's own
 * SDL_MouseWheelEvent.y sign convention — the one PUDE's own event loop
 * already reads it from. */
typedef struct { int32_t x, y; int32_t delta; } pu_qpa_wheel_t;

/* key_code: a small, portable "special key" enum -- NOT a raw
 * SDL_Scancode (the Qt client never links SDL, so SDL's numeric scancode
 * values would just be unexplained magic numbers on that side). PUDE's
 * on_key callback (user/pude_app.h) translates the SDL_Scancode it
 * receives into this enum via a small switch, exactly the same
 * "SDL_Scancode -> something else" translation user/pude_term.c's own
 * encode_key() already does for pty byte sequences -- PU_QPA_KEY_NONE
 * (0) covers every ordinary printable key, whose actual character
 * already arrives via ascii_char below. ascii_char: PUDE's own real
 * pu_scancode_to_ascii() translation (user/pude_widgets.h, already used
 * by PUTerm/PUText/PUFiles) computed once on the sender side so the Qt
 * client doesn't need its own duplicate keymap/locale logic just to know
 * what text a keystroke produces; 0 means "no printable character". */
enum {
    PU_QPA_KEY_NONE = 0,
    PU_QPA_KEY_RETURN, PU_QPA_KEY_BACKSPACE, PU_QPA_KEY_TAB, PU_QPA_KEY_ESCAPE,
    PU_QPA_KEY_UP, PU_QPA_KEY_DOWN, PU_QPA_KEY_LEFT, PU_QPA_KEY_RIGHT,
    PU_QPA_KEY_HOME, PU_QPA_KEY_END, PU_QPA_KEY_PAGEUP, PU_QPA_KEY_PAGEDOWN,
    PU_QPA_KEY_DELETE, PU_QPA_KEY_INSERT,
    PU_QPA_KEY_F1, PU_QPA_KEY_F2, PU_QPA_KEY_F3, PU_QPA_KEY_F4,
    PU_QPA_KEY_F5, PU_QPA_KEY_F6, PU_QPA_KEY_F7, PU_QPA_KEY_F8,
    PU_QPA_KEY_F9, PU_QPA_KEY_F10, PU_QPA_KEY_F11, PU_QPA_KEY_F12,
};
typedef struct {
    int32_t key_code;
    uint8_t shift, ctrl;
    uint8_t down;
    uint8_t ascii_char;
} pu_qpa_key_t;

typedef struct { int32_t default_w, default_h; } pu_qpa_window_create_t;

#ifdef __cplusplus
}
#endif
#endif
