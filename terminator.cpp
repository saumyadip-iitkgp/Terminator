/*
 * ============================================================================
 * TERMINATOR - the X11 TERMINAL
 * ============================================================================
 *
 * Multi-tab interactive terminal emulator.
 *
 * FEATURES:
 *  - Multiple independent Bash sessions
 *  - Clickable tabs and close buttons
 *  - New tab button
 *  - X11 clipboard support
 *  - Ctrl+Shift+C / Ctrl+Shift+V
 *  - PRIMARY selection and middle-click paste
 *  - Mouse text selection
 *  - Interactive keyboard input
 *  - Shell-native pipes and redirections
 *  - Shell-native dup2() handling
 *  - Ctrl+C / Ctrl+Z
 *  - fg / bg job control
 *  - Bash Readline command/path completion
 *  - Terminator .tbash script runner (terminator-bash)
 *  - poll() multi-PTY monitoring
 *  - Configurable GUI hotkeys
 *  - ANSI colors
 *  - Matrix Rain mode
 *  - Scrollback storage
 *  - Dynamic resizing
 *
 * BUILD:
 *
 * g++ -std=c++17 -O2 -Wall -Wextra -pthread terminator.cpp \
 *     -o terminator -lX11 -lutil -pthread -lrt
 *
 * RUN:
 *
 * ./terminator
 * ./terminator useProcess=True
 *
 * ============================================================================
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <pty.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <pthread.h>

#include <poll.h>
#include <signal.h>
#include <errno.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// CONFIGURATION
// ============================================================================

static constexpr int DEFAULT_WIDTH = 1100;
static constexpr int DEFAULT_HEIGHT = 750;

static constexpr int PADDING_X = 14;
static constexpr int PADDING_Y = 12;

static constexpr int TAB_HEIGHT = 34;
static constexpr int STATUS_HEIGHT = 24;

static constexpr int TAB_WIDTH = 170;
static constexpr int TAB_CLOSE_WIDTH = 25;

static constexpr int SCROLLBACK_LIMIT = 3000;
static constexpr int MAX_TABS = 32;

static constexpr int POLL_TIMEOUT = 33;

// ============================================================================
// COLORS
// ============================================================================

enum ColorIndex {
    BLACK,
    RED,
    GREEN,
    YELLOW,
    BLUE,
    MAGENTA,
    CYAN,
    WHITE,

    BRIGHT_BLACK,
    BRIGHT_RED,
    BRIGHT_GREEN,
    BRIGHT_YELLOW,
    BRIGHT_BLUE,
    BRIGHT_MAGENTA,
    BRIGHT_CYAN,
    BRIGHT_WHITE,

    COLOR_COUNT
};

// ============================================================================
// TERMINAL CELL
// ============================================================================

struct TerminalCell {

    char ch = ' ';

    int fg = BRIGHT_GREEN;
    int bg = BLACK;

    bool bold = false;
    bool underline = false;
    bool reverse = false;
};

// ============================================================================
// MATRIX DROP
// ============================================================================

struct MatrixDrop {

    int x = 0;

    float y = 0;
    float speed = 1;

    char ch = '0';
};

// ============================================================================
// HOTKEYS
// ============================================================================

struct Hotkey {

    KeySym key = NoSymbol;

    unsigned int modifiers = 0;
};

enum HotkeyAction {
    HOTKEY_NEW_TAB, HOTKEY_CLOSE_TAB, HOTKEY_NEXT_TAB, HOTKEY_PREVIOUS_TAB,
    HOTKEY_CONFIG, HOTKEY_MATRIX,
    HOTKEY_CURSOR_LEFT, HOTKEY_CURSOR_RIGHT,
    HOTKEY_HISTORY_PREVIOUS, HOTKEY_HISTORY_NEXT,
    HOTKEY_LINE_START, HOTKEY_LINE_END,
    HOTKEY_DELETE_TO_START, HOTKEY_DELETE_TO_END,
    HOTKEY_DELETE_PREVIOUS_WORD, HOTKEY_CLEAR_SCREEN, HOTKEY_EOF,
    HOTKEY_COPY, HOTKEY_PASTE,
    HOTKEY_COUNT
};

static const char* HOTKEY_NAMES[HOTKEY_COUNT] = {
    "New Tab", "Close Tab", "Next Tab", "Previous Tab",
    "Configure Hotkeys", "Toggle Matrix Mode",
    "Move Cursor Left", "Move Cursor Right",
    "Previous History Entry", "Next History Entry",
    "Move to Line Start (Ctrl+A)", "Move to Line End (Ctrl+E)",
    "Delete to Line Start (Ctrl+U)", "Delete to Line End (Ctrl+K)",
    "Delete Previous Word (Ctrl+W)", "Clear Screen (Ctrl+L)",
    "EOF / Exit on Empty Line (Ctrl+D)", "Copy Selection", "Paste Clipboard"
};

// ============================================================================
// SHARED HISTORY AND TAB WORKER BACKEND
// ============================================================================

static constexpr size_t HISTORY_CAPACITY = 10000;
static constexpr size_t HISTORY_LINE_SIZE = 4096;

struct SharedHistory {
    sem_t lock;
    uint64_t next;
    char lines[HISTORY_CAPACITY][HISTORY_LINE_SIZE];
};

static SharedHistory* g_history = nullptr;
static int g_history_fd = -1;
static bool g_history_owner = false;

static bool initializeSharedHistory() {
    char name[96];
    std::snprintf(name, sizeof(name), "/terminator_history_v2_%u", (unsigned)getuid());
    g_history_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (g_history_fd >= 0) {
        g_history_owner = true;
        if (ftruncate(g_history_fd, sizeof(SharedHistory)) != 0) return false;
    } else if (errno == EEXIST) {
        g_history_fd = shm_open(name, O_RDWR, 0600);
    }
    if (g_history_fd < 0) return false;
    void* p = mmap(nullptr, sizeof(SharedHistory), PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_history_fd, 0);
    if (p == MAP_FAILED) return false;
    g_history = static_cast<SharedHistory*>(p);
    if (g_history_owner) {
        std::memset(g_history, 0, sizeof(*g_history));
        if (sem_init(&g_history->lock, 1, 1) != 0) return false;
    }
    return true;
}

static void appendSharedHistory(const std::string& line) {
    if (!g_history || line.empty()) return;
    if (sem_wait(&g_history->lock) != 0) return;
    uint64_t slot = g_history->next++ % HISTORY_CAPACITY;
    std::snprintf(g_history->lines[slot], HISTORY_LINE_SIZE, "%s", line.c_str());
    sem_post(&g_history->lock);
}

static void closeSharedHistory() {
    if (g_history) munmap(g_history, sizeof(SharedHistory));
    if (g_history_fd >= 0) close(g_history_fd);
    g_history = nullptr;
    g_history_fd = -1;
    // The shared object intentionally remains available to concurrently running
    // Terminator instances. Its semaphore and ring are initialized only once.
}

struct TabResize {
    int cols;
    int rows;
};

static void setCloseOnExec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void closeUnrelatedFds(int keep_a, int keep_b, int keep_c) {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) limit = 65536;
    for (int fd = 3; fd < limit; ++fd) {
        if (fd != keep_a && fd != keep_b && fd != keep_c) close(fd);
    }
}

static bool writeAll(int fd, const char* data, size_t length) {
    size_t off = 0;
    while (off < length) {
        ssize_t n = write(fd, data + off, length - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

// Worker owns the PTY and shell. It has no Xlib calls and communicates with the
// GUI exclusively through three pipes: input, output, and resize.
static void runTabWorker(int input_read, int output_write, int resize_read,
                         int cols, int rows) {
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    int pty_master = -1;
    pid_t shell_pid = forkpty(&pty_master, nullptr, nullptr, &ws);
    if (shell_pid < 0) { close(input_read); close(output_write); close(resize_read); return; }
    if (shell_pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        char history_path[128];
        std::snprintf(history_path, sizeof(history_path), "/tmp/terminator_history_%u", (unsigned)getuid());
        setenv("HISTFILE", history_path, 1);
        setenv("HISTSIZE", "1000", 1);
        setenv("HISTFILESIZE", "10000", 1);
        setenv("HISTCONTROL", "ignoredups", 1);
        setenv("HISTTIMEFORMAT", "%F %T ", 1);
        setenv("HISTIGNORE", "", 1);
        // Serialize Bash history append/read operations across all tabs.
        char prompt_command[512];
        char history_lock[160];
        std::snprintf(history_lock, sizeof(history_lock), "/tmp/terminator_history_%u.lock", (unsigned)getuid());
        std::snprintf(prompt_command, sizeof(prompt_command),
            "shopt -s histappend; exec {__term_hist_lock}>%s; "
            "flock -x \"$__term_hist_lock\"; history -a; history -n; "
            "flock -u \"$__term_hist_lock\"; exec {__term_hist_lock}>&-", history_lock);
        setenv("PROMPT_COMMAND", prompt_command, 1);
        // Use a private rc file so every tab gets the same Terminator helpers
        // without modifying the user's ~/.bashrc. multiWatch starts one loop per
        // command, reports timestamped output, and is interrupted as a unit.
        char rc_path[160];
        std::snprintf(rc_path, sizeof(rc_path), "/tmp/terminator_rc_%u_%u",
                      (unsigned)getuid(), (unsigned)getpid());
        const char* rc =
            "[[ -f ~/.bashrc ]] && source ~/.bashrc\n"
            // Reassert predictable Readline completion after the user's bashrc.
            // The terminal forwards a real TAB byte; Readline owns completion.
            "shopt -s progcomp\n"
            "bind -m emacs-standard '\"\\C-i\": complete' 2>/dev/null || true\n"
            "bind 'set completion-ignore-case on' 2>/dev/null || true\n"
            "complete -D -o bashdefault -o default -o filenames 2>/dev/null || true\n"
            // Terminator's small script runner stores scripts under ~/.terminator/scripts.
            "TERMINATOR_SCRIPT_DIR=\"${TERMINATOR_SCRIPT_DIR:-$HOME/.terminator/scripts}\"\n"
            "export TERMINATOR_SCRIPT_DIR\n"
            "mkdir -p \"$TERMINATOR_SCRIPT_DIR\" 2>/dev/null\n"
            "terminator-bash() {\n"
            "  local __tb_name __tb_file; __tb_name=\"${1-}\"\n"
            "  if [[ -z $__tb_name || $__tb_name == --help || $__tb_name == -h ]]; then\n"
            "    printf '%s\\n' 'Usage: terminator-bash SCRIPT [ARGS...]' '       terminator-bash --create NAME' '       terminator-bash --list' 'Scripts live in ~/.terminator/scripts and use normal Bash syntax.'; return 0\n"
            "  fi\n"
            "  if [[ $__tb_name == --list ]]; then\n"
            "    find \"$TERMINATOR_SCRIPT_DIR\" -maxdepth 1 -type f -name '*.tbash' -printf '%f\\n' 2>/dev/null | sort; return 0\n"
            "  fi\n"
            "  if [[ $__tb_name == --create ]]; then\n"
            "    shift; __tb_name=\"${1-}\"\n"
            "    [[ -n $__tb_name && $__tb_name != */* && $__tb_name != .* ]] || { echo 'Usage: terminator-bash --create NAME' >&2; return 2; }\n"
            "    [[ $__tb_name == *.tbash ]] || __tb_name+=.tbash\n"
            "    __tb_file=\"$TERMINATOR_SCRIPT_DIR/$__tb_name\"\n"
            "    [[ ! -e $__tb_file ]] || { echo \"Already exists: $__tb_file\" >&2; return 1; }\n"
            "    printf '#!/usr/bin/env bash\\n\\n# Terminator Bash script\\nset -e\\n\\n' > \"$__tb_file\" || return 1\n"
            "    chmod 700 \"$__tb_file\"; printf 'Created %s\\n' \"$__tb_file\"; return 0\n"
            "  fi\n"
            "  shift; __tb_file=\"$__tb_name\"\n"
            "  if [[ ! -f $__tb_file ]]; then\n"
            "    [[ $__tb_name == *.tbash ]] || __tb_name+=.tbash\n"
            "    __tb_file=\"$TERMINATOR_SCRIPT_DIR/$__tb_name\"\n"
            "  fi\n"
            "  [[ -f $__tb_file && -r $__tb_file ]] || { printf 'terminator-bash: script not found: %s\\n' \"$__tb_name\" >&2; return 127; }\n"
            "  bash -- \"$__tb_file\" \"$@\"\n"
            "}\n"
            "_terminator_bash_complete() {\n"
            "  local __tb_cur=\"${COMP_WORDS[COMP_CWORD]}\" __tb_item\n"
            "  COMPREPLY=()\n"
            "  while IFS= read -r __tb_item; do COMPREPLY+=(\"$__tb_item\"); done < <(compgen -f -- \"$__tb_cur\")\n"
            "  if [[ $__tb_cur != */* ]]; then\n"
            "    while IFS= read -r __tb_item; do COMPREPLY+=(\"${__tb_item#\"$TERMINATOR_SCRIPT_DIR/\"}\"); done < <(compgen -f -- \"$TERMINATOR_SCRIPT_DIR/$__tb_cur\")\n"
            "  fi\n"
            "}\n"
            "complete -o filenames -F _terminator_bash_complete terminator-bash\n"
            "multiWatch() {\n"
            "  local -a __mw_cmds=() __mw_pids=(); local __mw_arg __mw_cmd\n"
            "  for __mw_arg in \"$@\"; do\n"
            "    [[ $__mw_arg == '[' || $__mw_arg == ']' || $__mw_arg == ',' ]] && continue\n"
            "    __mw_arg=${__mw_arg#,}; __mw_arg=${__mw_arg%,}; __mw_cmds+=(\"$__mw_arg\")\n"
            "  done\n"
            "  ((${#__mw_cmds[@]})) || { echo 'usage: multiWatch [\"cmd1\", \"cmd2\"]'; return 2; }\n"
            "  for __mw_cmd in \"${__mw_cmds[@]}\"; do\n"
            "    ( while :; do\n"
            "        __mw_ts=$(date +%s); __mw_out=$(eval \"$__mw_cmd\" 2>&1); __mw_rc=$?\n"
            "        printf '\"%s\", %s:\n' \"$__mw_cmd\" \"$__mw_ts\"\n"
            "        printf '%s\n' '----------------------------------------------------'\n"
            "        printf '%s\n' \"$__mw_out\"\n"
            "        printf '%s\n' '----------------------------------------------------'\n"
            "        sleep 2\n"
            "      done ) & __mw_pids+=(\"$!\")\n"
            "  done\n"
            "  trap 'kill \"${__mw_pids[@]}\" 2>/dev/null; wait; printf \"^C\\n\"; trap - INT; return 130' INT\n"
            "  wait; local __mw_status=$?; trap - INT; return $__mw_status\n"
            "}\n";
        int rc_fd = open(rc_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (rc_fd >= 0) { (void)writeAll(rc_fd, rc, std::strlen(rc)); close(rc_fd); }
        execlp("bash", "bash", "--rcfile", rc_path, "-i", static_cast<char*>(nullptr));
        _exit(127);
    }

    int flags = fcntl(pty_master, F_GETFL, 0);
    if (flags >= 0) fcntl(pty_master, F_SETFL, flags | O_NONBLOCK);
    bool alive = true;
    char buffer[8192];
    while (alive) {
        pollfd fds[3] = {
            {pty_master, POLLIN | POLLHUP | POLLERR, 0},
            {input_read, POLLIN | POLLHUP | POLLERR, 0},
            {resize_read, POLLIN | POLLHUP | POLLERR, 0}
        };
        int pr = poll(fds, 3, -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        if (fds[1].revents & POLLIN) {
            for (;;) {
                ssize_t n = read(input_read, buffer, sizeof(buffer));
                if (n > 0) { if (!writeAll(pty_master, buffer, static_cast<size_t>(n))) alive = false; }
                else if (n < 0 && errno == EINTR) continue;
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                else { alive = false; break; }
                break;
            }
        }
        if (fds[2].revents & POLLIN) {
            TabResize r{};
            ssize_t n = read(resize_read, &r, sizeof(r));
            if (n == sizeof(r)) {
                struct winsize new_ws{};
                new_ws.ws_col = static_cast<unsigned short>(std::max(1, r.cols));
                new_ws.ws_row = static_cast<unsigned short>(std::max(1, r.rows));
                ioctl(pty_master, TIOCSWINSZ, &new_ws);
                kill(-shell_pid, SIGWINCH);
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            for (;;) {
                ssize_t n = read(pty_master, buffer, sizeof(buffer));
                if (n > 0) { if (!writeAll(output_write, buffer, static_cast<size_t>(n))) alive = false; }
                else if (n == 0) { alive = false; break; }
                else if (errno == EINTR) continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else { alive = false; break; }
            }
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) alive = false;
        int status = 0;
        pid_t w = waitpid(shell_pid, &status, WNOHANG);
        if (w == shell_pid) alive = false;
    }
    kill(-shell_pid, SIGHUP);
    int status = 0;
    if (waitpid(shell_pid, &status, WNOHANG) == 0) {
        kill(-shell_pid, SIGTERM);
        waitpid(shell_pid, &status, 0);
    }
    close(pty_master);
    char rc_cleanup[160];
    std::snprintf(rc_cleanup, sizeof(rc_cleanup), "/tmp/terminator_rc_%u_%u",
                  (unsigned)getuid(), (unsigned)getpid());
    unlink(rc_cleanup);
    close(input_read);
    close(resize_read);
    close(output_write);
    return;
}

// ============================================================================
// TERMINAL SESSION
// ============================================================================

class TerminalSession {

public:

    pid_t pid = -1;

    // GUI-facing pipe ends. The worker, not the GUI, owns the PTY master.
    int master = -1;       // worker -> GUI output
    int input_fd = -1;     // GUI -> worker input
    int resize_fd = -1;    // GUI -> worker resize messages
    pid_t worker_pid = -1;
    std::thread worker_thread;
    bool process_mode = false;
    std::string pending_command;

    bool alive = true;

    int cols = 100;
    int rows = 35;

    int cursor_x = 0;
    int cursor_y = 0;
    int saved_cursor_x = 0;
    int saved_cursor_y = 0;

    int current_fg = BRIGHT_GREEN;
    int current_bg = BLACK;

    bool current_bold = false;

    std::string title = "Terminal";

    std::vector<std::vector<TerminalCell>> grid;

    std::deque<std::vector<TerminalCell>> scrollback;
    int scroll_offset = 0;

    enum ParserState {

        GROUND,

        ESCAPE,

        CSI,

        OSC

    };

    ParserState parser = GROUND;

    std::string ansi_buffer;

    TerminalSession() = default;

    ~TerminalSession() {

        closeSession();
    }

    // ------------------------------------------------------------------------
    // Initialize grid
    // ------------------------------------------------------------------------

    void initialize(int width, int height) {

        resize(width, height);
    }

    // ------------------------------------------------------------------------
    // Resize
    // ------------------------------------------------------------------------

    void resize(int width, int height) {

        cols = std::max(20, width);

        rows = std::max(5, height);

        std::vector<std::vector<TerminalCell>> new_grid(

            rows,

            std::vector<TerminalCell>(
                cols,
                TerminalCell{}
            )
        );

        int copy_rows = std::min(
            rows,
            static_cast<int>(grid.size())
        );

        for (int y = 0; y < copy_rows; ++y) {

            int copy_cols = std::min(
                cols,
                static_cast<int>(grid[y].size())
            );

            for (int x = 0; x < copy_cols; ++x) {

                new_grid[y][x] = grid[y][x];
            }
        }

        grid.swap(new_grid);

        cursor_x = std::clamp(cursor_x, 0, cols - 1);

        cursor_y = std::clamp(cursor_y, 0, rows - 1);

        if (resize_fd >= 0) {
            TabResize r{cols, rows};
            (void)write(resize_fd, &r, sizeof(r));
        }
    }

    // ------------------------------------------------------------------------
    // Clear screen
    // ------------------------------------------------------------------------

    void clearScreen() {

        for (auto& row : grid) {

            std::fill(

                row.begin(),

                row.end(),

                TerminalCell{}
            );
        }

        cursor_x = 0;

        cursor_y = 0;
    }

    // ------------------------------------------------------------------------
    // Scroll
    // ------------------------------------------------------------------------


    void scrollBy(int lines) {
        int max_offset = static_cast<int>(scrollback.size());

        scroll_offset = std::clamp(
            scroll_offset + lines,
            0,
            max_offset
        );
    }

    void scrollToBottom() {
        scroll_offset = 0;
    }


    void scrollUp() {

    if (grid.empty()) return;

    scrollback.push_back(grid.front());

    if (scrollback.size() > SCROLLBACK_LIMIT) {
        scrollback.pop_front();
    }

    // Preserve the current historical view as new output arrives.
    if (scroll_offset > 0) {
        scroll_offset = std::min(
            scroll_offset + 1,
            static_cast<int>(scrollback.size())
        );
    }

    for (int y = 0; y < rows - 1; ++y) {
        grid[y] = grid[y + 1];
    }

    grid[rows - 1] = std::vector<TerminalCell>(
        cols,
        TerminalCell{}
    );
}


const std::vector<TerminalCell>& getVisibleLine(int y) const {

    int total_lines =
        static_cast<int>(scrollback.size()) + rows;

    int start =
        total_lines - rows - scroll_offset;

    int index = start + y;

    if (index < static_cast<int>(scrollback.size())) {
        return scrollback[index];
    }

    return grid[index - static_cast<int>(scrollback.size())];
}

    // ------------------------------------------------------------------------
    // Character output
    // ------------------------------------------------------------------------

    void putCharacter(char c) {

        if (c == '\n') {

            cursor_x = 0;

            cursor_y++;

        } else if (c == '\r') {

            cursor_x = 0;

        } else if (c == '\b') {

            // In a terminal, BS moves the cursor; it does not erase the
            // character underneath it. Readline uses this while repositioning
            // the cursor during interactive line editing. Erasing here made
            // characters disappear whenever the user pressed Left.
            if (cursor_x > 0) cursor_x--;

        } else if (static_cast<unsigned char>(c) == 127) {

            // DEL is not a printable terminal-output character. Keyboard
            // Backspace is sent to the PTY and Bash decides how to edit text.

        } else if (c == '\t') {

            cursor_x = (cursor_x + 8) & ~7;

            if (cursor_x >= cols) {

                cursor_x = 0;

                cursor_y++;
            }

        } else if (static_cast<unsigned char>(c) >= 32) {

            if (cursor_x >= cols) {

                cursor_x = 0;

                cursor_y++;
            }

            if (cursor_y >= rows) {

                scrollUp();

                cursor_y = rows - 1;
            }

            TerminalCell cell;

            cell.ch = c;

            cell.fg = current_fg;

            cell.bg = current_bg;

            cell.bold = current_bold;

            grid[cursor_y][cursor_x] = cell;

            cursor_x++;
        }

        if (cursor_y >= rows) {

            scrollUp();

            cursor_y = rows - 1;
        }

        cursor_x = std::clamp(cursor_x, 0, cols - 1);
    }

    // ------------------------------------------------------------------------
    // ANSI CSI parser
    // ------------------------------------------------------------------------

    void executeCSI(char command) {

        std::vector<int> params;

        std::string token;

        for (char c : ansi_buffer) {

            if (c == ';') {

                params.push_back(
                    token.empty() ? 0 : std::atoi(token.c_str())
                );

                token.clear();

            } else if (c >= '0' && c <= '9') {

                token += c;
            }
        }

        if (!token.empty() || !ansi_buffer.empty()) {

            params.push_back(
                token.empty() ? 0 : std::atoi(token.c_str())
            );
        }

        int p1 = params.empty() || params[0] == 0
            ? 1 : params[0];

        int p2 = params.size() > 1 && params[1] != 0
            ? params[1] : 1;

        switch (command) {

            case 'A':

                cursor_y = std::max(0, cursor_y - p1);

                break;

            case 'B':

                cursor_y = std::min(rows - 1, cursor_y + p1);

                break;

            case 'C':

                cursor_x = std::min(cols - 1, cursor_x + p1);

                break;

            case 'D':

                cursor_x = std::max(0, cursor_x - p1);

                break;

            case 'H':
            case 'f':

                cursor_y = std::clamp(p1 - 1, 0, rows - 1);

                cursor_x = std::clamp(p2 - 1, 0, cols - 1);

                break;

            case 'G': // Horizontal absolute cursor position.
                cursor_x = std::clamp(p1 - 1, 0, cols - 1);
                break;

            case 'd': // Vertical absolute cursor position.
                cursor_y = std::clamp(p1 - 1, 0, rows - 1);
                break;

            case 'E': // Next line, column 1.
                cursor_y = std::min(rows - 1, cursor_y + p1);
                cursor_x = 0;
                break;

            case 'F': // Previous line, column 1.
                cursor_y = std::max(0, cursor_y - p1);
                cursor_x = 0;
                break;

            case 's': // Save/restore cursor (used by some Readline redraws).
                saved_cursor_x = cursor_x;
                saved_cursor_y = cursor_y;
                break;

            case 'u':
                cursor_x = std::clamp(saved_cursor_x, 0, cols - 1);
                cursor_y = std::clamp(saved_cursor_y, 0, rows - 1);
                break;

            case '@': { // Insert blank characters at cursor.
                int count = std::clamp(p1, 1, cols - cursor_x);
                auto& line = grid[cursor_y];
                std::move_backward(line.begin() + cursor_x,
                                   line.end() - count, line.end());
                std::fill(line.begin() + cursor_x,
                          line.begin() + cursor_x + count, TerminalCell{});
                break;
            }

            case 'P': { // Delete characters at cursor.
                int count = std::clamp(p1, 1, cols - cursor_x);
                auto& line = grid[cursor_y];
                std::move(line.begin() + cursor_x + count, line.end(),
                          line.begin() + cursor_x);
                std::fill(line.end() - count, line.end(), TerminalCell{});
                break;
            }

            case 'X': { // Erase characters without moving the cursor.
                int count = std::clamp(p1, 1, cols - cursor_x);
                auto& line = grid[cursor_y];
                std::fill(line.begin() + cursor_x,
                          line.begin() + cursor_x + count, TerminalCell{});
                break;
            }

            case 'J':

                if (p1 == 2 || p1 == 3) {

                    clearScreen();

                } else if (p1 == 0) {

                    for (int y = cursor_y; y < rows; ++y) {

                        int start = y == cursor_y ? cursor_x : 0;

                        for (int x = start; x < cols; ++x) {

                            grid[y][x] = TerminalCell{};
                        }
                    }

                } else if (p1 == 1) {

                    for (int y = 0; y <= cursor_y; ++y) {

                        int end = y == cursor_y
                            ? cursor_x : cols - 1;

                        for (int x = 0; x <= end; ++x) {

                            grid[y][x] = TerminalCell{};
                        }
                    }
                }

                break;

            case 'K': {
                    int erase_mode = params.empty() ? 0 : params[0];

                    if (erase_mode == 0) {
                        // Erase from cursor to end.
                        for (int x = cursor_x; x < cols; ++x)
                            grid[cursor_y][x] = TerminalCell{};

                    } else if (erase_mode == 1) {
                        // Erase from beginning to cursor.
                        for (int x = 0; x <= cursor_x; ++x)
                            grid[cursor_y][x] = TerminalCell{};

                    } else if (erase_mode == 2) {
                        // Erase entire line.
                        std::fill(
                            grid[cursor_y].begin(),
                            grid[cursor_y].end(),
                            TerminalCell{}
                        );
                    }

                    break;
                }


            case 'm':

                if (params.empty()) {

                    current_fg = BRIGHT_GREEN;

                    current_bg = BLACK;

                    current_bold = false;

                    break;
                }

                for (size_t i = 0; i < params.size(); ++i) {

                    int code = params[i];

                    if (code == 0) {

                        current_fg = BRIGHT_GREEN;

                        current_bg = BLACK;

                        current_bold = false;

                    } else if (code == 1) {

                        current_bold = true;

                    } else if (code == 22) {

                        current_bold = false;

                    } else if (code >= 30 && code <= 37) {

                        current_fg = code - 30 + 1;

                    } else if (code >= 40 && code <= 47) {

                        current_bg = code - 40 + 1;

                    } else if (code >= 90 && code <= 97) {

                        current_fg = code - 90 + 9;

                    } else if (code >= 100 && code <= 107) {

                        current_bg = code - 100 + 9;

                    } else if (code == 39) {

                        current_fg = BRIGHT_GREEN;

                    } else if (code == 49) {

                        current_bg = BLACK;
                    }
                }

                break;

            default:

                break;
        }
    }

    // ------------------------------------------------------------------------
    // ANSI parser
    // ------------------------------------------------------------------------

    void parseANSI(char c) {

        switch (parser) {

            case GROUND:

                if (c == '\033') {

                    parser = ESCAPE;

                } else {

                    putCharacter(c);
                }

                break;

            case ESCAPE:

                if (c == '[') {

                    parser = CSI;

                    ansi_buffer.clear();

                } else if (c == ']') {

                    parser = OSC;

                } else if (c == 'c') {

                    clearScreen();

                    parser = GROUND;

                } else {

                    parser = GROUND;
                }

                break;

            case CSI:

                if (c >= 0x40 && c <= 0x7e) {

                    executeCSI(c);

                    parser = GROUND;

                } else if (ansi_buffer.size() < 256) {

                    ansi_buffer += c;
                }

                break;

            case OSC:

                if (c == '\a') {

                    parser = GROUND;

                } else if (c == '\033') {

                    parser = ESCAPE;
                }

                break;
        }
    }

    void feed(const char* data, ssize_t length) {

        for (ssize_t i = 0; i < length; ++i) {

            parseANSI(data[i]);
        }
    }

    // ------------------------------------------------------------------------
    // Start shell
    // ------------------------------------------------------------------------

    bool startShell(bool use_process_mode) {
        int input_pipe[2] = {-1, -1};
        int output_pipe[2] = {-1, -1};
        int resize_pipe[2] = {-1, -1};
        if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0 || pipe(resize_pipe) != 0) {
            perror("pipe");
            for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], resize_pipe[0], resize_pipe[1]})
                if (fd >= 0) close(fd);
            return false;
        }
        for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], resize_pipe[0], resize_pipe[1]})
            setCloseOnExec(fd);
        process_mode = use_process_mode;
        if (use_process_mode) {
            pid_t p = fork();
            if (p < 0) {
                perror("fork tab worker");
                for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], resize_pipe[0], resize_pipe[1]}) close(fd);
                return false;
            }
            if (p == 0) {
                close(input_pipe[1]);
                close(output_pipe[0]);
                close(resize_pipe[1]);
                closeUnrelatedFds(input_pipe[0], output_pipe[1], resize_pipe[0]);
                runTabWorker(input_pipe[0], output_pipe[1], resize_pipe[0], cols, rows);
                _exit(0);
            }
            worker_pid = p;
        } else {
            worker_thread = std::thread([ir=input_pipe[0], ow=output_pipe[1], rr=resize_pipe[0], c=cols, r=rows]() {
                runTabWorker(ir, ow, rr, c, r);
            });
        }
        // With forked workers, the parent closes the worker ends. Threads share
        // the process fd table, so the worker thread itself closes its ends on exit.
        if (use_process_mode) {
            close(input_pipe[0]);
            close(output_pipe[1]);
            close(resize_pipe[0]);
        }
        input_fd = input_pipe[1];
        master = output_pipe[0];
        resize_fd = resize_pipe[1];
        int fl = fcntl(master, F_GETFL, 0);
        if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    // ------------------------------------------------------------------------
    // Send input
    // ------------------------------------------------------------------------

    void sendInput(const char* data, size_t length) {
        if (input_fd < 0 || !alive) return;
        size_t off = 0;
        while (off < length) {
            ssize_t n = write(input_fd, data + off, length - off);
            if (n > 0) off += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        // Maintain a process-shared, semaphore-protected command history ring.
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = static_cast<unsigned char>(data[i]);
            if (c == '\r' || c == '\n') {
                if (!pending_command.empty()) appendSharedHistory(pending_command);
                pending_command.clear();
            } else if (c == 127 || c == 8) {
                if (!pending_command.empty()) pending_command.pop_back();
            } else if (c >= 32 && c != 127 && pending_command.size() < HISTORY_LINE_SIZE - 1) {
                pending_command.push_back(static_cast<char>(c));
            }
        }
    }

    // ------------------------------------------------------------------------
    // Close session
    // ------------------------------------------------------------------------

    void closeSession() {
        alive = false;
        if (input_fd >= 0) { close(input_fd); input_fd = -1; }
        if (resize_fd >= 0) { close(resize_fd); resize_fd = -1; }
        if (master >= 0) { close(master); master = -1; }
        if (worker_pid > 0) {
            kill(worker_pid, SIGTERM);
            int status = 0;
            if (waitpid(worker_pid, &status, WNOHANG) == 0) {
                kill(worker_pid, SIGKILL);
                waitpid(worker_pid, &status, 0);
            }
            worker_pid = -1;
        }
        if (worker_thread.joinable()) worker_thread.join();
        pid = -1;
    }
};

