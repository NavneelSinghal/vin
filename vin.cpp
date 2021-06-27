/** includes */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** defines */

#define TAB_STOP 4
#define QUIT_TIMES 3

#define CTRL_KEY(k) ((k)&0b00011111)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

/** data */

struct editorConfig {
    int cursor_x = 0;     // location in the file
    int cursor_y = 0;     // location in the file
    int rendered_x = 0;   // taking into account rendering of characters, hence
                          // location in the rendered row
    int screen_rows = 0;  // total number of rows on the screen
    int screen_cols = 0;  // total number of cols on the screen
    int row_offset = 0;   // number of rows to be offset - truncation, hence
                          // works for both file and screen
    int col_offset = 0;   // number of cols to be offset - works with rendered_x
    bool dirty = false;   // whether we have made changes or not
    std::vector<std::string> rows;           // rows of the file
    std::vector<std::string> rendered_rows;  // rows of the rendered rows
    std::string filename = "";
    std::string command_bar = "";  // for command mode and alert messages
    struct termios
        original_termios;  // terminal information to be restored in the end
};

struct editorConfig E;

/** prototypes */

void editorSetStatusMessage(const char* fmt, ...);

/** terminal */

void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = E.original_termios;
    raw.c_iflag &= tcflag_t(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= tcflag_t(~(OPOST));
    raw.c_cflag |= tcflag_t((CS8));
    raw.c_lflag &= tcflag_t(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    ssize_t nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == -1 && errno != EAGAIN) die("read");
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getWindowSize(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    else {
        cols = ws.ws_col;
        rows = ws.ws_row;
        return 0;
    }
}

/** row operations */

void editorUpdateRow(const std::string& s, std::string& rendered_s) {
    rendered_s = "";
    for (auto c : s)
        if (c == '\t') {
            rendered_s += ' ';
            while (rendered_s.size() % TAB_STOP != 0) rendered_s += ' ';
        } else
            rendered_s += c;
}

void editorInsertRow(int at, const std::string& s) {
    if (at < 0 || at > (int)E.rows.size()) return;
    E.rows.insert(E.rows.begin() + at, s);
    E.rendered_rows.insert(E.rendered_rows.begin() + at, "");
    editorUpdateRow(*(E.rows.begin() + at), *(E.rendered_rows.begin() + at));
    E.dirty = true;
}

void editorRowInsertChar(std::string& s, std::string& rendered_s, int at,
                         int c) {
    if (at < 0 || at > (int)s.size()) at = (int)s.size();
    s.insert(s.begin() + at, (char)c);
    editorUpdateRow(s, rendered_s);
    E.dirty = true;
}

void editorRowDelChar(std::string& s, std::string& rendered_s, int at) {
    if (at <= 0 || at > (int)s.size()) return;
    s.erase(s.begin() + at - 1);
    editorUpdateRow(s, rendered_s);
    E.dirty = true;
}

void editorRowAppendString(std::string& s, std::string& rendered_s,
                           const std::string& to_append) {
    s += to_append;
    editorUpdateRow(s, rendered_s);
    E.dirty = true;
}

int editorComputeRenderedX(const std::string& s, int cursor_x) {
    int rendered_x = 0;
    for (auto c : std::string_view(s.data(), size_t(cursor_x)))
        if (c == '\t')
            rendered_x += TAB_STOP - rendered_x % TAB_STOP;
        else
            rendered_x++;
    return rendered_x;
}

/** editor operations */

void editorInsertChar(int c) {
    if (E.cursor_y == (int)E.rendered_rows.size())
        editorInsertRow((int)E.rows.size(), "");
    editorRowInsertChar(E.rows[size_t(E.cursor_y)],
                        E.rendered_rows[size_t(E.cursor_y)], E.cursor_x, c);
    E.cursor_x++;
}

void editorInsertNewline() {
    if (E.cursor_x == 0) {
        editorInsertRow(E.cursor_y, "");
    } else {
        editorInsertRow(E.cursor_y + 1,
                        E.rows[size_t(E.cursor_y)].substr(size_t(E.cursor_x)));
        E.rows[size_t(E.cursor_y)] =
            E.rows[size_t(E.cursor_y)].substr(size_t(0), size_t(E.cursor_x));
        editorUpdateRow(E.rows[size_t(E.cursor_y)],
                        E.rendered_rows[size_t(E.cursor_y)]);
    }
    E.cursor_x = 0;
    E.cursor_y++;
}

void editorDelRow(int at) {
    if (at < 0 || at >= (int)E.rows.size()) return;
    E.rows.erase(E.rows.begin() + at);
    E.rendered_rows.erase(E.rendered_rows.begin() + at);
    E.dirty = true;
}

void editorDelChar() {
    if (E.cursor_y == (int)E.rendered_rows.size()) return;
    if (E.cursor_x == 0 && E.cursor_y == 0) return;
    if (E.cursor_x == 0) {
        E.cursor_x = (int)E.rows[size_t(E.cursor_y - 1)].size();
        editorRowAppendString(E.rows[size_t(E.cursor_y - 1)],
                              E.rendered_rows[size_t(E.cursor_y - 1)],
                              E.rows[size_t(E.cursor_y)]);
        editorDelRow(E.cursor_y);
        E.cursor_y--;
    } else {
        editorRowDelChar(E.rows[size_t(E.cursor_y)],
                         E.rendered_rows[size_t(E.cursor_y)], E.cursor_x);
        E.cursor_x--;
    }
}

/** file i/o */

void editorOpen(char* filename) {
    E.filename = filename;
    FILE* fp = fopen(filename, "r");
    if (!fp) die("fopen");
    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow((int)E.rows.size(),
                        std::string(line).substr(0, size_t(linelen)));
    }
    free(line);
    fclose(fp);
    E.dirty = false;
}

