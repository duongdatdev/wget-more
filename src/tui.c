#include "wget.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include "tui.h"
#include "options.h"

#ifdef HAVE_NETTLE
#include <nettle/md5.h>
#include <nettle/sha2.h>
#endif

static bool tui_initialized = false;
static WINDOW *main_win = NULL;
static pthread_mutex_t tui_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *debug_log = NULL;

// Debug logging function
static void tui_debug(const char *fmt, ...) {
    if (!debug_log) {
        debug_log = fopen("/tmp/tui_debug.log", "a");
        if (!debug_log) return;
        fprintf(debug_log, "\n=== TUI Debug Session Started ===\n");
    }
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[26];
    strftime(time_buf, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(debug_log, "[%s] ", time_buf);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(debug_log, fmt, args);
    va_end(args);
    
    fprintf(debug_log, "\n");
    fflush(debug_log);
}

// Checksum types
typedef enum {
    CHECKSUM_NONE = 0,
    CHECKSUM_MD5,
    CHECKSUM_SHA256
} ChecksumType;

// Forward declarations
typedef struct {
    int id;
    wgint total;
    wgint current;
    double start_time;
    char *filename;
    char *filepath;         // Full path to downloaded file
    bool active;
    ChecksumType checksum_type;
    char checksum[65];      // Stores calculated checksum (64 hex chars + null for SHA256)
    char expected_checksum[65]; // Expected checksum from user
    bool checksum_verified; // true if checksum matches expected
    bool checksum_calculated;
} TuiProgress;

static TuiProgress **bars = NULL;
static int bar_count = 0;

static void tui_cleanup_handler(void) {
    if (tui_initialized) {
        endwin();
    }
}

static void init_ncurses_base() {
    tui_debug("init_ncurses_base called, tui_initialized=%d", tui_initialized);
    if (tui_initialized) return;
    
    initscr();
    start_color();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    if (has_colors()) {
        init_pair(1, COLOR_CYAN, COLOR_BLACK);  // Info text
        init_pair(2, COLOR_GREEN, COLOR_BLACK); // Progress bar
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);// Pause/Warning
        init_pair(4, COLOR_WHITE, COLOR_BLUE);  // Header
        init_pair(5, COLOR_RED, COLOR_BLACK);   // Brackets
    }
    atexit(tui_cleanup_handler);
    tui_initialized = true;
    tui_debug("init_ncurses_base completed");
}

// Helper function to convert bytes to hex string
static void bytes_to_hex(const unsigned char *bytes, int len, char *hex_str) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        hex_str[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_str[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_str[len * 2] = '\0';
}

// Calculate MD5 checksum of a file
static bool calculate_md5(const char *filepath, char *result) {
#ifdef HAVE_NETTLE
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        tui_debug("calculate_md5: failed to open file %s: %s", filepath, strerror(errno));
        return false;
    }
    
    struct md5_ctx ctx;
    md5_init(&ctx);
    
    unsigned char buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        md5_update(&ctx, bytes_read, buffer);
    }
    
    fclose(file);
    
    unsigned char digest[MD5_DIGEST_SIZE];
    md5_digest(&ctx, MD5_DIGEST_SIZE, digest);
    
    bytes_to_hex(digest, MD5_DIGEST_SIZE, result);
    tui_debug("calculate_md5: %s -> %s", filepath, result);
    return true;
#else
    tui_debug("calculate_md5: Nettle not available");
    strcpy(result, "N/A (Nettle not available)");
    return false;
#endif
}

// Calculate SHA256 checksum of a file
static bool calculate_sha256(const char *filepath, char *result) {
#ifdef HAVE_NETTLE
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        tui_debug("calculate_sha256: failed to open file %s: %s", filepath, strerror(errno));
        return false;
    }
    
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    
    unsigned char buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sha256_update(&ctx, bytes_read, buffer);
    }
    
    fclose(file);
    
    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_digest(&ctx, SHA256_DIGEST_SIZE, digest);
    
    bytes_to_hex(digest, SHA256_DIGEST_SIZE, result);
    tui_debug("calculate_sha256: %s -> %s", filepath, result);
    return true;