// ============================================================================
// MAIN TERMINAL
// ============================================================================

class X11Terminal {

private:

    Display* display = nullptr;

    int screen = 0;

    Window window = 0;

    Window config_window = 0;

    GC gc = nullptr;

    Pixmap buffer = 0;

    XFontStruct* font = nullptr;

    Atom wm_delete;

    // Clipboard atoms.

    Atom clipboard_atom;

    Atom primary_atom;

    Atom utf8_atom;

    Atom targets_atom;

    Atom paste_property;

    // Dimensions.

    int width = DEFAULT_WIDTH;

    int height = DEFAULT_HEIGHT;

    int char_width = 9;

    int line_height = 18;

    int terminal_cols = 100;

    int terminal_rows = 35;

    // Colors.

    unsigned long palette[COLOR_COUNT]{};

    unsigned long default_bg = 0;

    unsigned long default_fg = 0;

    // Tabs.

    std::vector<std::unique_ptr<TerminalSession>> tabs;

    int active_tab = 0;

    // Runtime.

    bool running = true;

    bool matrix_mode = false;

    bool use_process_mode = false;

    // Matrix rain.

    std::vector<MatrixDrop> matrix_drops;

    std::mt19937 rng{

        static_cast<unsigned int>(

            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count()
        )
    };