std::string editorRowsToString() {
    std::string representation = "";
    for (const auto& s : E.rows) representation += s + '\n';
    return representation;
}

void editorSave() {
    if (E.filename == "") return;
    std::string representation = editorRowsToString();
    int len = (int)representation.size();
    const char* buf = representation.c_str();
    int fd = open(E.filename.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, size_t(len)) == len) {
                close(fd);
                editorSetStatusMessage("%d bytes written to disk", len);
                E.dirty = false;
                return;
            }
        }
        close(fd);
    }
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/** input */

void editorMoveCursor(int c) {
    bool not_on_last = size_t(E.cursor_y) < E.rows.size();
    switch (c) {
        case ARROW_LEFT:
            if (E.cursor_x > 0)
                E.cursor_x--;
            else if (E.cursor_y > 0) {  // to go to end of previous line
                E.cursor_y--;
                E.cursor_x = (int)E.rows[size_t(E.cursor_y)].size();
            }
            break;
        case ARROW_RIGHT:
            // to not allow overflowing past the end (one past the end allowed)
            if (not_on_last &&
                size_t(E.cursor_x) < E.rows[size_t(E.cursor_y)].size())
                E.cursor_x++;
            // to allow going to the next line with a right movement
            else if (not_on_last &&
                     size_t(E.cursor_x) == E.rows[size_t(E.cursor_y)].size()) {
                E.cursor_y++;
                E.cursor_x = 0;
            }
            break;
        case ARROW_DOWN:
            if (E.cursor_y < (int)E.rows.size()) E.cursor_y++;
            break;
        case ARROW_UP:
            if (E.cursor_y > 0) E.cursor_y--;
            break;
    }
    // snap to end - done in terms of cursor_x, not rendered_x
    int row_len = (size_t(E.cursor_y) >= E.rows.size()
                       ? 0
                       : (int)E.rows[size_t(E.cursor_y)].size());
    if (E.cursor_x > row_len) E.cursor_x = row_len;
}