#else
    tui_debug("calculate_sha256: Nettle not available");
    strcpy(result, "N/A (Nettle not available)");
    return false;
#endif
}

// Calculate checksum based on type
static bool calculate_checksum(const char *filepath, ChecksumType type, char *result) {
    switch (type) {
        case CHECKSUM_MD5:
            return calculate_md5(filepath, result);
        case CHECKSUM_SHA256:
            return calculate_sha256(filepath, result);
        default:
            result[0] = '\0';
            return false;
    }
}

// Verify checksum against expected value
static bool verify_checksum(const char *calculated, const char *expected) {
    if (!calculated || !expected || strlen(expected) == 0) {
        return false;
    }
    // Case-insensitive comparison
    return strcasecmp(calculated, expected) == 0;
}

static void ensure_main_win() {
    tui_debug("ensure_main_win called, main_win=%p", (void*)main_win);
    if (main_win) return;
    
    int height, width;
    getmaxyx(stdscr, height, width);
    main_win = newwin(height, width, 0, 0);
    box(main_win, 0, 0);
    
    if (has_colors()) wattron(main_win, COLOR_PAIR(4) | A_BOLD);
    mvwhline(main_win, 1, 1, ' ', width - 2);
    mvwprintw(main_win, 1, 2, " GNU Wget - Multi-threaded TUI Downloader ");
    if (has_colors()) wattroff(main_win, COLOR_PAIR(4) | A_BOLD);
    wrefresh(main_win);
    tui_debug("ensure_main_win completed, main_win=%p, height=%d, width=%d", (void*)main_win, height, width);
}

TuiResult *tui_get_info(void) {
    tui_debug("tui_get_info called");
    init_ncurses_base();

    int height, width;
    getmaxyx(stdscr, height, width);
    WINDOW *win = newwin(20, 80, (height - 20) / 2, (width - 80) / 2);
    keypad(win, TRUE);

    char **urls = NULL;
    int count = 0;
    char input_buf[1024] = {0};

    while (1) {
        werase(win);
        box(win, 0, 0);
        
        if (has_colors()) wattron(win, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(win, 1, 2, " Wget Batch Downloader ");
        if (has_colors()) wattroff(win, COLOR_PAIR(4) | A_BOLD);

        mvwprintw(win, 3, 2, "URL List (Threads: %d):", opt.connections);
        for (int i = 0; i < 10; i++) {
            if (i < count) {
                mvwprintw(win, 4 + i, 4, "%d. %-60.60s", i + 1, urls[i]);
            } else {
                mvwprintw(win, 4 + i, 4, "-");
            }
        }

        mvwprintw(win, 15, 2, "Controls: [a] Add URL  [d] Delete Last  [s] Start Download  [q] Quit");
        
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 'q') {
            delwin(win);
            endwin();
            exit(0);
        } else if (ch == 'a') {
            if (count < 10) {
                mvwprintw(win, 17, 2, "Enter URL: ");
                echo();
                if (has_colors()) wattron(win, COLOR_PAIR(2));
                wgetnstr(win, input_buf, 1023);
                if (has_colors()) wattroff(win, COLOR_PAIR(2));
                noecho();
                
                if (strlen(input_buf) > 0) {
                    urls = realloc(urls, sizeof(char*) * (count + 1));
                    urls[count] = strdup(input_buf);
                    count++;
                }
            }
        } else if (ch == 'd') {
            if (count > 0) {
                count--;
                free(urls[count]);
            }
        } else if (ch == 's') {
            if (count > 0) {
                // Show feedback before returning
                werase(win);
                box(win, 0, 0);
                if (has_colors()) wattron(win, COLOR_PAIR(4) | A_BOLD);
                mvwprintw(win, 1, 2, " Wget Batch Downloader ");
                if (has_colors()) wattroff(win, COLOR_PAIR(4) | A_BOLD);
                
                mvwprintw(win, 10, 25, "Initializing download...");
                mvwprintw(win, 11, 25, "Please wait while connecting...");
                wrefresh(win);
                
                // Small delay to show the message
                napms(500);
                break;
            }
        }
    }

    // Clean up the input window
    delwin(win);
    
    tui_debug("tui_get_info: pressed 's', count=%d", count);
    
    // Create the main progress window immediately
    clear();
    refresh();
    ensure_main_win();
    
    tui_debug("tui_get_info: main_win created, returning result");
    
    TuiResult *res = malloc(sizeof(TuiResult));
    res->urls = urls;
    res->count = count;
    return res;
}