    // Hotkeys.

    Hotkey hotkeys[HOTKEY_COUNT]{};
    int selected_hotkey = 0;
    bool config_open = false;
    bool capturing_hotkey = false;
    std::string hotkey_config_message;

    // Selection.

    bool selecting = false;

    bool selection_valid = false;

    int selection_start_x = 0;

    int selection_start_y = 0;

    int selection_end_x = 0;

    int selection_end_y = 0;

    std::string selected_text;

    // Keep a stable copy independent of the live mouse-selection state.
    // Pasting from our own X11 CLIPBOARD owner uses this cache directly.
    std::string clipboard_text;

    // Clipboard.

    bool paste_pending = false;

    bool paste_fallback = false;

    Atom paste_target = None;

    // ========================================================================
    // COLOR MANAGEMENT
    // ========================================================================

    unsigned long allocateColor(const char* hex) {

        XColor color;

        XColor exact;

        if (XAllocNamedColor(

            display,

            DefaultColormap(display, screen),

            hex,

            &color,

            &exact
        )) {

            return color.pixel;
        }

        return WhitePixel(display, screen);
    }

    void initializeColors() {

        const char* colors[COLOR_COUNT] = {
    // Neutrals 
    "#050811",
    "#1b262c",
    "#10ac84",
    "#718093",
    
    // Greens & Limes
    "#0c5c25",
    "#1dd1a1",
    "#2ed573",
    "#a3cb38",
    "#1f4ca6",
    "#20bf6b",
    
    // Yellow Accents
    "#f39c12",
    "#feca57",
    "#ffd32a",
    "#ffeaa7",
    
    // Light Neutrals / Highlights
    "#dcdde1",
    "#ffffff"
};

        for (int i = 0; i < COLOR_COUNT; ++i) {

            palette[i] = allocateColor(colors[i]);
        }

        default_bg = palette[BLACK];

        default_fg = palette[BRIGHT_GREEN];
    }

