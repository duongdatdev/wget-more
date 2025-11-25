#include "wget.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "tui.h"

TuiResult *tui_get_info(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    start_color();
    if (has_colors()) {
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_BLACK, COLOR_CYAN);
    }

    int height, width;
    getmaxyx(stdscr, height, width);
    WINDOW *win = newwin(20, 80, (height - 20) / 2, (width - 80) / 2);
    keypad(win, TRUE);

    char **urls = NULL;
    int count = 0;
    int selected = 0;
    char input_buf[1024] = {0};

    while (1) {
        werase(win);
        box(win, 0, 0);
        
        if (has_colors()) wattron(win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(win, 1, 2, " Wget Batch Downloader ");
        if (has_colors()) wattroff(win, COLOR_PAIR(1) | A_BOLD);

        mvwprintw(win, 3, 2, "URL List:");
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
            if (count > 0) break;
        }
    }

    endwin();
    
    TuiResult *res = malloc(sizeof(TuiResult));
    res->urls = urls;
    res->count = count;
    return res;
}

char *tui_get_url(void) {
    // Legacy wrapper not used anymore
    return NULL;
}

static WINDOW *prog_win = NULL;
static wgint prog_total = 0;
static wgint prog_current = 0;
static double prog_start_time = 0;
static int prog_paused = 0;

void *tui_progress_create (const char *f_name, wgint initial, wgint total) {
    if (!prog_win) {
        initscr();
        start_color();
        cbreak();
        noecho();
        curs_set(0);
        
        if (has_colors()) {
            init_pair(1, COLOR_CYAN, COLOR_BLACK);  // Info text
            init_pair(2, COLOR_GREEN, COLOR_BLACK); // Progress bar
            init_pair(3, COLOR_YELLOW, COLOR_BLACK);// Pause/Warning
            init_pair(4, COLOR_WHITE, COLOR_BLUE);  // Header
            init_pair(5, COLOR_RED, COLOR_BLACK);   // Brackets
        }

        int height, width;
        getmaxyx(stdscr, height, width);
        prog_win = newwin(14, 80, (height - 14) / 2, (width - 80) / 2);
        box(prog_win, 0, 0);
        
        // Header
        if (has_colors()) wattron(prog_win, COLOR_PAIR(4) | A_BOLD);
        mvwhline(prog_win, 1, 1, ' ', 78);
        mvwprintw(prog_win, 1, 2, " GNU Wget - TUI Downloader ");
        if (has_colors()) wattroff(prog_win, COLOR_PAIR(4) | A_BOLD);
    }
    prog_total = total;
    prog_current = initial;
    prog_start_time = (double)time(NULL);
    
    mvwprintw(prog_win, 3, 2, "File: ");
    if (has_colors()) wattron(prog_win, A_BOLD);
    wprintw(prog_win, "%s", f_name);
    if (has_colors()) wattroff(prog_win, A_BOLD);
    
    wrefresh(prog_win);
    return prog_win;
}

void tui_progress_draw (void *bar) {
    if (!prog_win) return;
    
    int width = 76;
    double pct = 0;
    if (prog_total > 0)
        pct = (double)prog_current / prog_total;
    
    // Draw bar
    if (has_colors()) wattron(prog_win, COLOR_PAIR(5));
    mvwprintw(prog_win, 5, 2, "[");
    mvwprintw(prog_win, 5, 2 + width - 1, "]");
    if (has_colors()) wattroff(prog_win, COLOR_PAIR(5));

    int filled = (int)(pct * (width - 2));
    
    if (has_colors()) wattron(prog_win, COLOR_PAIR(2));
    for (int i = 0; i < width - 2; i++) {
        if (i < filled) mvwaddch(prog_win, 5, 3 + i, ACS_CKBOARD); // Use block character
        else mvwaddch(prog_win, 5, 3 + i, ' ');
    }
    if (has_colors()) wattroff(prog_win, COLOR_PAIR(2));
    
    // Stats
    double now = (double)time(NULL);
    double elapsed = now - prog_start_time;
    double speed = 0;
    if (elapsed > 0) speed = prog_current / elapsed;
    
    if (has_colors()) wattron(prog_win, COLOR_PAIR(1));
    mvwprintw(prog_win, 7, 2, "Progress: %.1f%%", pct * 100);
    mvwprintw(prog_win, 7, 30, "Speed: %.2f KB/s", speed / 1024);
    
    if (speed > 0 && prog_total > 0) {
        double eta = (prog_total - prog_current) / speed;
        int h = (int)eta / 3600;
        int m = ((int)eta % 3600) / 60;
        int s = (int)eta % 60;
        mvwprintw(prog_win, 7, 60, "ETA: %02d:%02d:%02d", h, m, s);
    }
    if (has_colors()) wattroff(prog_win, COLOR_PAIR(1));
    
    // Footer controls
    mvwprintw(prog_win, 10, 2, "Controls:");
    if (has_colors()) wattron(prog_win, A_BOLD);
    wprintw(prog_win, " [p] ");
    if (has_colors()) wattroff(prog_win, A_BOLD);
    wprintw(prog_win, "Pause/Resume");
    
    if (has_colors()) wattron(prog_win, A_BOLD);
    wprintw(prog_win, "   [q] ");
    if (has_colors()) wattroff(prog_win, A_BOLD);
    wprintw(prog_win, "Quit");

    wrefresh(prog_win);
}

void tui_progress_update (void *bar, wgint howmuch, double time_taken) {
    prog_current += howmuch;
    // Handle input
    nodelay(stdscr, TRUE);
    int ch = getch();
    if (ch == 'q') {
        endwin();
        exit(0);
    } else if (ch == 'p') {
        prog_paused = !prog_paused;
        while (prog_paused) {
            if (has_colors()) wattron(prog_win, COLOR_PAIR(3) | A_BLINK);
            mvwprintw(prog_win, 12, 2, " *** PAUSED *** ");
            if (has_colors()) wattroff(prog_win, COLOR_PAIR(3) | A_BLINK);
            wrefresh(prog_win);
            usleep(100000);
            ch = getch();
            if (ch == 'p') {
                prog_paused = 0;
                mvwprintw(prog_win, 12, 2, "                ");
            } else if (ch == 'q') {
                endwin();
                exit(0);
            }
        }
    }
    
    tui_progress_draw(bar);
}

void tui_progress_finish (void *bar, double time_taken) {
    if (prog_win) {
        if (has_colors()) wattron(prog_win, COLOR_PAIR(2) | A_BOLD);
        mvwprintw(prog_win, 12, 2, " Download Complete! ");
        if (has_colors()) wattroff(prog_win, COLOR_PAIR(2) | A_BOLD);
        wrefresh(prog_win);
        usleep(1500000); // Show for 1.5s
        endwin();
        prog_win = NULL;
    }
}

void tui_progress_set_params (const char *params) {
    // Not used
}