char *tui_get_url(void) {
    return NULL;
}

void *tui_progress_create (const char *f_name, wgint initial, wgint total) {
    tui_debug("tui_progress_create called: file=%s, initial=%lld, total=%lld", f_name, (long long)initial, (long long)total);
    pthread_mutex_lock(&tui_mutex);
    
    tui_debug("tui_progress_create: got mutex, tui_initialized=%d", tui_initialized);
    if (!tui_initialized) init_ncurses_base();
    ensure_main_win();

    TuiProgress *bar = malloc(sizeof(TuiProgress));
    bar->total = total;
    bar->current = initial;
    bar->start_time = (double)time(NULL);
    bar->filename = strdup(f_name);
    bar->active = true;
    
    int slot = -1;
    for (int i = 0; i < bar_count; i++) {
        if (bars[i] == NULL) {
            slot = i;
            bars[i] = bar;
            break;
        }
        // Reuse finished slots
        if (!bars[i]->active) {
            slot = i;
            free(bars[i]->filename);
            free(bars[i]);
            bars[i] = bar;
            break;
        }
    }
    if (slot == -1) {
        bar_count++;
        bars = realloc(bars, sizeof(TuiProgress*) * bar_count);
        bars[bar_count - 1] = bar;
        slot = bar_count - 1;
    }
    bar->id = slot;

    pthread_mutex_unlock(&tui_mutex);

    // Force initial draw to show the bar immediately
    tui_progress_draw(bar);

    return bar;
}

void tui_progress_draw (void *bar_ptr) {
    pthread_mutex_lock(&tui_mutex);
    
    if (!main_win) {
        pthread_mutex_unlock(&tui_mutex);
        return;
    }

    int height, width;
    getmaxyx(main_win, height, width);

    // Count actually active downloads
    int active_count = 0;
    for (int i = 0; i < bar_count; i++) {
        if (bars[i] && bars[i]->active) {
            active_count++;
        }
    }

    // Redraw header to ensure it persists
    box(main_win, 0, 0);
    if (has_colors()) wattron(main_win, COLOR_PAIR(4) | A_BOLD);
    mvwhline(main_win, 1, 1, ' ', width - 2);
    mvwprintw(main_win, 1, 2, " GNU Wget - Multi-threaded TUI Downloader (Active: %d) ", active_count);
    if (has_colors()) wattroff(main_win, COLOR_PAIR(4) | A_BOLD);

    // Iterate over ALL bars to ensure they are all visible
    for (int i = 0; i < bar_count; i++) {
        TuiProgress *bar = bars[i];
        if (bar == NULL) continue;

        int row = 3 + (bar->id * 4);
        if (row + 3 >= height) continue;

        // Clear area for this bar
        for(int k=0; k<3; k++) mvwhline(main_win, row+k, 1, ' ', width-2);

        mvwprintw(main_win, row, 2, "File: %s (ID: %d)", bar->filename, bar->id);

        if (!bar->active) {
            // Draw finished state
            if (has_colors()) wattron(main_win, COLOR_PAIR(2) | A_BOLD);
            mvwprintw(main_win, row + 1, 2, "[ DONE ]");
            mvwprintw(main_win, row + 2, 2, "Download Complete");
            if (has_colors()) wattroff(main_win, COLOR_PAIR(2) | A_BOLD);
            continue;
        }

        double pct = 0;
        if (bar->total > 0) pct = (double)bar->current / bar->total;

        // Draw bar
        int bar_width = width - 4;
        if (has_colors()) wattron(main_win, COLOR_PAIR(5));
        mvwprintw(main_win, row + 1, 2, "[");
        mvwprintw(main_win, row + 1, 2 + bar_width - 1, "]");
        if (has_colors()) wattroff(main_win, COLOR_PAIR(5));

        int filled = (int)(pct * (bar_width - 2));
        if (has_colors()) wattron(main_win, COLOR_PAIR(2));
        for (int j = 0; j < bar_width - 2; j++) {
            if (j < filled) mvwaddch(main_win, row + 1, 3 + j, ACS_CKBOARD);
            else mvwaddch(main_win, row + 1, 3 + j, ' ');
        }
        if (has_colors()) wattroff(main_win, COLOR_PAIR(2));

        // Stats
        double now = (double)time(NULL);
        double elapsed = now - bar->start_time;
        double speed = 0;
        if (elapsed > 0) speed = bar->current / elapsed;
        
        if (has_colors()) wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, row + 2, 2, "%.1f%%  %.2f KB/s", pct * 100, speed / 1024);
        
        if (speed > 0 && bar->total > 0) {
            double eta = (bar->total - bar->current) / speed;
            int h = (int)eta / 3600;
            int m = ((int)eta % 3600) / 60;
            int s = (int)eta % 60;
            mvwprintw(main_win, row + 2, 40, "ETA: %02d:%02d:%02d", h, m, s);
        }
        if (has_colors()) wattroff(main_win, COLOR_PAIR(1));
    }

    wrefresh(main_win);
    pthread_mutex_unlock(&tui_mutex);
}