    unsigned long terminalColor(int index, bool foreground) {

        if (index < 0 || index >= COLOR_COUNT) {

            return foreground ? default_fg : default_bg;
        }

        return palette[index];
    }

    // ========================================================================
    // MATRIX
    // ========================================================================

    void drawScrollbar() {

        TerminalSession* session = currentSession();

        if (!session) return;

        int track_x = width - 10;
        int track_y = TAB_HEIGHT + PADDING_Y;

        int track_height =
            height - TAB_HEIGHT - STATUS_HEIGHT - 2 * PADDING_Y;

        if (track_height <= 0) return;

        int total_lines =
            static_cast<int>(session->scrollback.size())
            + session->rows;

        int visible_lines = session->rows;

        // Background track.
        XSetForeground(display, gc, palette[BRIGHT_BLACK]);

        XFillRectangle(
            display, buffer, gc,
            track_x, track_y,
            6, track_height
        );

        // Calculate thumb size.
        int thumb_height = std::max(
            20,
            track_height * visible_lines /
                std::max(1, total_lines)
        );

        thumb_height = std::min(thumb_height, track_height);

        int max_offset =
            static_cast<int>(session->scrollback.size());

        int thumb_y = track_y;

        if (max_offset > 0) {

            thumb_y +=
                (track_height - thumb_height)
                * (max_offset - session->scroll_offset)
                / max_offset;
        }

        // Scrollbar thumb.
        XSetForeground(display, gc, palette[BRIGHT_GREEN]);

        XFillRectangle(
            display, buffer, gc,
            track_x, thumb_y,
            6, thumb_height
        );
    }
    void initializeMatrix() {

        matrix_drops.clear();

        int count = std::max(1, width / 20);

        for (int i = 0; i < count; ++i) {

            MatrixDrop drop;

            drop.x = std::uniform_int_distribution<int>(
                0,
                terminal_cols - 1
            )(rng);

            drop.y = std::uniform_real_distribution<float>(
                -terminal_rows,
                0
            )(rng);

            drop.speed = std::uniform_real_distribution<float>(
                0.3f,
                1.8f
            )(rng);

            drop.ch = static_cast<char>(
                33 + rng() % 90
            );

            matrix_drops.push_back(drop);
        }
    }

    // ========================================================================
    // DIMENSIONS
    // ========================================================================

    void calculateDimensions() {

        terminal_cols = std::max(

            20,

            (width - 2 * PADDING_X - 16) / char_width
        );

        terminal_rows = std::max(

            5,

            (height - TAB_HEIGHT - STATUS_HEIGHT
                - 2 * PADDING_Y) / line_height
        );
    }

    void resizeAllTabs() {

        calculateDimensions();

        for (auto& tab : tabs) {

            tab->resize(
                terminal_cols,
                terminal_rows
            );
        }

        if (buffer) {

            XFreePixmap(display, buffer);
        }

        buffer = XCreatePixmap(

            display,

            window,

            width,

            height,

            DefaultDepth(display, screen)
        );

        initializeMatrix();
    }

    // ========================================================================
    // CURRENT SESSION
    // ========================================================================