void editorProcessKeypress() {
    static int quit_times = QUIT_TIMES;
    int c = editorReadKey();
    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage(
                    "File has unsaved changes. "
                    "Repeat Ctrl-Q %d more times to quit.",
                    quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case HOME_KEY:
            E.cursor_x = 0;
            break;
        case END_KEY:
            if (E.cursor_y < (int)E.rendered_rows.size())
                E.cursor_x = (int)E.rows[size_t(E.cursor_y)].size();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                E.cursor_y = E.row_offset;
            } else if (c == PAGE_DOWN) {
                E.cursor_y = E.row_offset + E.screen_rows - 1;
                if (E.cursor_y > (int)E.rendered_rows.size())
                    E.cursor_y = (int)E.rendered_rows.size();
            }
            int times = E.screen_rows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        } break;
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_UP:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editorInsertChar(c);
            break;
    }
    quit_times = QUIT_TIMES;
}

/** output */

void editorScroll() {
    E.rendered_x = 0;
    if (E.cursor_y < (int)E.rendered_rows.size())
        E.rendered_x =
            editorComputeRenderedX(E.rows[size_t(E.cursor_y)], E.cursor_x);
    if (E.cursor_y < E.row_offset) E.row_offset = E.cursor_y;
    if (E.cursor_y >= E.row_offset + E.screen_rows)
        E.row_offset = E.cursor_y - E.screen_rows + 1;
    if (E.rendered_x < E.col_offset) E.col_offset = E.rendered_x;
    if (E.rendered_x >= E.col_offset + E.screen_cols)
        E.col_offset = E.rendered_x - E.screen_cols + 1;
}

void editorDrawRows(std::string& s) {
    for (int y = 0; y < E.screen_rows; y++) {
        int row_number = E.row_offset + y;
        if (row_number >= (int)E.rendered_rows.size())
            s += '~';
        else
            s += E.rendered_rows[size_t(row_number)].substr(
                size_t(
                    std::min(E.col_offset,
                             (int)E.rendered_rows[size_t(row_number)].size())),
                size_t(std::min(
                    E.screen_cols,
                    std::max(0,
                             (int)E.rendered_rows[size_t(row_number)].size() -
                                 E.col_offset))));
        s += "\x1b[K";  // to clear a single line
        s += "\r\n";
    }
}

void editorDrawStatusBar(std::string& s) {
    s += "\x1b[7m";
    std::string display_name =
        (E.filename.size() == 0 ? "[No Name]" : E.filename);
    std::string display_status =
        display_name.substr(size_t(0),
                            std::min(display_name.size(), size_t(20))) +
        " - " + std::to_string(E.rows.size()) + " lines" + " " +
        (E.dirty ? "(modified)" : "");
    display_status = display_status.substr(
        size_t(0), std::min(display_status.size(), size_t(E.screen_cols)));
    auto line_number = std::to_string(E.cursor_y);
    s += display_status;
    s += std::string(size_t(E.screen_cols) - display_status.size(), ' ');
    for (int i = 0; i < (int)line_number.size(); ++i) s.pop_back();
    s += line_number;
    s += "\x1b[m";
    s += "\r\n";
}

void editorDrawCommandBar(std::string& s) {
    s += "\x1b[K";
    s += E.command_bar;
}

void editorRefreshScreen() {
    editorScroll();
    std::string s = "";
    s += "\x1b[?25l";  // to hide the cursor
    // s += "\x1b[2J";    // to clear the screen
    s += "\x1b[H";  // to go to the top left
    editorDrawRows(s);
    editorDrawStatusBar(s);
    editorDrawCommandBar(s);
    s += "\x1b[" + std::to_string(E.cursor_y - E.row_offset + 1) + ";" +
         std::to_string(E.rendered_x - E.col_offset + 1) + "H";
    // to go to a specified position
    s += "\x1b[?25h";  // to show the cursor again
    write(STDOUT_FILENO, s.c_str(), s.size());
}

void editorSetStatusMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[80];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    E.command_bar = buf;
}

/** init */

void initEditor() {
    if (getWindowSize(E.screen_rows, E.screen_cols) == -1) die("getWindowSize");
    E.screen_rows -= 2;
}

int main(int argc, char** argv) {
    enableRawMode();
    initEditor();
    if (argc >= 2) editorOpen(argv[1]);
    editorSetStatusMessage("Use Ctrl-Q to quit, Ctrl-S to save");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
