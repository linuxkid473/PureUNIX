#ifndef PUDE_LAUNCH_H
#define PUDE_LAUNCH_H

/* pude_launch_foreground() -- the one general-purpose mechanism any pude
 * app uses to hand the whole screen to a real external program (e.g.
 * PUFiles opening a .png with imgview) without corrupting desktop
 * ownership. See user/pude_launch.c for the design rationale and
 * docs/pude.md's "Launching a foreground program" section. */

/* Forks, execve()s `path` with `argv` (NULL-terminated, argv[0] by POSIX
 * convention), and blocks the calling pude process until the child exits
 * -- the caller's own event loop (user/pude.c's main loop) is not pumped
 * at all while this runs, so it can never race the child for input or
 * paint over its output. Returns 0 once the child has exited (regardless
 * of its exit status -- the child is responsible for reporting its own
 * errors, e.g. imgview prints to a still-visible console on a bad file),
 * or -1 if fork() itself failed (nothing was ever launched). */
int pude_launch_foreground(const char *path, char *const argv[]);

#endif