    TerminalSession* currentSession() {

        if (tabs.empty()) return nullptr;

        if (active_tab < 0 ||
            active_tab >= static_cast<int>(tabs.size())) {

            return nullptr;
        }

        return tabs[active_tab].get();
    }

    // ========================================================================
    // TAB MANAGEMENT
    // ========================================================================

    bool createTab() {

        if (tabs.size() >= MAX_TABS) {

            return false;
        }

        auto session = std::make_unique<TerminalSession>();

        session->initialize(
            terminal_cols,
            terminal_rows
        );

        if (!session->startShell(use_process_mode)) {

            return false;
        }

        tabs.push_back(std::move(session));

        active_tab = static_cast<int>(tabs.size()) - 1;

        selected_text.clear();

        selection_valid = false;

        redraw();

        return true;
    }

    void closeTab(int index) {

        if (index < 0 ||
            index >= static_cast<int>(tabs.size())) {

            return;
        }

        tabs.erase(tabs.begin() + index);

        if (tabs.empty()) {

            running = false;

            return;
        }

        if (active_tab > index) {

            active_tab--;

        } else if (active_tab == index) {

            active_tab = std::min(
                index,
                static_cast<int>(tabs.size()) - 1
            );
        }

        selected_text.clear();

        selection_valid = false;

        selecting = false;

        redraw();
    }

    void nextTab() {

        if (tabs.empty()) return;

        active_tab = (
            active_tab + 1
        ) % static_cast<int>(tabs.size());

        selected_text.clear();

        selection_valid = false;

        redraw();
    }

    void previousTab() {

        if (tabs.empty()) return;

        active_tab = (
            active_tab - 1 + static_cast<int>(tabs.size())
        ) % static_cast<int>(tabs.size());

        selected_text.clear();

        selection_valid = false;

        redraw();
    }

    // ========================================================================
    // DRAWING
    // ========================================================================

    void drawText(

        Drawable drawable,

        int x,

        int y,

        const std::string& text,

        unsigned long color
    ) {

        XSetForeground(display, gc, color);

        XDrawString(

            display,

            drawable,

            gc,

            x,

            y,

            text.c_str(),

            static_cast<int>(text.size())
        );
    }

    static KeySym normalizeHotkeyKey(KeySym key) {
        KeySym lower, upper;

        XConvertCase(key, &lower, &upper);

    return lower;
    }

    // ------------------------------------------------------------------------
    // Tabs with close buttons
    // ------------------------------------------------------------------------

    void drawTabs() {

        XSetForeground(
            display,
            gc,
            palette[BRIGHT_BLACK]
        );

        XFillRectangle(
            display,
            buffer,
            gc,
            0,
            0,
            width,
            TAB_HEIGHT
        );

        for (size_t i = 0; i < tabs.size(); ++i) {

            int x = 5 + static_cast<int>(i) * TAB_WIDTH;

            bool active = (
                static_cast<int>(i) == active_tab
            );

            XSetForeground(

                display,

                gc,

                active
                    ? palette[BLACK]
                    : palette[BRIGHT_BLACK]
            );

            XFillRectangle(

                display,

                buffer,

                gc,

                x,

                3,

                TAB_WIDTH - 5,

                TAB_HEIGHT - 5
            );

            std::string title =
                "Terminal " + std::to_string(i + 1);

            drawText(

                buffer,

                x + 9,

                21,

                title,

                active
                    ? palette[BRIGHT_GREEN]
                    : palette[WHITE]
            );

            // Close button.

            int close_x = x + TAB_WIDTH - 29;

            XSetForeground(

                display,

                gc,

                active
                    ? palette[RED]
                    : palette[BRIGHT_BLACK]
            );

            XFillRectangle(

                display,

                buffer,

                gc,

                close_x,

                6,

                21,

                21
            );

            drawText(

                buffer,

                close_x + 6,

                21,

                "x",

                palette[BRIGHT_WHITE]
            );
        }

        // New tab button.

        int new_x = 8 +
            static_cast<int>(tabs.size()) * TAB_WIDTH;

        drawText(

            buffer,

            new_x,

            21,

            "+ NEW",

            palette[CYAN]
        );
    }

    // ------------------------------------------------------------------------
    // Matrix rendering
    // ------------------------------------------------------------------------

    void drawMatrix() {

        if (!matrix_mode) return;

        XSetForeground(
            display,
            gc,
            palette[BRIGHT_BLACK]
        );

        for (auto& drop : matrix_drops) {

            drop.y += drop.speed;

            if (drop.y > terminal_rows) {

                drop.y = 0;

                drop.x = std::uniform_int_distribution<int>(
                    0,
                    terminal_cols - 1
                )(rng);
            }

            int y = static_cast<int>(drop.y);

            if (y >= 0 && y < terminal_rows) {

                char str[2] = {
                    drop.ch,
                    '\0'
                };

                XDrawString(

                    display,

                    buffer,

                    gc,

                    PADDING_X + drop.x * char_width,

                    TAB_HEIGHT + PADDING_Y
                        + font->ascent
                        + y * line_height,

                    str,

                    1
                );
            }
        }
    }

    // ------------------------------------------------------------------------
    // Selection comparison
    // ------------------------------------------------------------------------

    bool isCellSelected(int x, int y) {

        if (!selecting && !selection_valid) {

            return false;
        }

        int sx = selection_start_x;

        int sy = selection_start_y;

        int ex = selection_end_x;

        int ey = selection_end_y;

        if (sy > ey || (sy == ey && sx > ex)) {

            std::swap(sx, ex);

            std::swap(sy, ey);
        }

        if (y < sy || y > ey) return false;

        if (sy == ey) {

            return x >= sx && x <= ex;
        }

        if (y == sy) return x >= sx;

        if (y == ey) return x <= ex;

        return true;
    }

    // ------------------------------------------------------------------------
    // Terminal rendering
    // ------------------------------------------------------------------------

    void drawTerminal() {

        TerminalSession* session = currentSession();

        if (!session) return;

        for (int y = 0; y < session->rows; ++y) {

            for (int x = 0; x < session->cols; ++x) {

                const TerminalCell& cell = session->getVisibleLine(y)[x];

                int px = PADDING_X + x * char_width;

                int py = TAB_HEIGHT + PADDING_Y
                    + y * line_height;

                bool selected = isCellSelected(x, y);

                unsigned long foreground =
                    terminalColor(cell.fg, true);

                unsigned long background =
                    terminalColor(cell.bg, false);

                if (cell.reverse) {

                    std::swap(foreground, background);
                }

                if (selected) {

                    background = palette[MAGENTA];

                    foreground = palette[BLACK];
                }

                if (background != default_bg || selected) {

                    XSetForeground(
                        display,
                        gc,
                        background
                    );

                    XFillRectangle(

                        display,

                        buffer,

                        gc,

                        px,

                        py,

                        char_width,

                        line_height
                    );
                }

                if (cell.ch != ' ' && cell.ch != '\0') {

                    char text[2] = {
                        cell.ch,
                        '\0'
                    };

                    drawText(

                        buffer,

                        px,

                        py + font->ascent,

                        text,

                        foreground
                    );

                    if (cell.underline) {

                        XSetForeground(
                            display,
                            gc,
                            foreground
                        );

                        XDrawLine(

                            display,

                            buffer,

                            gc,

                            px,

                            py + line_height - 2,

                            px + char_width,

                            py + line_height - 2
                        );
                    }
                }
            }
        }

        // Cursor.

        XSetForeground(
            display,
            gc,
            palette[CYAN]
        );

        XDrawRectangle(

            display,

            buffer,

            gc,

            PADDING_X + session->cursor_x * char_width,

            TAB_HEIGHT + PADDING_Y
                + session->cursor_y * line_height,

            char_width,

            line_height
        );
    }

    // ------------------------------------------------------------------------
    // Status bar
    // ------------------------------------------------------------------------

    void drawStatusBar() {

        int y = height - STATUS_HEIGHT;

        XSetForeground(
            display,
            gc,
            palette[BRIGHT_BLACK]
        );

        XFillRectangle(

            display,

            buffer,

            gc,

            0,

            y,

            width,

            STATUS_HEIGHT
        );

        char status[256];

        std::snprintf(

            status,

            sizeof(status),

            " TERMINATOR | TABS: %zu | ACTIVE: %d | "
            "MATRIX: %s | CTRL+SHIFT+K: HOTKEYS",

            tabs.size(),

            active_tab + 1,

            matrix_mode ? "ON" : "OFF"
        );

        drawText(

            buffer,

            PADDING_X,

            y + 16,

            status,

            palette[BRIGHT_GREEN]
        );
    }

    // ------------------------------------------------------------------------
    // Redraw
    // ------------------------------------------------------------------------

    void redraw() {

        if (!display || !buffer) return;

        XSetForeground(
            display,
            gc,
            default_bg
        );

        XFillRectangle(

            display,

            buffer,

            gc,

            0,

            0,

            width,

            height
        );

        drawTabs();

        drawMatrix();

        drawTerminal();

        drawStatusBar();

        drawScrollbar();

        XCopyArea(

            display,

            buffer,

            window,

            gc,

            0,

            0,

            width,

            height,

            0,

            0
        );

        XFlush(display);
    }

    // ========================================================================
    // CLIPBOARD
    // ========================================================================

    void requestPaste(Atom selection) {

        // If this terminal owns CLIPBOARD, do not ask the X server to convert
        // our own selection back to us.  Deliver the cached bytes directly;
        // this keeps Ctrl+Shift+V reliable even if the visible selection has
        // since changed or the X selection round-trip races with redraws.
        if (selection == clipboard_atom &&
            XGetSelectionOwner(display, clipboard_atom) == window) {
            TerminalSession* session = currentSession();
            if (session && !clipboard_text.empty()) {
                session->sendInput(clipboard_text.data(), clipboard_text.size());
            }
            redraw();
            return;
        }

        paste_target = selection;

        paste_pending = true;

        paste_fallback = false;

        XConvertSelection(

            display,

            selection,

            utf8_atom,

            paste_property,

            window,

            CurrentTime
        );

        XFlush(display);
    }