void tui_progress_update (void *bar_ptr, wgint howmuch, double time_taken) {
    pthread_mutex_lock(&tui_mutex);
    TuiProgress *bar = (TuiProgress*)bar_ptr;
    if (bar) {
        bar->current += howmuch;
    }
    pthread_mutex_unlock(&tui_mutex);
}

void tui_progress_finish (void *bar_ptr, double time_taken) {
    pthread_mutex_lock(&tui_mutex);
    TuiProgress *bar = (TuiProgress*)bar_ptr;
    
    if (bar) {
        bar->active = false;
        bar->current = bar->total; // Ensure 100%
    }
    
    pthread_mutex_unlock(&tui_mutex);
    
    // Force redraw to show "Done"
    tui_progress_draw(bar);
}

void tui_progress_set_params (const char *params) {
    // Not used
}

bool tui_is_active(void) {
    pthread_mutex_lock(&tui_mutex);
    bool active = tui_initialized && bar_count > 0;
    pthread_mutex_unlock(&tui_mutex);
    return active;
}

int tui_get_active_count(void) {
    int active = 0;
    pthread_mutex_lock(&tui_mutex);
    for (int i = 0; i < bar_count; i++) {
        if (bars[i] && bars[i]->active) {
            active++;
        }
    }
    pthread_mutex_unlock(&tui_mutex);
    return active;
}

void tui_cleanup(void) {
    pthread_mutex_lock(&tui_mutex);
    
    // Free all progress bars
    for (int i = 0; i < bar_count; i++) {
        if (bars[i]) {
            if (bars[i]->filename) {
                free(bars[i]->filename);
            }
            free(bars[i]);
            bars[i] = NULL;
        }
    }
    if (bars) {
        free(bars);
        bars = NULL;
    }
    bar_count = 0;
    
    // Clean up ncurses
    if (main_win) {
        delwin(main_win);
        main_win = NULL;
    }
    
    if (tui_initialized) {
        endwin();
        tui_initialized = false;
    }
    
    pthread_mutex_unlock(&tui_mutex);
}

void tui_wait_for_completion(void) {
    pthread_mutex_lock(&tui_mutex);
    
    if (!main_win || !tui_initialized) {
        pthread_mutex_unlock(&tui_mutex);
        return;
    }
    
    int height, width;
    getmaxyx(main_win, height, width);
    
    // Show completion message and wait for key press
    if (has_colors()) wattron(main_win, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(main_win, height - 3, 2, "All downloads completed! Press any key to exit (auto-exit in 5s)...");
    if (has_colors()) wattroff(main_win, COLOR_PAIR(2) | A_BOLD);
    
    wrefresh(main_win);
    pthread_mutex_unlock(&tui_mutex);
    
    // Set timeout of 5 seconds - auto exit if no key pressed
    wtimeout(main_win, 5000);
    keypad(main_win, TRUE);
    wgetch(main_win);
    
    // Cleanup TUI
    tui_cleanup();
}