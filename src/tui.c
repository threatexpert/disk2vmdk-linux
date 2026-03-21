/*
 * tui.c — Terminal UI for interactive disk imaging
 *
 * Pure ANSI escape codes + termios raw mode. No ncurses dependency.
 *
 * Layout:
 *   Disk info header
 *   Partition table with checkboxes and copy mode toggle
 *   Output file input
 *   Format selection
 *   Action bar
 */
#include "common.h"
#include <termios.h>
#include <signal.h>

/* ========================================================================= */
/*  Terminal helpers                                                           */
/* ========================================================================= */

static struct termios g_orig_termios;
static bool g_raw_mode = false;

static void tui_restore_term(void)
{
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
    /* Show cursor */
    fprintf(stderr, "\033[?25h");
    fflush(stderr);
}

static int tui_enter_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) return -1;
    g_raw_mode = true;
    atexit(tui_restore_term);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    /* Hide cursor */
    fprintf(stderr, "\033[?25l");
    return 0;
}

/* Read a single keypress, handling escape sequences for arrow keys */
#define KEY_UP     1000
#define KEY_DOWN   1001
#define KEY_LEFT   1002
#define KEY_RIGHT  1003
#define KEY_TAB    9
#define KEY_ENTER  10
#define KEY_SPACE  32
#define KEY_ESC    27
#define KEY_Q      113
#define KEY_BACKSPACE 127