    // ------------------------------------------------------------------------
    // Clipboard response
    // ------------------------------------------------------------------------

    void handleSelectionNotify(XSelectionEvent& event) {

        if (!paste_pending) return;

        if (event.property == None) {

            if (!paste_fallback) {

                paste_fallback = true;

                XConvertSelection(

                    display,

                    paste_target,

                    XA_STRING,

                    paste_property,

                    window,

                    CurrentTime
                );

                XFlush(display);

                return;
            }

            paste_pending = false;

            return;
        }

        Atom actual_type = None;

        int actual_format = 0;

        unsigned long count = 0;

        unsigned long remaining = 0;

        unsigned char* data = nullptr;

        int result = XGetWindowProperty(

            display,

            window,

            paste_property,

            0,

            1024 * 1024,

            True,

            AnyPropertyType,

            &actual_type,

            &actual_format,

            &count,

            &remaining,

            &data
        );

        if (result == Success &&
            data &&
            actual_format == 8) {

            TerminalSession* session = currentSession();

            if (session && count > 0) {

                session->sendInput(

                    reinterpret_cast<char*>(data),

                    count
                );
            }
        }

        if (data) {

            XFree(data);
        }

        paste_pending = false;

        redraw();
    }

    // ------------------------------------------------------------------------
    // Extract selected text
    // ------------------------------------------------------------------------

    void updateSelectedText() {

        TerminalSession* session = currentSession();

        if (!session) return;

        selected_text.clear();

        int sx = selection_start_x;

        int sy = selection_start_y;

        int ex = selection_end_x;

        int ey = selection_end_y;

        if (sy > ey || (sy == ey && sx > ex)) {

            std::swap(sx, ex);

            std::swap(sy, ey);
        }

        sx = std::clamp(sx, 0, session->cols - 1);

        ex = std::clamp(ex, 0, session->cols - 1);

        sy = std::clamp(sy, 0, session->rows - 1);

        ey = std::clamp(ey, 0, session->rows - 1);

        for (int y = sy; y <= ey; ++y) {

            int start = y == sy ? sx : 0;

            int end = y == ey ? ex : session->cols - 1;

            for (int x = start; x <= end; ++x) {

                selected_text += session->grid[y][x].ch;
            }

            if (y != ey) {

                selected_text += '\n';
            }
        }

        while (!selected_text.empty() &&
               selected_text.back() == ' ') {

            selected_text.pop_back();
        }
    }

    // ------------------------------------------------------------------------
    // Copy selection
    // ------------------------------------------------------------------------

    void copySelection() {

        if (selected_text.empty()) return;

        clipboard_text = selected_text;

        XSetSelectionOwner(

            display,

            clipboard_atom,

            window,

            CurrentTime
        );

        XSetSelectionOwner(

            display,

            primary_atom,

            window,

            CurrentTime
        );

        XFlush(display);
    }

    // ------------------------------------------------------------------------
    // Respond to clipboard requests
    // ------------------------------------------------------------------------

    void handleSelectionRequest(XSelectionRequestEvent& request) {

        XSelectionEvent response{};

        response.type = SelectionNotify;

        response.display = display;

        response.requestor = request.requestor;

        response.selection = request.selection;

        response.target = request.target;

        response.time = request.time;

        response.property = None;

        Atom property = request.property;

        if (property == None) {

            property = request.target;
        }

        if (request.target == targets_atom) {

            Atom supported[] = {

                targets_atom,

                utf8_atom,

                XA_STRING
            };

            XChangeProperty(

                display,

                request.requestor,

                property,

                XA_ATOM,

                32,

                PropModeReplace,

                reinterpret_cast<unsigned char*>(supported),

                3
            );

            response.property = property;

        } else if (
            request.target == utf8_atom ||
            request.target == XA_STRING
        ) {

            XChangeProperty(

                display,

                request.requestor,

                property,

                request.target,

                8,

                PropModeReplace,

                reinterpret_cast<const unsigned char*>(
                    (request.selection == clipboard_atom ? clipboard_text : selected_text).data()
                ),

                static_cast<int>(
                    (request.selection == clipboard_atom ? clipboard_text : selected_text).size()
                )
            );

            response.property = property;
        }

        XSendEvent(

            display,

            request.requestor,

            False,

            0,

            reinterpret_cast<XEvent*>(&response)
        );

        XFlush(display);
    }

    // ========================================================================
    // HOTKEY CONFIGURATION GUI
    // ========================================================================

    std::string hotkeyDescription(const Hotkey& hotkey) {

        std::string result;

        if (hotkey.modifiers & ControlMask) {

            result += "CTRL+";
        }

        if (hotkey.modifiers & ShiftMask) {

            result += "SHIFT+";
        }

        if (hotkey.modifiers & Mod1Mask) {

            result += "ALT+";
        }

        const char* key = XKeysymToString(hotkey.key);

        result += key ? key : "UNKNOWN";

        return result;
    }

    void resetHotkeysToDefaults() {
        const Hotkey defaults[HOTKEY_COUNT] = {
            {XK_t, ControlMask | ShiftMask},
            {XK_w, ControlMask | ShiftMask},
            {XK_Tab, ControlMask},
            {XK_Tab, ControlMask | ShiftMask},
            {XK_k, ControlMask | ShiftMask},
            {XK_F1, 0},
            {XK_Left, 0}, {XK_Right, 0}, {XK_Up, 0}, {XK_Down, 0},
            {XK_a, ControlMask}, {XK_e, ControlMask},
            {XK_u, ControlMask}, {XK_k, ControlMask},
            {XK_w, ControlMask}, {XK_l, ControlMask}, {XK_d, ControlMask},
            {XK_c, ControlMask | ShiftMask},
            {XK_v, ControlMask | ShiftMask}
        };
        std::copy(std::begin(defaults), std::end(defaults), std::begin(hotkeys));
    }

    bool hotkeySetIsValid(const Hotkey* candidate) const {
        const unsigned int allowed = ControlMask | ShiftMask | Mod1Mask;
        for (int i = 0; i < HOTKEY_COUNT; ++i) {
            if (candidate[i].key == NoSymbol ||
                (candidate[i].modifiers & ~allowed) != 0) return false;
            for (int j = i + 1; j < HOTKEY_COUNT; ++j) {
                if (normalizeHotkeyKey(candidate[i].key) ==
                        normalizeHotkeyKey(candidate[j].key) &&
                    candidate[i].modifiers == candidate[j].modifiers)
                    return false;
            }
        }
        return true;
    }

    std::string hotkeyConfigPath() const {
        const char* home = std::getenv("HOME");
        return (home && *home) ? std::string(home) + "/.terminatorrc"
                               : ".terminatorrc";
    }

    bool saveHotkeys() const {
        std::ofstream out(hotkeyConfigPath());
        if (!out) return false;
        out << "# Terminator hotkey configuration v1\n";
        out << "# action-index keysym-hex modifiers-hex\n";
        for (int i = 0; i < HOTKEY_COUNT; ++i) {
            out << std::dec << i << " "
                << std::hex << static_cast<unsigned long>(hotkeys[i].key)
                << " " << hotkeys[i].modifiers << "\n";
        }
        return out.good();
    }

