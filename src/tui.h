#ifndef TUI_H
#define TUI_H

#include "wget.h"

// Checksum types for verification
typedef enum {
    TUI_CHECKSUM_NONE = 0,
    TUI_CHECKSUM_MD5,
    TUI_CHECKSUM_SHA256
} TuiChecksumType;

typedef struct {
    char **urls;
    int count;
    TuiChecksumType checksum_type;      // Type of checksum to calculate
    char **expected_checksums;           // Array of expected checksums (parallel to urls)
} TuiResult;

TuiResult *tui_get_info(void);
char *tui_get_url(void);

void *tui_progress_create (const char *, wgint, wgint);
void *tui_progress_create_with_checksum(const char *filename, const char *filepath, 
                                         wgint initial, wgint total,
                                         TuiChecksumType checksum_type, 
                                         const char *expected_checksum);
void tui_progress_update (void *, wgint, double);
void tui_progress_draw (void *);
void tui_progress_finish (void *, double);
void tui_progress_finish_with_checksum(void *bar_ptr, double time_taken);
void tui_progress_set_params (const char *);
void tui_progress_set_filepath(void *bar_ptr, const char *filepath);
bool tui_is_active(void);
void tui_cleanup(void);
void tui_wait_for_completion(void);
int tui_get_active_count(void);

// Checksum utility functions
const char *tui_get_checksum(void *bar_ptr);
bool tui_is_checksum_verified(void *bar_ptr);

#endif /* TUI_H */
