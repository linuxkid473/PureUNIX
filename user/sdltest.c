/* Minimal real SDL2 application (docs/sdl-port.md) -- opens a fullscreen
 * window on the real framebuffer, bounces a colored box around using
 * SDL_GetTicks()-driven timing, reacts to real keyboard (arrow keys/Escape)
 * and mouse (left click) input, and exits cleanly back to the shell on
 * Escape/quit. Exercises every piece of the platform port: video,
 * software rendering via the window surface, events, and timing. */
#include <SDL.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("sdltest", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                           0, 0, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        SDL_Log("SDL_GetWindowSurface failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int w = surface->w;
    int h = surface->h;
    int box_w = w / 12 > 0 ? w / 12 : 8;
    int box_h = h / 12 > 0 ? h / 12 : 8;
    int x = w / 2;
    int y = h / 2;
    int dx = 3, dy = 2;
    Uint32 color = SDL_MapRGB(surface->format, 0, 200, 90);
    Uint32 bg = SDL_MapRGB(surface->format, 10, 10, 40);

    Uint32 frame = 0;
    int quit = 0;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                quit = 1;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = 1;
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_LEFT) {
                    dx -= 1;
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
                    dx += 1;
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_UP) {
                    dy -= 1;
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_DOWN) {
                    dy += 1;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                x = ev.button.x;
                y = ev.button.y;
                break;
            default:
                break;
            }
        }

        x += dx;
        y += dy;
        if (x < 0 || x + box_w > w) {
            dx = -dx;
            x += dx;
        }
        if (y < 0 || y + box_h > h) {
            dy = -dy;
            y += dy;
        }

        SDL_Rect full = { 0, 0, w, h };
        SDL_FillRect(surface, &full, bg);
        SDL_Rect box = { x, y, box_w, box_h };
        SDL_FillRect(surface, &box, color);

        SDL_UpdateWindowSurface(window);

        frame++;
        SDL_Delay(16);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
