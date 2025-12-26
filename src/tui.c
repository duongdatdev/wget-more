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

// Pause and Cancel state
static volatile bool tui_paused = false;
static volatile bool tui_cancelled = false;
static pthread_t input_thread;
static volatile bool input_thread_running = false;

// Scroll state for progress bar list
static volatile int scroll_offset = 0;
static int visible_bars = 0;  // Number of bars that can fit on screen

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

// Structure to store completed/merged file info for checksum
typedef struct {
    char *filename;         // Display name
    char *filepath;         // Full path to merged file
    char checksum[65];      // Calculated checksum
    char expected_checksum[65]; // Expected checksum from user
    bool checksum_calculated;
    bool checksum_verified;
} CompletedFile;

static CompletedFile *completed_files = NULL;
static int completed_file_count = 0;

// Pause/Cancel getter functions
bool tui_is_paused(void) {
    return tui_paused;
}

bool tui_is_cancelled(void) {
    return tui_cancelled;
}

void tui_set_paused(bool paused) {
    tui_paused = paused;
}

void tui_set_cancelled(bool cancelled) {
    tui_cancelled = cancelled;
}

// Forward declare bar_count for use in input handler
static int bar_count;

// Input handler thread function
static void *tui_input_handler(void *arg) {
    (void)arg;
    
    while (input_thread_running && !tui_cancelled) {
        pthread_mutex_lock(&tui_mutex);
        if (main_win) {
            nodelay(main_win, TRUE);
            int ch = wgetch(main_win);
            nodelay(main_win, FALSE);
            
            if (ch != ERR) {
                switch (ch) {
                    case 'p':
                    case 'P':
                        tui_paused = !tui_paused;
                        break;
                    case 'c':
                    case 'C':
                    case 27:  // ESC
                        tui_cancelled = true;
                        tui_paused = false;  // Unpause to allow cleanup
                        break;
                    case 'j':
                    case 'J':
                    case KEY_DOWN:
                        // Scroll down
                        if (bar_count > visible_bars && scroll_offset < bar_count - visible_bars) {
                            scroll_offset++;
                        }
                        break;
                    case 'k':
                    case 'K':
                    case KEY_UP:
                        // Scroll up
                        if (scroll_offset > 0) {
                            scroll_offset--;
                        }
                        break;
                }
            }
        }
        pthread_mutex_unlock(&tui_mutex);
        
        usleep(100000);  // 100ms polling interval
    }
    
    return NULL;
}

// Start input handler thread
void tui_start_input_handler(void) {
    if (!input_thread_running) {
        input_thread_running = true;
        pthread_create(&input_thread, NULL, tui_input_handler, NULL);
    }
}

// Stop input handler thread
void tui_stop_input_handler(void) {
    if (input_thread_running) {
        input_thread_running = false;
        pthread_join(input_thread, NULL);
    }
}

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

// Register a completed/merged file for checksum verification
void tui_register_completed_file(const char *filename, const char *filepath) {
    if (!filename || !filepath) return;
    
    pthread_mutex_lock(&tui_mutex);
    tui_debug("tui_register_completed_file: %s -> %s", filename, filepath);
    
    // Check if already registered
    for (int i = 0; i < completed_file_count; i++) {
        if (completed_files[i].filepath && strcmp(completed_files[i].filepath, filepath) == 0) {
            pthread_mutex_unlock(&tui_mutex);
            return; // Already registered
        }
    }
    
    completed_files = realloc(completed_files, sizeof(CompletedFile) * (completed_file_count + 1));
    CompletedFile *cf = &completed_files[completed_file_count];
    cf->filename = strdup(filename);
    cf->filepath = strdup(filepath);
    cf->checksum[0] = '\0';
    cf->expected_checksum[0] = '\0';
    cf->checksum_calculated = false;
    cf->checksum_verified = false;
    completed_file_count++;
    
    pthread_mutex_unlock(&tui_mutex);
}

