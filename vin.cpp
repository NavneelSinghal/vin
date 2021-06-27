/** includes */

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
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

enum editorHighlight {
    HIGHLIGHT_NORMAL = 0,
    HIGHLIGHT_KEYWORD1,
    HIGHLIGHT_KEYWORD2,
    HIGHLIGHT_COMMENT,
    HIGHLIGHT_STRING,
    HIGHLIGHT_NUMBER
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

enum editorMode { NORMAL, COMMAND, INSERT };

/** data */

struct editorSyntax {
    std::string filetype = "";
    std::vector<std::string> filematch{};
    std::vector<std::string> keywords{};
    std::string singleline_comment_start = "";
    int flags = 0;
};

struct editorRow {
    std::string raw_row;
    std::string rendered_row;
    std::string highlight_row;
    editorRow() : raw_row(), rendered_row(), highlight_row() {}
    editorRow(const std::string& _raw_row, const std::string& _rendered_row,
              const std::string& _highlight_row)
        : raw_row(_raw_row),
          rendered_row(_rendered_row),
          highlight_row(_highlight_row) {}
};

struct editorConfig {
    int mode = NORMAL;    // mode in which the editor operates
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
    std::vector<editorRow> rows;  // rows
    std::string filename = "";
    std::string command_bar = "";  // for command mode and alert messages
    std::string normal_buf = "";
    std::string command_buf = "";
    editorSyntax syntax;
    struct termios
        original_termios;  // terminal information to be restored in the end
};

struct editorConfig E;

/** filetypes */

std::vector<std::string> C_HL_extensions = {".c", ".h", ".cpp"};
std::vector<std::string> C_HL_keywords = {
    "switch", "if",    "while",     "for",     "break",   "continue",
    "return", "else",  "struct",    "union",   "typedef", "static",
    "enum",   "class", "case",      "int|",    "long|",   "double|",
    "float|", "char|", "unsigned|", "signed|", "void|"};

std::vector<editorSyntax> HLDB = {
    editorSyntax{"c", C_HL_extensions, C_HL_keywords, "//",
                 HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

/** prototypes */

void editorSetStatusMessage(const char* fmt, ...);

/** terminal */

void die(const char* s) {
    std::ignore = write(STDOUT_FILENO, "\x1b[2J", 4);
    std::ignore = write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    std::ignore = write(STDOUT_FILENO, "\x1b[?1049l", 8);
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
    std::ignore = write(STDOUT_FILENO, "\x1b[?1049h", 8);
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

/** syntax highlighting */

bool is_separator(char c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(editorRow& row) {
    row.highlight_row = std::string(row.rendered_row.size(), HIGHLIGHT_NORMAL);
    if (E.syntax.filetype == "") return;
    const auto& keywords = E.syntax.keywords;
    const auto& scs = E.syntax.singleline_comment_start;
    size_t i = 0;
    size_t len = row.rendered_row.size();
    bool prev_sep = true;
    char in_string = 0;
    char prev_hl = HIGHLIGHT_NORMAL;
    while (i < len) {
        char c = row.rendered_row[i];
        if (i > 0) prev_hl = row.highlight_row[i - 1];

        if (scs.size() != 0 && !in_string) {
            if (!strncmp(row.rendered_row.data() + i, scs.data(), scs.size())) {
                memset(row.highlight_row.data() + i, HIGHLIGHT_COMMENT,
                       len - i);
                break;
            }
        }

        if ((E.syntax.flags & HL_HIGHLIGHT_STRINGS) != 0) {
            if (in_string) {
                row.highlight_row[i] = HIGHLIGHT_STRING;
                if (c == '\\' && i + 1 < row.rendered_row.size()) {
                    row.highlight_row[i + 1] = HIGHLIGHT_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = false;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row.highlight_row[i] = HIGHLIGHT_STRING;
                    i++;
                    continue;
                }
            }
        }

        if ((E.syntax.flags & HL_HIGHLIGHT_NUMBERS) != 0) {
            if ((isdigit(c) && (prev_sep || prev_hl == HIGHLIGHT_NUMBER)) ||
                (c == '.' && prev_hl == HIGHLIGHT_NUMBER)) {
                row.highlight_row[i] = HIGHLIGHT_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            for (const auto& keyword : keywords) {
                size_t klen = keyword.size();
                bool kw2 = keyword[klen - 1] == '|';
                if (kw2) klen--;
                if (!strncmp(row.rendered_row.data() + i, keyword.data(),
                             klen) &&
                    is_separator(row.rendered_row[i + klen])) {
                    memset(row.highlight_row.data() + i,
                           kw2 ? HIGHLIGHT_KEYWORD2 : HIGHLIGHT_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            prev_sep = 0;
            continue;
        }

        prev_sep = is_separator(c);
        i++;
    }
}

int editorSyntaxToColor(int x) {
    switch (x) {
        case HIGHLIGHT_COMMENT:
            return 90;
        case HIGHLIGHT_KEYWORD1:
            return 94;  // 33;
        case HIGHLIGHT_KEYWORD2:
            return 91;  // 32;
        case HIGHLIGHT_NUMBER:
            return 36;  // 31;
        case HIGHLIGHT_STRING:
            return 36;  // 35;
        default:
            return 37;
    }
}

void editorSelectSyntaxHighlight() {
    if (E.filename == "") return;
    char* ext = strrchr(E.filename.data(), '.');
    for (unsigned int j = 0; j < HLDB.size(); j++) {
        editorSyntax* s = &HLDB[j];
        unsigned int i = 0;
        while (i < s->filematch.size()) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i].c_str())) ||
                (!is_ext &&
                 strstr(E.filename.c_str(), s->filematch[i].c_str()))) {
                E.syntax = *s;
                for (auto& row : E.rows) editorUpdateSyntax(row);
                return;
            }
            i++;
        }
    }
}

/** row operations */

void editorUpdateRow(editorRow& row) {
    row.rendered_row = "";
    for (auto c : row.raw_row)
        if (c == '\t') {
            row.rendered_row += ' ';
            while (row.rendered_row.size() % TAB_STOP != 0)
                row.rendered_row += ' ';
        } else
            row.rendered_row += c;
    editorUpdateSyntax(row);
}

void editorInsertRow(int at, const std::string& s) {
    if (at < 0 || at > (int)E.rows.size()) return;
    E.rows.insert(E.rows.begin() + at, {s, "", ""});
    editorUpdateRow(*(E.rows.begin() + at));
    E.dirty = true;
}

void editorRowInsertChar(editorRow& row, int at, int c) {
    auto& s = row.raw_row;
    if (at < 0 || at > (int)s.size()) at = (int)s.size();
    s.insert(s.begin() + at, (char)c);
    editorUpdateRow(row);
    E.dirty = true;
}

void editorRowDelChar(editorRow& row, int at) {
    auto& s = row.raw_row;
    if (at <= 0 || at > (int)s.size()) return;
    s.erase(s.begin() + at - 1);
    editorUpdateRow(row);
    E.dirty = true;
}

void editorRowAppendString(editorRow& row, const std::string& to_append) {
    row.raw_row += to_append;
    editorUpdateRow(row);
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
    if (E.cursor_y == (int)E.rows.size())
        editorInsertRow((int)E.rows.size(), "");
    editorRowInsertChar(E.rows[size_t(E.cursor_y)], E.cursor_x, c);
    E.cursor_x++;
}

void editorInsertNewline() {
    if (E.cursor_x == 0) {
        editorInsertRow(E.cursor_y, "");
    } else {
        editorInsertRow(
            E.cursor_y + 1,
            E.rows[size_t(E.cursor_y)].raw_row.substr(size_t(E.cursor_x)));
        E.rows[size_t(E.cursor_y)].raw_row =
            E.rows[size_t(E.cursor_y)].raw_row.substr(size_t(0),
                                                      size_t(E.cursor_x));
        editorUpdateRow(E.rows[size_t(E.cursor_y)]);
    }
    E.cursor_x = 0;
    E.cursor_y++;
}

void editorDelRow(int at) {
    if (at < 0 || at >= (int)E.rows.size()) return;
    E.rows.erase(E.rows.begin() + at);
    E.dirty = true;
}

void editorDelChar() {
    if (E.cursor_y == (int)E.rows.size()) return;
    if (E.cursor_x == 0 && E.cursor_y == 0) return;
    if (E.cursor_x == 0) {
        E.cursor_x = (int)E.rows[size_t(E.cursor_y - 1)].raw_row.size();
        editorRowAppendString(E.rows[size_t(E.cursor_y - 1)],
                              E.rows[size_t(E.cursor_y)].raw_row);
        editorDelRow(E.cursor_y);
        E.cursor_y--;
    } else {
        editorRowDelChar(E.rows[size_t(E.cursor_y)], E.cursor_x);
        E.cursor_x--;
    }
}

/** file i/o */

void editorOpen(char* filename) {
    E.filename = filename;
    editorSelectSyntaxHighlight();
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
    for (const auto& s : E.rows) representation += s.raw_row + '\n';
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

void editorExecuteCommand() {
    if (E.command_buf == "q") {
        if (E.dirty) {
            editorSetStatusMessage(
                "File has unsaved changes. Use :q! to force quit");
            return;
        }
        std::ignore = write(STDOUT_FILENO, "\x1b[2J", 4);
        std::ignore = write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
    } else if (E.command_buf == "q!") {
        std::ignore = write(STDOUT_FILENO, "\x1b[2J", 4);
        std::ignore = write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
    } else if (E.command_buf == "w") {
        editorSave();
    } else {
        editorSetStatusMessage("Unsupported command: %s", E.command_buf.data());
    }
}

/** input */

void editorMoveCursor(int c) {
    bool not_on_last = size_t(E.cursor_y) < E.rows.size();
    switch (c) {
        case ARROW_LEFT:
        case 'h':
            if (E.cursor_x > 0)
                E.cursor_x--;
            else if (E.cursor_y > 0) {  // to go to end of previous line
                E.cursor_y--;
                E.cursor_x = (int)E.rows[size_t(E.cursor_y)].raw_row.size();
            }
            break;
        case ARROW_RIGHT:
        case 'l':
            // to not allow overflowing past the end (one past the end allowed)
            if (not_on_last &&
                size_t(E.cursor_x) < E.rows[size_t(E.cursor_y)].raw_row.size())
                E.cursor_x++;
            // to allow going to the next line with a right movement
            else if (not_on_last &&
                     size_t(E.cursor_x) ==
                         E.rows[size_t(E.cursor_y)].raw_row.size()) {
                E.cursor_y++;
                E.cursor_x = 0;
            }
            break;
        case ARROW_DOWN:
        case 'j':
            if (E.cursor_y < (int)E.rows.size()) E.cursor_y++;
            break;
        case ARROW_UP:
        case 'k':
            if (E.cursor_y > 0) E.cursor_y--;
            break;
    }
    // snap to end - done in terms of cursor_x, not rendered_x
    int row_len = (size_t(E.cursor_y) >= E.rows.size()
                       ? 0
                       : (int)E.rows[size_t(E.cursor_y)].raw_row.size());
    if (E.cursor_x > row_len) E.cursor_x = row_len;
}

void editorProcessKeypress() {
    int c = editorReadKey();
    if (E.mode == INSERT) {
        switch (c) {
            case '\x1b':
                E.mode = NORMAL;
                break;
            case '\r':
                editorInsertNewline();
                break;
            case HOME_KEY:
                E.cursor_x = 0;
                break;
            case END_KEY:
                if (E.cursor_y < (int)E.rows.size())
                    E.cursor_x = (int)E.rows[size_t(E.cursor_y)].raw_row.size();
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
                    if (E.cursor_y > (int)E.rows.size())
                        E.cursor_y = (int)E.rows.size();
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
                break;
            default:
                editorInsertChar(c);
                break;
        }
    } else if (E.mode == NORMAL) {
        if (E.normal_buf.empty()) {
            switch (c) {
                case 'i':
                    E.normal_buf = "";
                    E.mode = INSERT;
                    break;
                case ':':
                    E.normal_buf = "";
                    E.mode = COMMAND;
                    editorSetStatusMessage(":");
                    break;
                case '\x1b':
                    E.mode = NORMAL;
                    break;
                case '\r':
                    editorMoveCursor(ARROW_DOWN);
                case HOME_KEY:
                case '0':
                    E.cursor_x = 0;
                    break;
                case END_KEY:
                case '$':
                    if (E.cursor_y < (int)E.rows.size())
                        E.cursor_x =
                            (int)E.rows[size_t(E.cursor_y)].raw_row.size();
                    break;
                case BACKSPACE:
                case CTRL_KEY('h'):
                    editorMoveCursor(ARROW_LEFT);
                    break;
                case PAGE_UP:
                case PAGE_DOWN: {
                    if (c == PAGE_UP) {
                        E.cursor_y = E.row_offset;
                    } else if (c == PAGE_DOWN) {
                        E.cursor_y = E.row_offset + E.screen_rows - 1;
                        if (E.cursor_y > (int)E.rows.size())
                            E.cursor_y = (int)E.rows.size();
                    }
                    int times = E.screen_rows;
                    while (times--)
                        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                } break;
                case ARROW_LEFT:
                case ARROW_DOWN:
                case ARROW_UP:
                case ARROW_RIGHT:
                case 'h':
                case 'j':
                case 'k':
                case 'l':
                    editorMoveCursor(c);
                    break;
                case CTRL_KEY('l'):
                    break;
                case 'G':
                    while (E.cursor_y != (int)E.rows.size())
                        editorMoveCursor(ARROW_DOWN);
            }
        } else {
            switch (c) {
                case BACKSPACE:
                    E.normal_buf.pop_back();
                    break;
            }
        }
    } else if (E.mode == COMMAND) {
        switch (c) {
            case '\r':
                editorExecuteCommand();
                E.command_buf = "";
                E.mode = NORMAL;
                break;
            case '\x1b':
                E.command_buf = "";
                E.mode = NORMAL;
                editorSetStatusMessage("");
                break;
            case BACKSPACE:
                if (!E.command_buf.empty())
                    E.command_buf.pop_back(),
                        editorSetStatusMessage((":" + E.command_buf).data());
                else
                    E.mode = NORMAL, editorSetStatusMessage("");
                break;
            default:
                E.command_buf.push_back((char)c);
                editorSetStatusMessage((":" + E.command_buf).data());
                break;
        }
    }
}

/** output */

void editorScroll() {
    E.rendered_x = 0;
    if (E.cursor_y < (int)E.rows.size())
        E.rendered_x = editorComputeRenderedX(
            E.rows[size_t(E.cursor_y)].raw_row, E.cursor_x);
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
        if (row_number >= (int)E.rows.size())
            s += '~';
        else {
            int len = (int)E.rows[size_t(row_number)].rendered_row.size() -
                      E.col_offset;
            len = std::clamp(len, 0, E.screen_cols);
            const char* c =
                E.rows[size_t(row_number)].rendered_row.c_str() + E.col_offset;
            const char* hl =
                E.rows[size_t(row_number)].highlight_row.c_str() + E.col_offset;
            int current_color = -1;
            for (int j = 0; j < len; ++j) {
                if (hl[j] == HIGHLIGHT_NORMAL) {
                    if (current_color != -1) {
                        s += "\x1b[39m";
                        current_color = -1;
                    }
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        std::ignore =
                            snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        s += buf;
                    }
                }
                s += c[j];
            }
            s += "\x1b[39m";
        }
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
    display_status += " [";
    switch (E.mode) {
        case NORMAL:
            display_status += "NORMAL";
            break;
        case INSERT:
            display_status += "INSERT";
            break;
        case COMMAND:
            display_status += "COMMAND";
            break;
    }
    display_status += "] ";
    s += display_status;
    display_status = display_status.substr(
        size_t(0), std::min(display_status.size(), size_t(E.screen_cols)));
    s += std::string(size_t(E.screen_cols) - display_status.size(), ' ');
    auto line_number = std::to_string(E.cursor_y);
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
    std::ignore = write(STDOUT_FILENO, s.c_str(), s.size());
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

void handleSIGWINCH(int t = 0) {
    std::ignore = t;
    initEditor();
    editorRefreshScreen();
}

void setSignalHandler() { signal(SIGWINCH, handleSIGWINCH); }

int main(int argc, char** argv) {
    enableRawMode();
    initEditor();
    setSignalHandler();
    if (argc >= 2) editorOpen(argv[1]);
    editorSetStatusMessage("Use :q to quit, :w to save");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
