#ifndef PUDE_CALC_H
#define PUDE_CALC_H

/* A real ring-3 GUI calculator app for `pude` (docs/pude.md), plugged in
 * via the generic app_class_t (user/pude_app.h) exactly like PUTerm
 * (user/pude_term.h) -- see user/pude_calc.c for the implementation.
 * Digits, decimal point, add/subtract/multiply/divide, equals, and clear,
 * all mouse-driven; re-lays out its button grid on resize instead of
 * bitmap-scaling a fixed rendering. */
#include "pude_app.h"

extern const app_class_t calc_app_class;

#endif