    bool loadHotkeys() {
        resetHotkeysToDefaults();
        std::ifstream in(hotkeyConfigPath());
        if (!in) return false;

        Hotkey loaded[HOTKEY_COUNT];
        std::copy(std::begin(hotkeys), std::end(hotkeys), std::begin(loaded));
        bool seen[HOTKEY_COUNT]{};
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            int action = -1;
            unsigned long key = 0;
            unsigned int modifiers = 0;
            if (!(row >> std::dec >> action >> std::hex >> key >> modifiers) ||
                action < 0 || action >= HOTKEY_COUNT || seen[action] ||
                key == 0 || key > 0x00ffffffUL) {
                resetHotkeysToDefaults();
                saveHotkeys();
                return false;
            }
            seen[action] = true;
            loaded[action] = {static_cast<KeySym>(key), modifiers};
        }
        if (!in.eof() || !hotkeySetIsValid(loaded)) {
            resetHotkeysToDefaults();
            saveHotkeys();
            return false;
        }
        std::copy(std::begin(loaded), std::end(loaded), std::begin(hotkeys));
        return true;
    }

    void openHotkeyConfig() {
        if (config_open) return;
        constexpr unsigned int config_width = 560, config_height = 620;
        config_window = XCreateSimpleWindow(
            display, window, width / 2 - static_cast<int>(config_width / 2),
            height / 2 - static_cast<int>(config_height / 2),
            config_width, config_height, 2, palette[CYAN], palette[BLACK]);
        XStoreName(display, config_window, "Terminator - Hotkey Configuration");
        XSelectInput(display, config_window,
                     ExposureMask | KeyPressMask | ButtonPressMask);
        XMapRaised(display, config_window);
        XSetInputFocus(display, config_window, RevertToParent, CurrentTime);
        selected_hotkey = std::clamp(selected_hotkey, 0, HOTKEY_COUNT - 1);
        capturing_hotkey = false;
        hotkey_config_message = "UP/DOWN: SELECT   ENTER: RECORD   ESC: CLOSE";
        config_open = true;
        drawHotkeyConfig();
    }

    void drawHotkeyConfig() {
        if (!config_open) return;
        constexpr int config_width = 560, config_height = 620;
        XSetForeground(display, gc, palette[BLACK]);
        XFillRectangle(display, config_window, gc, 0, 0,
                       config_width, config_height);
        drawText(config_window, 20, 27, "HOTKEY CONFIGURATION", palette[CYAN]);
        drawText(config_window, 20, 49,
                 "Select an action, press ENTER, then press its new key.",
                 palette[WHITE]);

        for (int i = 0; i < HOTKEY_COUNT; ++i) {
            const int y = 78 + i * 25;
            const unsigned long color =
                i == selected_hotkey ? palette[BRIGHT_GREEN] : palette[WHITE];
            std::string line =
                (i == selected_hotkey ? "> " : "  ") +
                std::string(HOTKEY_NAMES[i]) + " : " + hotkeyDescription(hotkeys[i]);
            if (i == selected_hotkey && capturing_hotkey)
                line += "   [PRESS A KEY...]";
            drawText(config_window, 20, y, line, color);
        }
        drawText(config_window, 20, 575, hotkey_config_message,
                 capturing_hotkey ? palette[YELLOW] : palette[CYAN]);
        drawText(config_window, 20, 600,
                 "Changes are saved automatically to ~/.terminatorrc",
                 palette[BRIGHT_BLACK]);
        XFlush(display);
    }

    void closeHotkeyConfig() {
        if (!config_open) return;
        XDestroyWindow(display, config_window);
        config_window = 0;
        config_open = false;
        capturing_hotkey = false;
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        redraw();
    }

    void configureHotkey(XKeyEvent& event) {
        KeySym key = normalizeHotkeyKey(XLookupKeysym(&event, 0));
        const unsigned int modifiers =
            event.state & (ControlMask | ShiftMask | Mod1Mask);

        if (capturing_hotkey) {
            if (key == XK_Escape) {
                capturing_hotkey = false;
                hotkey_config_message = "Recording cancelled.";
                drawHotkeyConfig();
                return;
            }
            if (key == XK_Shift_L || key == XK_Shift_R ||
                key == XK_Control_L || key == XK_Control_R ||
                key == XK_Alt_L || key == XK_Alt_R ||
                key == XK_Meta_L || key == XK_Meta_R) return;

            Hotkey candidate[HOTKEY_COUNT];
            std::copy(std::begin(hotkeys), std::end(hotkeys), std::begin(candidate));
            candidate[selected_hotkey] = {key, modifiers};

            if (!hotkeySetIsValid(candidate)) {
                hotkey_config_message =
                    "Shortcut already assigned. Choose a different combination.";
                capturing_hotkey = false;
                drawHotkeyConfig();
                return;
            }

            std::copy(std::begin(candidate), std::end(candidate), std::begin(hotkeys));
            capturing_hotkey = false;
            hotkey_config_message = saveHotkeys()
                ? "Saved to ~/.terminatorrc"
                : "Could not save ~/.terminatorrc";
            drawHotkeyConfig();
            return;
        }

        if (key == XK_Escape) { closeHotkeyConfig(); return; }
        if (key == XK_Up) {
            selected_hotkey = (selected_hotkey - 1 + HOTKEY_COUNT) % HOTKEY_COUNT;
            drawHotkeyConfig();
            return;
        }
        if (key == XK_Down) {
            selected_hotkey = (selected_hotkey + 1) % HOTKEY_COUNT;
            drawHotkeyConfig();
            return;
        }
        if (key == XK_Return || key == XK_KP_Enter) {
            capturing_hotkey = true;
            hotkey_config_message = "Recording: press the desired key combination...";
            drawHotkeyConfig();
        }
    }

    // HOTKEY PROCESSING
    // ========================================================================

    bool matchesHotkey(

        int action,

        KeySym key,

        unsigned int modifiers
    ) {

        return (

            hotkeys[action].key == key &&

            hotkeys[action].modifiers ==
                (modifiers & (
                    ControlMask |
                    ShiftMask |
                    Mod1Mask
                ))
        );
    }

    bool processHotkey(

        KeySym key,

        unsigned int modifiers
    ) {

        if (matchesHotkey(
            HOTKEY_NEW_TAB,
            key,
            modifiers
        )) {

            createTab();

            return true;
        }

        if (matchesHotkey(
            HOTKEY_CLOSE_TAB,
            key,
            modifiers
        )) {

            closeTab(active_tab);

            return true;
        }

        if (matchesHotkey(
            HOTKEY_NEXT_TAB,
            key,
            modifiers
        )) {

            nextTab();

            return true;
        }

        if (matchesHotkey(
            HOTKEY_PREVIOUS_TAB,
            key,
            modifiers
        )) {

            previousTab();

            return true;
        }

        if (matchesHotkey(
            HOTKEY_CONFIG,
            key,
            modifiers
        )) {

            openHotkeyConfig();

            return true;
        }

        if (matchesHotkey(
            HOTKEY_MATRIX,
            key,
            modifiers
        )) {

            matrix_mode = !matrix_mode;

            redraw();

            return true;
        }

        return false;
    }

    // ========================================================================
    bool processEditingHotkey(KeySym key, unsigned int modifiers,
                              TerminalSession* session) {
        if (!session) return false;
        struct Binding { int action; const char* bytes; size_t length; };
        static const Binding bindings[] = {
            {HOTKEY_CURSOR_LEFT, "\033[D", 3},
            {HOTKEY_CURSOR_RIGHT, "\033[C", 3},
            {HOTKEY_HISTORY_PREVIOUS, "\033[A", 3},
            {HOTKEY_HISTORY_NEXT, "\033[B", 3},
            {HOTKEY_LINE_START, "\001", 1},
            {HOTKEY_LINE_END, "\005", 1},
            {HOTKEY_DELETE_TO_START, "\025", 1},
            {HOTKEY_DELETE_TO_END, "\013", 1},
            {HOTKEY_DELETE_PREVIOUS_WORD, "\027", 1},
            {HOTKEY_CLEAR_SCREEN, "\014", 1},
            {HOTKEY_EOF, "\004", 1}
        };
        for (const Binding& binding : bindings) {
            if (matchesHotkey(binding.action, key, modifiers)) {
                session->sendInput(binding.bytes, binding.length);
                redraw();
                return true;
            }
        }
        if (matchesHotkey(HOTKEY_COPY, key, modifiers)) {
            copySelection();
            return true;
        }
        if (matchesHotkey(HOTKEY_PASTE, key, modifiers)) {
            requestPaste(clipboard_atom);
            return true;
        }
        return false;
    }

    // KEYBOARD INPUT
    // ========================================================================

    void handleKeyPress(XKeyEvent& event) {

        if (config_open) {
            configureHotkey(event);
            return;
        }

        KeySym keysym = normalizeHotkeyKey(
            XLookupKeysym(&event, 0)
        );

        unsigned int modifiers = event.state;
        const bool shift = (modifiers & ShiftMask) != 0;

        TerminalSession* session = currentSession();

        if (!session) return;

        // Configurable clipboard shortcuts are handled by processEditingHotkey.
        // Shift+Insert remains a conventional PRIMARY-selection paste shortcut.

        // PRIMARY selection paste
        // --------------------------------------------------------

        if (shift && keysym == XK_Insert) {
            requestPaste(primary_atom);
            return;
        }

        // --------------------------------------------------------
        // Application-level configurable hotkeys
        // --------------------------------------------------------

        if (processHotkey(keysym, modifiers)) {
            return;
        }

        // --------------------------------------------------------
        // Configurable Bash editing shortcuts.
        if (processEditingHotkey(keysym, modifiers, session)) {
            return;
        }

        // Bash Readline implements programmable/path/command completion.
        // Forward a literal TAB byte so the interactive shell can perform its
        // normal completion (a second TAB lists ambiguous candidates).
        if (keysym == XK_Tab && !(modifiers & ControlMask)) {
            session->sendInput("\t", 1);
            redraw();
            return;
        }

        // Arrow keys and navigation
        // --------------------------------------------------------

        switch (keysym) {

            case XK_Home:
                session->sendInput("\033[H", 3);
                redraw();
                return;

            case XK_End:
                session->sendInput("\033[F", 3);
                redraw();
                return;

            case XK_Delete:
                session->sendInput("\033[3~", 4);
                redraw();
                return;

            case XK_BackSpace:
                session->sendInput("\177", 1);
                redraw();
                return;

            case XK_Page_Up:
                session->scrollBy(session->rows - 1);
                redraw();
                return;

            case XK_Page_Down:
                session->scrollBy(-(session->rows - 1));
                redraw();
                return;

            default:
                break;
        }

        // --------------------------------------------------------
        // Normal keyboard input
        // --------------------------------------------------------

        char keybuf[64] = {};

        KeySym translated = NoSymbol;

        int len = XLookupString(
            &event,
            keybuf,
            sizeof(keybuf),
            &translated,
            nullptr
        );

        if (len > 0) {

            session->sendInput(
                keybuf,
                static_cast<size_t>(len)
            );
        }

        redraw();
    }
        
    // ========================================================================
    // MOUSE INPUT
    // ========================================================================

    void handleMousePress(XButtonEvent& event) {
        
        TerminalSession* session = currentSession();

        if (!session) return;

        // Mouse wheel up.
        if (event.button == Button4) {
            session->scrollBy(3);
            redraw();
            return;
        }

        // Mouse wheel down.
        if (event.button == Button5) {
            session->scrollBy(-3);
            redraw();
            return;
        }
        // Tab bar.

        if (event.y >= 0 && event.y < TAB_HEIGHT) {

            int index = (event.x - 5) / TAB_WIDTH;

            if (index >= 0 &&
                index < static_cast<int>(tabs.size())) {

                int local_x = (event.x - 5) % TAB_WIDTH;

                // Close button.

                if (local_x >= TAB_WIDTH - 29) {

                    closeTab(index);

                } else {

                    active_tab = index;

                    selecting = false;

                    selection_valid = false;

                    selected_text.clear();

                    redraw();
                }

                return;
            }

            // New tab button.

            if (index >= static_cast<int>(tabs.size())) {

                createTab();

                return;
            }
        }

        // Middle mouse paste.

        if (event.button == Button2) {

            requestPaste(primary_atom);

            return;
        }

        // Left click starts selection.

        if (event.button == Button1) {

            TerminalSession* session = currentSession();

            if (!session) return;

            selecting = true;

            selection_valid = true;

            selection_start_x = std::clamp(

                (event.x - PADDING_X) / char_width,

                0,

                session->cols - 1
            );

            selection_start_y = std::clamp(

                (event.y - TAB_HEIGHT - PADDING_Y) / line_height,

                0,

                session->rows - 1
            );

            selection_end_x = selection_start_x;

            selection_end_y = selection_start_y;

            selected_text.clear();

            redraw();
        }
    }

    // ------------------------------------------------------------------------
    // Mouse movement
    // ------------------------------------------------------------------------

    void handleMouseMotion(XMotionEvent& event) {

        if (!selecting) return;

        TerminalSession* session = currentSession();

        if (!session) return;

        selection_end_x = std::clamp(

            (event.x - PADDING_X) / char_width,

            0,

            session->cols - 1
        );

        selection_end_y = std::clamp(

            (event.y - TAB_HEIGHT - PADDING_Y) / line_height,

            0,

            session->rows - 1
        );

        redraw();
    }

    // ------------------------------------------------------------------------
    // Mouse release
    // ------------------------------------------------------------------------

    void handleMouseRelease(XButtonEvent& event) {

        if (event.button != Button1) return;

        if (!selecting) return;

        selecting = false;

        updateSelectedText();

        copySelection();

        redraw();
    }

    // ========================================================================
    // PTY INPUT
    // ========================================================================

    void handlePTY(TerminalSession& session) {

        char data[8192];

        while (true) {

            ssize_t n = read(

                session.master,

                data,

                sizeof(data)
            );

            if (n > 0) {

                session.feed(data, n);

            } else if (n == 0) {

                session.alive = false;

                break;

            } else {

                if (errno == EINTR) {

                    continue;
                }

                if (errno == EAGAIN ||
                    errno == EWOULDBLOCK) {

                    break;
                }

                session.alive = false;

                break;
            }
        }
    }

    // ========================================================================
    // EVENT HANDLING
    // ========================================================================

    void handleEvent(XEvent& event) {

        if (event.type == Expose) {

            if (event.xexpose.count == 0) {

                redraw();
            }

        } else if (event.type == KeyPress) {

            handleKeyPress(event.xkey);

        } else if (event.type == ButtonPress) {

            if (config_open &&
                event.xbutton.window == config_window) {

                if (event.xbutton.button == Button1) {

                    int index = (event.xbutton.y - 66) / 25;

                    if (event.xbutton.y >= 66 &&
                        index >= 0 && index < HOTKEY_COUNT) {
                        selected_hotkey = index;
                        capturing_hotkey = false;
                        drawHotkeyConfig();
                    }
                }

                return;
            }

            handleMousePress(event.xbutton);

        } else if (event.type == MotionNotify) {

            if (event.xmotion.window == window) {

                handleMouseMotion(event.xmotion);
            }

        } else if (event.type == ButtonRelease) {

            if (event.xbutton.window == window) {

                handleMouseRelease(event.xbutton);
            }

        } else if (event.type == SelectionNotify) {

            handleSelectionNotify(event.xselection);

        } else if (event.type == SelectionRequest) {

            handleSelectionRequest(event.xselectionrequest);

        } else if (event.type == ConfigureNotify) {

            if (event.xconfigure.window == window) {

                int new_width = event.xconfigure.width;

                int new_height = event.xconfigure.height;

                if (new_width != width ||
                    new_height != height) {

                    width = new_width;

                    height = new_height;

                    resizeAllTabs();

                    redraw();
                }
            }

        } else if (event.type == ClientMessage) {

            if (static_cast<Atom>(
                event.xclient.data.l[0]
            ) == wm_delete) {

                running = false;
            }
        }
    }

    // ========================================================================
    // MULTIWATCH
    // ========================================================================

    void runMultiWatch() {

        int xfd = ConnectionNumber(display);

        while (running) {

            std::vector<pollfd> descriptors;

            // X11 descriptor.

            descriptors.push_back({

                xfd,

                POLLIN,

                0
            });

            // All terminal descriptors.

            for (auto& tab : tabs) {

                if (tab->master >= 0 && tab->alive) {

                    descriptors.push_back({

                        tab->master,

                        static_cast<short>(
                            POLLIN | POLLHUP | POLLERR
                        ),

                        0
                    });
                }
            }

            int timeout = matrix_mode
                ? POLL_TIMEOUT
                : -1;

            int result = poll(

                descriptors.data(),

                descriptors.size(),

                timeout
            );

            if (result < 0) {

                if (errno == EINTR) {

                    continue;
                }

                perror("poll");

                break;
            }

            bool changed = false;

            // X11 events.

            if (descriptors[0].revents & POLLIN) {

                while (XPending(display) > 0) {

                    XEvent event;

                    XNextEvent(display, &event);

                    handleEvent(event);

                    changed = true;
                }
            }

            // PTY events.

            for (size_t i = 1;
                 i < descriptors.size();
                 ++i) {

                if (!(descriptors[i].revents &
                    (POLLIN | POLLHUP | POLLERR))) {

                    continue;
                }

                int fd = descriptors[i].fd;

                for (auto& tab : tabs) {

                    if (tab->master == fd) {

                        handlePTY(*tab);

                        changed = true;

                        break;
                    }
                }
            }

            // Reap completed shell processes. The shell's normal `exit`
            // builtin (and Ctrl+D) closes the PTY; reflect that by closing the
            // corresponding GUI tab, just like the Close Tab hotkey.
            for (auto& tab : tabs) {

                if (tab->worker_pid > 0) {
                    int status = 0;
                    pid_t child_result = waitpid(tab->worker_pid, &status, WNOHANG);
                    if (child_result == tab->worker_pid) {
                        tab->worker_pid = -1;
                        tab->alive = false;
                        if (tab->master >= 0) { close(tab->master); tab->master = -1; }
                        changed = true;
                    }
                }
            }

            // Erase dead tabs from the back so indices remain valid. This is
            // also needed in thread-worker mode, where PTY EOF marks alive=false
            // without a worker PID to reap.
            for (int i = static_cast<int>(tabs.size()) - 1; i >= 0; --i) {
                if (!tabs[static_cast<size_t>(i)]->alive) {
                    closeTab(i);
                    changed = true;
                }
            }

            if (matrix_mode) {

                changed = true;
            }

            if (changed) {

                redraw();
            }
        }
    }

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    bool initialize() {

        (void)initializeSharedHistory();

        display = XOpenDisplay(nullptr);

        if (!display) {

            fprintf(
                stderr,
                "ERROR: Cannot open X11 display.\n"
            );

            return false;
        }

        screen = DefaultScreen(display);

        initializeColors();

        font = XLoadQueryFont(

            display,

            "-misc-fixed-medium-r-normal--18-120-100-100-c-90-iso10646-1"
        );

        if (!font) {

            font = XLoadQueryFont(
                display,
                "fixed"
            );
        }

        if (!font) {

            fprintf(
                stderr,
                "ERROR: Cannot load X11 font.\n"
            );

            return false;
        }

        char_width = font->max_bounds.width;

        if (char_width <= 0) {

            char_width = 9;
        }

        line_height = font->ascent + font->descent;

        // Main window.

        window = XCreateSimpleWindow(

            display,

            RootWindow(display, screen),

            50,

            50,

            width,

            height,

            2,

            palette[CYAN],

            default_bg
        );

        XStoreName(

            display,

            window,

            "Terminator"
        );

        XSelectInput(

            display,

            window,

            ExposureMask |
            KeyPressMask |
            StructureNotifyMask |
            ButtonPressMask |
            ButtonReleaseMask |
            PointerMotionMask
        );

        // Window close protocol.

        wm_delete = XInternAtom(

            display,

            "WM_DELETE_WINDOW",

            False
        );

        XSetWMProtocols(

            display,

            window,

            &wm_delete,

            1
        );

        // Clipboard atoms.

        clipboard_atom = XInternAtom(

            display,

            "CLIPBOARD",

            False
        );

        primary_atom = XA_PRIMARY;

        utf8_atom = XInternAtom(

            display,

            "UTF8_STRING",

            False
        );

        targets_atom = XInternAtom(

            display,

            "TARGETS",

            False
        );

        paste_property = XInternAtom(

            display,

            "TERMINATOR_CLIPBOARD",

            False
        );

        // Graphics.

        gc = XCreateGC(

            display,

            window,

            0,

            nullptr
        );

        XSetFont(

            display,

            gc,

            font->fid
        );

        XMapWindow(display, window);

        XSync(display, False);

        calculateDimensions();

        buffer = XCreatePixmap(

            display,

            window,

            width,

            height,

            DefaultDepth(display, screen)
        );

        initializeMatrix();

        return createTab();
    }

