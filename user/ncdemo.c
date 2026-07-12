/* ncdemo — a real interactive ncurses application (built against the real
 * upstream libncurses.a vendored under third_party/ncurses/) exercising the
 * "pureunix" terminfo entry's full surface: colors, bordered/nested windows,
 * keyboard input (arrows, function keys), full-screen redraw, SIGWINCH, and
 * clean terminal restoration on exit. See docs/ncurses-port.md.
 */
#include <curses.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *const ITEMS[] = {
    "Colors demo",
    "Cursor keys demo",
    "Reverse/bold attribute demo",
    "Quit",
};
#define NUM_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

static volatile sig_atomic_t g_winch;

static void on_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}

static void apply_resize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        resizeterm(ws.ws_row, ws.ws_col);
    }
    clear();
}

static void draw_frame(WINDOW *win, int sel)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " ncdemo ");
    for (int i = 0; i < NUM_ITEMS; ++i) {
        if (i == sel) {
            wattron(win, A_REVERSE);
        }
        mvwprintw(win, 2 + i, 2, "%-28s", ITEMS[i]);
        if (i == sel) {
            wattroff(win, A_REVERSE);
        }
    }
    mvwprintw(win, 2 + NUM_ITEMS + 1, 2, "Up/Down + Enter, q to quit");
    mvwprintw(win, 2 + NUM_ITEMS + 2, 2, "%dx%d", LINES, COLS);
    wrefresh(win);
}

static void colors_demo(WINDOW *win)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " colors ");
    if (has_colors()) {
        for (int i = 0; i < 8; ++i) {
            init_pair((short)(i + 1), (short)i, COLOR_BLACK);
            wattron(win, COLOR_PAIR(i + 1));
            mvwprintw(win, 2 + i, 2, "color pair %d", i);
            wattroff(win, COLOR_PAIR(i + 1));
        }
    } else {
        mvwprintw(win, 2, 2, "no color support");
    }
    mvwprintw(win, 12, 2, "press any key");
    wrefresh(win);
    wgetch(win);
}

static void keys_demo(WINDOW *win)
{
    int y = 5, x = 10;
    for (;;) {
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " cursor keys (q to exit) ");
        mvwaddch(win, y, x, 'X');
        wmove(win, y, x);
        wrefresh(win);
        int c = wgetch(win);
        if (c == 'q') {
            return;
        }
        switch (c) {
        case KEY_UP: if (y > 1) y--; break;
        case KEY_DOWN: if (y < getmaxy(win) - 2) y++; break;
        case KEY_LEFT: if (x > 1) x--; break;
        case KEY_RIGHT: if (x < getmaxx(win) - 2) x++; break;
        case KEY_HOME: x = 1; break;
        case KEY_END: x = getmaxx(win) - 2; break;
        default: break;
        }
    }
}

static void attr_demo(WINDOW *win)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " attributes ");
    mvwprintw(win, 2, 2, "normal text");
    wattron(win, A_BOLD);
    mvwprintw(win, 3, 2, "bold text");
    wattroff(win, A_BOLD);
    wattron(win, A_REVERSE);
    mvwprintw(win, 4, 2, "reverse text");
    wattroff(win, A_REVERSE);
    mvwprintw(win, 6, 2, "press any key");
    wrefresh(win);
    wgetch(win);
}

int main(void)
{
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    if (has_colors()) {
        start_color();
    }
    signal(SIGWINCH, on_winch);

    WINDOW *win = newwin(LINES, COLS, 0, 0);
    keypad(win, TRUE);

    int sel = 0;
    draw_frame(win, sel);
    for (;;) {
        int c = wgetch(win);
        if (g_winch) {
            g_winch = 0;
            apply_resize();
            wresize(win, LINES, COLS);
        }
        if (c == KEY_UP) {
            sel = (sel + NUM_ITEMS - 1) % NUM_ITEMS;
        } else if (c == KEY_DOWN) {
            sel = (sel + 1) % NUM_ITEMS;
        } else if (c == 'q' || c == 'Q') {
            break;
        } else if (c == '\n' || c == KEY_ENTER) {
            if (strcmp(ITEMS[sel], "Quit") == 0) {
                break;
            } else if (strcmp(ITEMS[sel], "Colors demo") == 0) {
                colors_demo(win);
            } else if (strcmp(ITEMS[sel], "Cursor keys demo") == 0) {
                keys_demo(win);
            } else if (strcmp(ITEMS[sel], "Reverse/bold attribute demo") == 0) {
                attr_demo(win);
            }
        }
        draw_frame(win, sel);
    }

    delwin(win);
    endwin();
    return 0;
}
