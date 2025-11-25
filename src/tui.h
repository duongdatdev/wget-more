#ifndef TUI_H
#define TUI_H

#include "wget.h"

typedef struct {
    char **urls;
    int count;
} TuiResult;

TuiResult *tui_get_info(void);
char *tui_get_url(void);

void *tui_progress_create (const char *, wgint, wgint);
void tui_progress_update (void *, wgint, double);
void tui_progress_draw (void *);
void tui_progress_finish (void *, double);
void tui_progress_set_params (const char *);

#endif /* TUI_H */
