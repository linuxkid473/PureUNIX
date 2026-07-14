/* user/pude_launch.c -- see pude_launch.h.
 *
 * Why a blocking fork+exec is the *correct* design here, not a shortcut:
 * every pude app's own render loop is driven by user/pude.c's top-level
 * SDL_PollEvent() loop, which (SDL_puevents.c's PUREUNIX_PumpEvents())
 * drains the same raw input queue (SYS_INPUT_POLL) a directly-launched
 * fullscreen program like imgview also reads from directly. If pude kept
 * calling SDL_PollEvent()/redrawing its own desktop while such a child
 * was alive, the two processes would race for the same keyboard/mouse
 * events, and pude's own SDL_UpdateWindowSurface() would periodically
 * blit its stale desktop surface right over the child's freshly drawn
 * frame. Blocking the WM's *entire* process (not just the calling
 * window's app logic) in a real waitpid() until the child exits removes
 * both races at the source, and costs nothing extra: a fullscreen program
 * legitimately owns the whole display until it exits, so there is nothing
 * useful for any other pude window to be doing on screen in the meantime
 * anyway.
 *
 * This is also exactly what kernel/vt.c's graphics_owner_stack (added for
 * Chocolate Doom, see docs/pude.md) already exists to support: pude's own
 * SYS_SET_GRAPHICS_MODE(1) call (made once, at SDL_Init(), by
 * SDL_puvideo.c) makes it the VT's depth-0 graphics owner. The forked
 * child calling SYS_SET_GRAPHICS_MODE(1) again just pushes a nested
 * owner; its own SYS_SET_GRAPHICS_MODE(0) on exit pops back to pude
 * without ever touching the hardware graphics-mode flag (see
 * vt_set_graphics_mode()'s "had_prev" branch) -- pude resumes owning the
 * screen automatically, it just needs to actually repaint once, which
 * happens for free the next time its own event loop notices `had_event`
 * (this call happens from inside an on_mouse_down/up dispatch, itself
 * already inside an event being processed) and calls render_frame().
 */
#include "pude_launch.h"
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

int pude_launch_foreground(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execve(path, argv, environ);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    return 0;
}