public:

    explicit X11Terminal(bool process_mode = false) : use_process_mode(process_mode) {
        resetHotkeysToDefaults();
        loadHotkeys();
    }

    ~X11Terminal() {

        for (auto& tab : tabs) {

            tab->closeSession();
        }

        if (config_window) {

            XDestroyWindow(display, config_window);
        }

        if (buffer) {

            XFreePixmap(display, buffer);
        }

        if (gc) {

            XFreeGC(display, gc);
        }

        if (font) {

            XFreeFont(display, font);
        }

        if (window) {

            XDestroyWindow(display, window);
        }

        if (display) {

            XCloseDisplay(display);
        }

        closeSharedHistory();
    }

    void run() {

        if (!initialize()) {

            return;
        }

        redraw();

        runMultiWatch();
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv) {

    // A tab may be closed while its worker is writing to the output pipe.
    // Convert EPIPE into a normal worker shutdown instead of terminating the GUI.
    signal(SIGPIPE, SIG_IGN);

    bool use_process_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "useProcess=True") == 0 ||
            std::strcmp(argv[i], "--process") == 0 ||
            std::strcmp(argv[i], "--use-process") == 0) {
            use_process_mode = true;
        } else if (std::strcmp(argv[i], "useProcess=False") == 0 ||
                   std::strcmp(argv[i], "--thread") == 0) {
            use_process_mode = false;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            std::fprintf(stderr, "Usage: %s [useProcess=True|useProcess=False|--process|--thread]\n", argv[0]);
            return 2;
        }
    }

    X11Terminal terminal(use_process_mode);
    terminal.run();

    return 0;
}