static int tui_read_key(void)
{
    uint8_t c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;

    if (c == 27) {
        uint8_t seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;
        if (seq[0] == '[') {
            switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    return (int)c;
}

/* ANSI helpers */
#define CSI         "\033["
#define CLEAR_SCREEN CSI "2J" CSI "H"
#define BOLD        CSI "1m"
#define DIM         CSI "2m"
#define RESET       CSI "0m"
#define FG_GREEN    CSI "32m"
#define FG_RED      CSI "31m"
#define FG_YELLOW   CSI "33m"
#define FG_CYAN     CSI "36m"
#define FG_WHITE    CSI "37m"
#define BG_BLUE     CSI "44m"
#define REVERSE     CSI "7m"

static void move_to(int row, int col) { fprintf(stderr, CSI "%d;%dH", row, col); }
static void clear_line(void) { fprintf(stderr, CSI "2K"); }

/* ========================================================================= */
/*  TUI state                                                                 */
/* ========================================================================= */

#define COL_SELECT   0   /* column: partition checkbox */
#define COL_MODE     1   /* column: copy mode */
#define COL_OUTPUT   2   /* column: output file */
#define COL_FORMAT   3   /* column: format */
#define COL_ACTION   4   /* column: start / quit buttons */
#define NUM_COLS     5

typedef struct {
    disk_info_t  *disk;
    int           cursor_row;     /* current partition index (for COL_SELECT/COL_MODE) */
    int           cursor_col;     /* which column is active */
    int           action_idx;     /* 0=Start, 1=Quit (for COL_ACTION) */
    char          output_path[MAX_PATH_LEN];
    int           output_len;
    vdisk_format_t format;
    bool          done;
    bool          confirmed;
} tui_state_t;

/* ========================================================================= */
/*  Draw the interface                                                        */
/* ========================================================================= */

static void tui_draw(const tui_state_t *st)
{
    disk_info_t *disk = st->disk;
    char sz[32];
    int row = 1;
    int i;

    fprintf(stderr, CLEAR_SCREEN);

    /* Header */
    move_to(row++, 1);
    fprintf(stderr, BOLD FG_CYAN "disk2vmdk v" D2V_VERSION " — Interactive Mode" RESET);
    row++;

    move_to(row++, 1);
    format_size(disk->size, sz, sizeof(sz));
    const char *pt = disk->pt_type == PT_GPT ? "GPT" : (disk->pt_type == PT_MBR ? "MBR" : "?");
    fprintf(stderr, "Disk: " BOLD "%s" RESET "  %s  %s  %s",
            disk->dev_path, disk->model[0] ? disk->model : "", sz, pt);
    row++;

    /* Table header */
    move_to(row++, 1);
    fprintf(stderr, DIM "  %-3s %-20s %-8s %10s  %-16s %-12s" RESET,
            "Sel", "Device", "Type", "Size", "Label", "Copy Mode");
    move_to(row++, 1);
    fprintf(stderr, DIM "  ──────────────────────────────────────────────────────────────────────" RESET);

    /* Partition rows */
    for (i = 0; i < disk->num_partitions; i++) {
        partition_info_t *p = &disk->partitions[i];
        format_size(p->size, sz, sizeof(sz));

        move_to(row, 1);
        clear_line();

        /* Highlight current row */
        bool is_current = (i == st->cursor_row &&
                          (st->cursor_col == COL_SELECT || st->cursor_col == COL_MODE));

        /* Checkbox */
        const char *chk;
        if (st->cursor_col == COL_SELECT && i == st->cursor_row)
            chk = p->selected ? (REVERSE FG_GREEN " ✓ " RESET) : (REVERSE FG_RED " ✗ " RESET);
        else
            chk = p->selected ? (FG_GREEN " ✓ " RESET) : (FG_RED " ✗ " RESET);

        /* Copy mode */
        char mode_str[64];
        if (p->selected) {
            const char *mode_text;
            if (p->copy_mode == 1 &&
                (p->fs_type == FS_EXT4 || p->fs_type == FS_EXT3 ||
                 p->fs_type == FS_EXT2 || p->fs_type == FS_XFS ||
                 p->fs_type == FS_LVM))
                mode_text = "Used-only";
            else
                mode_text = "Full";

            if (st->cursor_col == COL_MODE && i == st->cursor_row)
                snprintf(mode_str, sizeof(mode_str), REVERSE " %s " RESET, mode_text);
            else
                snprintf(mode_str, sizeof(mode_str), " %s ", mode_text);
        } else {
            snprintf(mode_str, sizeof(mode_str), DIM " --skip-- " RESET);
        }

        fprintf(stderr, "%s %-20s %-8s %10s  %-16s%s",
                chk, p->dev_path, fs_type_name(p->fs_type),
                sz, p->fs_label, mode_str);

        if (is_current) fprintf(stderr, "  ◄");
        row++;
    }

    row++;
    move_to(row++, 1);
    fprintf(stderr, DIM "  ──────────────────────────────────────────────────────────────────────" RESET);

    /* Output file */
    row++;
    move_to(row++, 1);
    if (st->cursor_col == COL_OUTPUT)
        fprintf(stderr, REVERSE "  Output file: " RESET " %s" BOLD "▏" RESET, st->output_path);
    else
        fprintf(stderr, "  Output file:  " BOLD "%s" RESET, st->output_path[0] ? st->output_path : "(press Tab to edit)");

    /* Format */
    move_to(row++, 1);
    const char *fmts[] = { "VMDK", "VHD", "VDI", "DD" };
    fprintf(stderr, "  Format:       ");
    for (i = 0; i < 4; i++) {
        bool sel = ((int)st->format == i + 1); /* VMDK=1, VHD=2, VDI=3, DD would need mapping */
        /* Map: VDISK_FMT_DD=0, VMDK=1, VHD=2, VDI=3 */
        vdisk_format_t fmt_val;
        switch (i) {
        case 0: fmt_val = VDISK_FMT_VMDK; break;
        case 1: fmt_val = VDISK_FMT_VHD; break;
        case 2: fmt_val = VDISK_FMT_VDI; break;
        default: fmt_val = VDISK_FMT_DD; break;
        }
        sel = (st->format == fmt_val);

        if (st->cursor_col == COL_FORMAT && sel)
            fprintf(stderr, REVERSE " %s " RESET "  ", fmts[i]);
        else if (sel)
            fprintf(stderr, BOLD FG_GREEN "[%s]" RESET "  ", fmts[i]);
        else
            fprintf(stderr, DIM " %s " RESET "  ", fmts[i]);
    }

    /* Action buttons */
    row += 2;
    move_to(row++, 1);
    if (st->cursor_col == COL_ACTION) {
        if (st->action_idx == 0)
            fprintf(stderr, "  " REVERSE BG_BLUE BOLD "  ▶ START  " RESET "    " DIM "  QUIT  " RESET);
        else
            fprintf(stderr, "  " DIM "  ▶ START  " RESET "    " REVERSE "  QUIT  " RESET);
    } else {
        fprintf(stderr, "  " BOLD "  ▶ START  " RESET "    " DIM "  QUIT  " RESET);
    }

    /* Help bar */
    row += 2;
    move_to(row, 1);
    fprintf(stderr, DIM "  ↑↓ Navigate   Space: toggle select/mode   Tab: next field   Enter: confirm   q: quit" RESET);

    fflush(stderr);
}

/* ========================================================================= */
/*  Auto-detect format from output filename extension                         */
/* ========================================================================= */

static vdisk_format_t detect_format(const char *path)
{
    const char *ext = strrchr(path, '.');
    return vdisk_format_from_ext(ext);
}

/* ========================================================================= */
/*  TUI main loop                                                             */
/* ========================================================================= */

int tui_run(int disk_fd, disk_info_t *disk, const char *output_preset,
            imaging_config_t *out_cfg)
{
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Error: interactive mode requires a terminal\n");
        return -1;
    }

    if (tui_enter_raw() < 0) {
        fprintf(stderr, "Error: failed to set terminal raw mode\n");
        return -1;
    }

    tui_state_t st;
    memset(&st, 0, sizeof(st));
    st.disk = disk;
    st.cursor_row = 0;
    st.cursor_col = COL_SELECT;
    st.format = VDISK_FMT_VMDK;
    st.action_idx = 0;

    /* Pre-fill output path from -o if provided */
    if (output_preset && output_preset[0]) {
        strncpy(st.output_path, output_preset, sizeof(st.output_path) - 1);
        st.output_len = (int)strlen(st.output_path);
        st.format = detect_format(st.output_path);
    }

    /* Default: all partitions selected, full copy */
    int i;
    for (i = 0; i < disk->num_partitions; i++) {
        disk->partitions[i].selected = true;
        disk->partitions[i].copy_mode = 0;
    }

    tui_draw(&st);

    while (!st.done) {
        int key = tui_read_key();
        if (key < 0) break;

        /* Global: q/Esc always quits */
        if ((key == KEY_Q || key == KEY_ESC) && st.cursor_col != COL_OUTPUT) {
            st.done = true;
            st.confirmed = false;
            break;
        }

        switch (st.cursor_col) {
        case COL_SELECT:
            if (key == KEY_UP && st.cursor_row > 0) st.cursor_row--;
            else if (key == KEY_DOWN && st.cursor_row < disk->num_partitions - 1) st.cursor_row++;
            else if (key == KEY_DOWN && st.cursor_row == disk->num_partitions - 1)
                st.cursor_col = COL_OUTPUT;  /* ↓ past last partition → output field */
            else if (key == KEY_SPACE)
                disk->partitions[st.cursor_row].selected = !disk->partitions[st.cursor_row].selected;
            else if (key == KEY_TAB || key == KEY_RIGHT) st.cursor_col = COL_MODE;
            else if (key == KEY_ENTER) { st.cursor_col = COL_ACTION; st.action_idx = 0; }
            break;

        case COL_MODE:
            if (key == KEY_UP && st.cursor_row > 0) st.cursor_row--;
            else if (key == KEY_DOWN && st.cursor_row < disk->num_partitions - 1) st.cursor_row++;
            else if (key == KEY_DOWN && st.cursor_row == disk->num_partitions - 1)
                st.cursor_col = COL_OUTPUT;
            else if (key == KEY_SPACE) {
                partition_info_t *p = &disk->partitions[st.cursor_row];
                if (p->selected) p->copy_mode = (p->copy_mode == 0) ? 1 : 0;
            }
            else if (key == KEY_TAB) st.cursor_col = COL_OUTPUT;
            else if (key == KEY_LEFT) st.cursor_col = COL_SELECT;
            else if (key == KEY_ENTER) { st.cursor_col = COL_ACTION; st.action_idx = 0; }
            break;

        case COL_OUTPUT:
            if (key == KEY_TAB || key == KEY_DOWN)
                st.cursor_col = COL_FORMAT;
            else if (key == KEY_UP) {
                st.cursor_col = COL_SELECT;
                st.cursor_row = disk->num_partitions - 1;
            }
            else if (key == KEY_BACKSPACE) {
                if (st.output_len > 0) st.output_path[--st.output_len] = '\0';
                st.format = detect_format(st.output_path);
            }
            else if (key == KEY_ENTER)
                st.cursor_col = COL_ACTION;
            else if (key == KEY_ESC)
                st.cursor_col = COL_SELECT;
            else if (key >= 32 && key < 127 && st.output_len < MAX_PATH_LEN - 1) {
                st.output_path[st.output_len++] = (char)key;
                st.output_path[st.output_len] = '\0';
                st.format = detect_format(st.output_path);
            }
            break;

        case COL_FORMAT:
            if (key == KEY_TAB || key == KEY_DOWN)
                st.cursor_col = COL_ACTION;
            else if (key == KEY_UP)
                st.cursor_col = COL_OUTPUT;
            else if (key == KEY_LEFT || key == KEY_SPACE) {
                if (st.format == VDISK_FMT_VHD) st.format = VDISK_FMT_VMDK;
                else if (st.format == VDISK_FMT_VDI) st.format = VDISK_FMT_VHD;
                else if (st.format == VDISK_FMT_DD) st.format = VDISK_FMT_VDI;
            }
            else if (key == KEY_RIGHT) {
                if (st.format == VDISK_FMT_VMDK) st.format = VDISK_FMT_VHD;
                else if (st.format == VDISK_FMT_VHD) st.format = VDISK_FMT_VDI;
                else if (st.format == VDISK_FMT_VDI) st.format = VDISK_FMT_DD;
            }
            else if (key == KEY_ENTER) { st.cursor_col = COL_ACTION; st.action_idx = 0; }
            break;

        case COL_ACTION:
            if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_TAB)
                st.action_idx = st.action_idx ? 0 : 1;
            else if (key == KEY_UP)
                st.cursor_col = COL_FORMAT;
            else if (key == KEY_ENTER || key == KEY_SPACE) {
                if (st.action_idx == 0) {
                    if (st.output_path[0] == '\0')
                        st.cursor_col = COL_OUTPUT;
                    else {
                        st.done = true;
                        st.confirmed = true;
                    }
                } else {
                    st.done = true;
                    st.confirmed = false;
                }
            }
            break;
        }

        tui_draw(&st);
    }

    tui_restore_term();
    fprintf(stderr, "\n");

    if (!st.confirmed) {
        fprintf(stderr, "Cancelled.\n");
        return 1;  /* not an error, just cancelled */
    }

    /* Validate */
    if (st.output_path[0] == '\0') {
        fprintf(stderr, "Error: no output file specified\n");
        return -1;
    }

    /* Populate output config */
    out_cfg->disk = disk;
    out_cfg->disk_fd = disk_fd;
    out_cfg->output_path = strdup(st.output_path);
    out_cfg->format = st.format;
    out_cfg->progress = progress_print;
    out_cfg->buf_size = 8 * 1024 * 1024;

    /* Print summary */
    fprintf(stderr, "\nStarting imaging:\n");
    char sz[32];
    for (i = 0; i < disk->num_partitions; i++) {
        partition_info_t *p = &disk->partitions[i];
        format_size(p->size, sz, sizeof(sz));
        if (!p->selected)
            fprintf(stderr, "  %-20s  %-8s  %10s  [EXCLUDED]\n",
                    p->dev_path, fs_type_name(p->fs_type), sz);
        else if (p->copy_mode == 1)
            fprintf(stderr, "  %-20s  %-8s  %10s  [USED-ONLY]\n",
                    p->dev_path, fs_type_name(p->fs_type), sz);
        else
            fprintf(stderr, "  %-20s  %-8s  %10s  [FULL]\n",
                    p->dev_path, fs_type_name(p->fs_type), sz);
    }
    fprintf(stderr, "  Output: %s  Format: %s\n\n",
            st.output_path, vdisk_format_name(st.format));

    return 0;
}