// Get count of completed files
int tui_get_completed_file_count(void) {
    return completed_file_count;
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
                wclrtoeol(win);
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
    res->checksum_type = TUI_CHECKSUM_NONE;
    res->expected_checksums = NULL;
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
    bar->filepath = NULL;
    bar->active = true;
    bar->checksum_type = CHECKSUM_NONE;
    bar->checksum[0] = '\0';
    bar->expected_checksum[0] = '\0';
    bar->checksum_verified = false;
    bar->checksum_calculated = false;
    
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
            if (bars[i]->filename) free(bars[i]->filename);
            if (bars[i]->filepath) free(bars[i]->filepath);
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

// Create progress bar with checksum support
void *tui_progress_create_with_checksum(const char *filename, const char *filepath, 
                                         wgint initial, wgint total,
                                         TuiChecksumType checksum_type, 
                                         const char *expected_checksum) {
    tui_debug("tui_progress_create_with_checksum: file=%s, path=%s, checksum_type=%d", 
              filename, filepath ? filepath : "NULL", checksum_type);
    
    void *bar_ptr = tui_progress_create(filename, initial, total);
    TuiProgress *bar = (TuiProgress *)bar_ptr;
    
    if (bar) {
        pthread_mutex_lock(&tui_mutex);
        bar->checksum_type = (ChecksumType)checksum_type;
        if (filepath) {
            bar->filepath = strdup(filepath);
        }
        if (expected_checksum && strlen(expected_checksum) > 0) {
            strncpy(bar->expected_checksum, expected_checksum, 64);
            bar->expected_checksum[64] = '\0';
        }
        pthread_mutex_unlock(&tui_mutex);
    }
    
    return bar_ptr;
}

// Set filepath for checksum calculation
void tui_progress_set_filepath(void *bar_ptr, const char *filepath) {
    if (!bar_ptr || !filepath) return;
    
    pthread_mutex_lock(&tui_mutex);
    TuiProgress *bar = (TuiProgress *)bar_ptr;
    if (bar->filepath) {
        free(bar->filepath);
    }
    bar->filepath = strdup(filepath);
    tui_debug("tui_progress_set_filepath: id=%d, path=%s", bar->id, filepath);
    pthread_mutex_unlock(&tui_mutex);
}

void tui_progress_draw (void *bar_ptr) {
    pthread_mutex_lock(&tui_mutex);
    
    if (!main_win) {
        pthread_mutex_unlock(&tui_mutex);
        return;
    }

    int height, width;
    getmaxyx(main_win, height, width);

    // Calculate how many bars can fit on screen
    // Each bar needs 4 rows, header takes 3 rows (0-2), footer takes 2 rows
    int available_rows = height - 5;  // header (3) + footer (2)
    visible_bars = available_rows / 4;
    if (visible_bars < 1) visible_bars = 1;

    // Count actually active downloads
    int active_count = 0;
    for (int i = 0; i < bar_count; i++) {
        if (bars[i] && bars[i]->active) {
            active_count++;
        }
    }

    // Adjust scroll offset if needed (e.g., if items were removed)
    if (bar_count <= visible_bars) {
        scroll_offset = 0;
    } else if (scroll_offset > bar_count - visible_bars) {
        scroll_offset = bar_count - visible_bars;
    }
    if (scroll_offset < 0) scroll_offset = 0;

    // Redraw header to ensure it persists
    box(main_win, 0, 0);
    if (has_colors()) wattron(main_win, COLOR_PAIR(4) | A_BOLD);
    mvwhline(main_win, 1, 1, ' ', width - 2);
    
    // Show pause/cancel status in header
    if (tui_cancelled) {
        mvwprintw(main_win, 1, 2, " GNU Wget - TUI Downloader [CANCELLING...] ");
    } else if (tui_paused) {
        if (has_colors()) wattroff(main_win, COLOR_PAIR(4));
        if (has_colors()) wattron(main_win, COLOR_PAIR(3) | A_BOLD);  // Yellow for paused
        mvwprintw(main_win, 1, 2, " GNU Wget - TUI Downloader [PAUSED] (Active: %d) ", active_count);
        if (has_colors()) wattroff(main_win, COLOR_PAIR(3));
        if (has_colors()) wattron(main_win, COLOR_PAIR(4) | A_BOLD);
    } else {
        mvwprintw(main_win, 1, 2, " GNU Wget - TUI Downloader (Active: %d) ", active_count);
    }
    if (has_colors()) wattroff(main_win, COLOR_PAIR(4) | A_BOLD);

    // Show scroll indicator in header if scrolling is possible
    if (bar_count > visible_bars) {
        int first_shown = scroll_offset + 1;
        int last_shown = scroll_offset + visible_bars;
        if (last_shown > bar_count) last_shown = bar_count;
        
        if (has_colors()) wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, 1, width - 25, "[%d-%d of %d]", first_shown, last_shown, bar_count);
        if (has_colors()) wattroff(main_win, COLOR_PAIR(1));
    }
    
    // Show key hints at bottom
    if (has_colors()) wattron(main_win, COLOR_PAIR(5));
    mvwhline(main_win, height - 2, 1, ' ', width - 2);
    if (tui_paused) {
        mvwprintw(main_win, height - 2, 2, "[P] Resume  [C/ESC] Cancel");
    } else {
        mvwprintw(main_win, height - 2, 2, "[P] Pause   [C/ESC] Cancel");
    }
    // Show scroll hints if scrolling is possible
    if (bar_count > visible_bars) {
        mvwprintw(main_win, height - 2, 32, "[J/Down] Scroll Down  [K/Up] Scroll Up");
    }
    if (has_colors()) wattroff(main_win, COLOR_PAIR(5));

    // Clear all bar areas first
    for (int row = 3; row < height - 2; row++) {
        mvwhline(main_win, row, 1, ' ', width - 2);
    }

    // Draw bars based on scroll offset
    int display_index = 0;  // Position on screen (0, 1, 2, ...)
    for (int i = scroll_offset; i < bar_count && display_index < visible_bars; i++) {
        TuiProgress *bar = bars[i];
        if (bar == NULL) continue;

        int row = 3 + (display_index * 4);
        if (row + 3 >= height - 2) break;  // Don't overwrite footer

        mvwprintw(main_win, row, 2, "File: %s (ID: %d)", bar->filename, bar->id);

        if (!bar->active) {
            // Draw finished state
            if (has_colors()) wattron(main_win, COLOR_PAIR(2) | A_BOLD);
            mvwprintw(main_win, row + 1, 2, "[ DONE ]");
            mvwprintw(main_win, row + 2, 2, "Download Complete");
            if (has_colors()) wattroff(main_win, COLOR_PAIR(2) | A_BOLD);
            display_index++;
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
        
        display_index++;
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

// Finish with checksum calculation
void tui_progress_finish_with_checksum(void *bar_ptr, double time_taken) {
    TuiProgress *bar = (TuiProgress*)bar_ptr;
    if (!bar) return;
    
    pthread_mutex_lock(&tui_mutex);
    bar->active = false;
    bar->current = bar->total;
    pthread_mutex_unlock(&tui_mutex);
    
    // Force redraw to show "Done" immediately
    tui_progress_draw(bar);
    
    // Calculate checksum if requested and filepath is available
    if (bar->checksum_type != CHECKSUM_NONE && bar->filepath) {
        tui_debug("tui_progress_finish_with_checksum: calculating checksum for %s", bar->filepath);
        
        bool success = calculate_checksum(bar->filepath, bar->checksum_type, bar->checksum);
        
        pthread_mutex_lock(&tui_mutex);
        bar->checksum_calculated = success;
        
        if (success && bar->expected_checksum[0] != '\0') {
            bar->checksum_verified = verify_checksum(bar->checksum, bar->expected_checksum);
            tui_debug("tui_progress_finish_with_checksum: verification result=%d", bar->checksum_verified);
        }
        pthread_mutex_unlock(&tui_mutex);
        
        // Redraw to show checksum result
        tui_progress_draw(bar);
    }
}

// Get checksum string
const char *tui_get_checksum(void *bar_ptr) {
    TuiProgress *bar = (TuiProgress *)bar_ptr;
    if (!bar || !bar->checksum_calculated) {
        return NULL;
    }
    return bar->checksum;
}

// Check if checksum was verified successfully
bool tui_is_checksum_verified(void *bar_ptr) {
    TuiProgress *bar = (TuiProgress *)bar_ptr;
    if (!bar) return false;
    return bar->checksum_verified;
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
    // Stop input handler first (before acquiring mutex to avoid deadlock)
    tui_stop_input_handler();
    
    pthread_mutex_lock(&tui_mutex);
    
    // Free all progress bars
    for (int i = 0; i < bar_count; i++) {
        if (bars[i]) {
            if (bars[i]->filename) {
                free(bars[i]->filename);
            }
            if (bars[i]->filepath) {
                free(bars[i]->filepath);
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
    
    // Reset scroll state
    scroll_offset = 0;
    visible_bars = 0;
    
    // Free completed files list
    for (int i = 0; i < completed_file_count; i++) {
        if (completed_files[i].filename) {
            free(completed_files[i].filename);
        }
        if (completed_files[i].filepath) {
            free(completed_files[i].filepath);
        }
    }
    if (completed_files) {
        free(completed_files);
        completed_files = NULL;
    }
    completed_file_count = 0;
    
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
    pthread_mutex_unlock(&tui_mutex);
    
    // Check if we have any completed files to verify
    pthread_mutex_lock(&tui_mutex);
    int file_count = completed_file_count;
    pthread_mutex_unlock(&tui_mutex);
    
    if (file_count == 0) {
        // No completed files, just show completion and exit
        pthread_mutex_lock(&tui_mutex);
        if (has_colors()) wattron(main_win, COLOR_PAIR(2) | A_BOLD);
        mvwprintw(main_win, height - 3, 2, "All downloads completed! Press any key to exit...");
        if (has_colors()) wattroff(main_win, COLOR_PAIR(2) | A_BOLD);
        wrefresh(main_win);
        pthread_mutex_unlock(&tui_mutex);
        
        wtimeout(main_win, 5000);
        wgetch(main_win);
        tui_cleanup();
        return;
    }
    
    // Ask user if they want to verify checksums
    pthread_mutex_lock(&tui_mutex);
    if (has_colors()) wattron(main_win, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(main_win, height - 4, 2, "All downloads completed! (%d files merged)", file_count);
    if (has_colors()) wattroff(main_win, COLOR_PAIR(2) | A_BOLD);
    
    mvwprintw(main_win, height - 3, 2, "Do you want to verify checksums? [y/n]: ");
    wclrtoeol(main_win);
    wrefresh(main_win);
    pthread_mutex_unlock(&tui_mutex);
    
    // Wait for y/n response
    wtimeout(main_win, -1); // No timeout for this question
    keypad(main_win, TRUE);
    int ch = wgetch(main_win);
    
    if (ch == 'y' || ch == 'Y') {
        // Create a new window for checksum verification
        pthread_mutex_lock(&tui_mutex);
        WINDOW *checksum_win = newwin(height - 2, width - 4, 1, 2);
        box(checksum_win, 0, 0);
        keypad(checksum_win, TRUE);
        
        if (has_colors()) wattron(checksum_win, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(checksum_win, 1, 2, " Checksum Verification ");
        if (has_colors()) wattroff(checksum_win, COLOR_PAIR(4) | A_BOLD);
        
        // Show list of files to verify
        mvwprintw(checksum_win, 3, 2, "Files to verify:");
        for (int i = 0; i < completed_file_count && i < 8; i++) {
            mvwprintw(checksum_win, 4 + i, 4, "%d. %s", i + 1, completed_files[i].filename);
        }
        if (completed_file_count > 8) {
            mvwprintw(checksum_win, 12, 4, "... and %d more files", completed_file_count - 8);
        }
        
        // Ask for checksum type
        mvwprintw(checksum_win, 14, 2, "Select checksum type:");
        mvwprintw(checksum_win, 15, 4, "[1] MD5");
        mvwprintw(checksum_win, 16, 4, "[2] SHA256");
        mvwprintw(checksum_win, 17, 4, "[q] Skip verification");
        wrefresh(checksum_win);
        pthread_mutex_unlock(&tui_mutex);
        
        ch = wgetch(checksum_win);
        ChecksumType selected_type = CHECKSUM_NONE;
        const char *type_name = "";
        
        if (ch == '1') {
            selected_type = CHECKSUM_MD5;
            type_name = "MD5";
        } else if (ch == '2') {
            selected_type = CHECKSUM_SHA256;
            type_name = "SHA256";
        }
        
        if (selected_type != CHECKSUM_NONE) {
            char input_buf[128] = {0};
            
            // Process each completed/merged file
            pthread_mutex_lock(&tui_mutex);
            for (int i = 0; i < completed_file_count; i++) {
                CompletedFile *cf = &completed_files[i];
                if (!cf->filepath) continue;
                
                // Clear previous content
                werase(checksum_win);
                box(checksum_win, 0, 0);
                
                if (has_colors()) wattron(checksum_win, COLOR_PAIR(4) | A_BOLD);
                mvwprintw(checksum_win, 1, 2, " Checksum Verification - %s ", type_name);
                if (has_colors()) wattroff(checksum_win, COLOR_PAIR(4) | A_BOLD);
                
                // Show file info
                mvwprintw(checksum_win, 3, 2, "File %d of %d:", i + 1, completed_file_count);
                if (has_colors()) wattron(checksum_win, COLOR_PAIR(1));
                mvwprintw(checksum_win, 4, 4, "Name: %s", cf->filename);
                mvwprintw(checksum_win, 5, 4, "Path: %.60s", cf->filepath);
                if (has_colors()) wattroff(checksum_win, COLOR_PAIR(1));
                
                // Calculate checksum
                mvwprintw(checksum_win, 7, 2, "Calculating %s checksum (please wait)...", type_name);
                wrefresh(checksum_win);
                pthread_mutex_unlock(&tui_mutex);
                
                bool calc_success = calculate_checksum(cf->filepath, selected_type, cf->checksum);
                
                pthread_mutex_lock(&tui_mutex);
                cf->checksum_calculated = calc_success;
                
                if (calc_success) {
                    // Show calculated checksum
                    mvwprintw(checksum_win, 7, 2, "Calculated %s:                              ", type_name);
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(2));
                    mvwprintw(checksum_win, 8, 4, "%s", cf->checksum);
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(2));
                    
                    // Ask for expected checksum
                    mvwprintw(checksum_win, 10, 2, "Enter expected %s for '%s':", type_name, cf->filename);
                    mvwprintw(checksum_win, 11, 2, "(Press Enter to skip, or paste checksum): ");
                    wrefresh(checksum_win);
                    
                    // Enable echo for input
                    echo();
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(3));
                    wmove(checksum_win, 12, 4);
                    wgetnstr(checksum_win, input_buf, 127);
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(3));
                    noecho();
                    
                    // Verify if user entered a checksum
                    if (strlen(input_buf) > 0) {
                        strncpy(cf->expected_checksum, input_buf, 64);
                        cf->expected_checksum[64] = '\0';
                        cf->checksum_verified = verify_checksum(cf->checksum, cf->expected_checksum);
                        
                        // Show verification result
                        if (cf->checksum_verified) {
                            if (has_colors()) wattron(checksum_win, COLOR_PAIR(2) | A_BOLD);
                            mvwprintw(checksum_win, 14, 4, "CHECKSUM VERIFIED - OK!");
                            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(2) | A_BOLD);
                        } else {
                            if (has_colors()) wattron(checksum_win, COLOR_PAIR(5) | A_BOLD);
                            mvwprintw(checksum_win, 14, 4, "CHECKSUM MISMATCH - FAILED!");
                            mvwprintw(checksum_win, 15, 4, "Expected: %.64s", cf->expected_checksum);
                            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(5) | A_BOLD);
                        }
                    } else {
                        if (has_colors()) wattron(checksum_win, COLOR_PAIR(3));
                        mvwprintw(checksum_win, 14, 4, "Skipped verification for this file.");
                        if (has_colors()) wattroff(checksum_win, COLOR_PAIR(3));
                    }
                } else {
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(5));
                    mvwprintw(checksum_win, 7, 2, "Failed to calculate checksum!                    ");
                    mvwprintw(checksum_win, 8, 4, "File may not exist or cannot be read.");
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(5));
                }
                
                // Wait for user to continue
                mvwprintw(checksum_win, height - 5, 2, "Press any key to continue...");
                wrefresh(checksum_win);
                pthread_mutex_unlock(&tui_mutex);
                
                wgetch(checksum_win);
                pthread_mutex_lock(&tui_mutex);
                memset(input_buf, 0, sizeof(input_buf));
            }
            
            // Show summary
            werase(checksum_win);
            box(checksum_win, 0, 0);
            
            if (has_colors()) wattron(checksum_win, COLOR_PAIR(4) | A_BOLD);
            mvwprintw(checksum_win, 1, 2, " Checksum Verification Summary ");
            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(4) | A_BOLD);
            
            int verified_count = 0;
            int failed_count = 0;
            int skipped_count = 0;
            
            int current_row = 3;
            for (int i = 0; i < completed_file_count; i++) {
                CompletedFile *cf = &completed_files[i];
                
                mvwprintw(checksum_win, current_row, 2, "%d. %.50s: ", i + 1, cf->filename);
                
                if (!cf->checksum_calculated) {
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(5));
                    wprintw(checksum_win, "CALC FAILED");
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(5));
                    failed_count++;
                } else if (cf->expected_checksum[0] == '\0') {
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(3));
                    wprintw(checksum_win, "SKIPPED");
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(3));
                    skipped_count++;
                } else if (cf->checksum_verified) {
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(2) | A_BOLD);
                    wprintw(checksum_win, "VERIFIED");
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(2) | A_BOLD);
                    verified_count++;
                } else {
                    if (has_colors()) wattron(checksum_win, COLOR_PAIR(5) | A_BOLD);
                    wprintw(checksum_win, "MISMATCH");
                    if (has_colors()) wattroff(checksum_win, COLOR_PAIR(5) | A_BOLD);
                    failed_count++;
                }
                current_row++;
                if (current_row >= height - 8) break; // Prevent overflow
            }
            
            current_row += 2;
            mvwprintw(checksum_win, current_row, 2, "Summary: ");
            if (has_colors()) wattron(checksum_win, COLOR_PAIR(2));
            wprintw(checksum_win, "%d verified", verified_count);
            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(2));
            wprintw(checksum_win, ", ");
            if (has_colors()) wattron(checksum_win, COLOR_PAIR(5));
            wprintw(checksum_win, "%d failed", failed_count);
            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(5));
            wprintw(checksum_win, ", ");
            if (has_colors()) wattron(checksum_win, COLOR_PAIR(3));
            wprintw(checksum_win, "%d skipped", skipped_count);
            if (has_colors()) wattroff(checksum_win, COLOR_PAIR(3));
            
            mvwprintw(checksum_win, height - 5, 2, "Press any key to exit...");
            wrefresh(checksum_win);
            pthread_mutex_unlock(&tui_mutex);
            
            wgetch(checksum_win);
            
            pthread_mutex_lock(&tui_mutex);
            delwin(checksum_win);
            pthread_mutex_unlock(&tui_mutex);
        } else {
            pthread_mutex_lock(&tui_mutex);
            delwin(checksum_win);
            pthread_mutex_unlock(&tui_mutex);
        }
    }
    
    // Cleanup TUI
    tui_cleanup();
}