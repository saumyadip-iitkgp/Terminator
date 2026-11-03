# Terminator — Software Design Document

**Implementation basis:** `terminator(20261103-095710).cpp`  
**Platform:** Linux, X11/Xlib, POSIX processes, threads, PTYs and pipes  
**Language:** C++17

## 1. Purpose and architectural overview

Terminator is a graphical terminal front end. It does not implement a second command-language parser. The GUI manages tabs, terminal screen state, keyboard and mouse input, rendering, and the transport of bytes. Each tab has a worker that owns a pseudo-terminal (PTY) and launches an interactive Bash shell attached to that PTY.

The architecture separates into three cooperating layers:

1. **X11 GUI:** owns the display connection, window, tab UI, input handling, terminal screen buffers and rendering.
2. **Tab worker:** bridges GUI-side pipes to a PTY. It multiplexes PTY output, user input and resize messages.
3. **Interactive Bash session:** interprets commands and implements shell features such as pipelines, redirection, command execution, Readline completion, history and job control.

The worker has no Xlib calls. GUI-to-worker communication uses three pipes per tab, regardless of whether the worker is hosted in a thread or a separate process.

```text
                         Terminator GUI process
  +----------------------------------------------------------------+
  | X11 window, tab bar, keyboard/mouse handling, renderer         |
  |                                                                |
  | X11 event fd ----+                                              |
  |                  +--> poll() event loop                         |
  | Tab A output <-- pipe <-- worker A                              |
  | Tab A input  --> pipe --> worker A                              |
  | Tab A resize --> pipe --> worker A                              |
  |                                                                |
  | Tab B output <-- pipe <-- worker B                              |
  | Tab B input  --> pipe --> worker B                              |
  | Tab B resize --> pipe --> worker B                              |
  +----------------------------------------------------------------+
                              | worker owns PTY master
                              v
                     +--------------------+
                     | PTY master/slave   |
                     +--------------------+
                              |
                              v
                     +--------------------+
                     | Interactive Bash  |
                     | commands/jobs/etc.|
                     +--------------------+
```

Each tab is an independent shell session. It therefore has its own current directory, shell variables, foreground process group and Bash job table.

## 2. X11 GUI and event loop

### 2.1 X11 ownership

The `X11Terminal` class owns the X11 display and the top-level window. Initialization opens the display with `XOpenDisplay`, creates the window, selects the required X events and sets the window-manager protocols. Xlib calls remain on the GUI side; the worker code does not draw or process X events.

The GUI is responsible for:

- Creating, selecting and closing tabs.
- Handling keyboard and mouse events.
- Managing configurable hotkeys and clipboard/selection interaction.
- Maintaining and rendering the terminal grid and scrollback for each tab.
- Translating terminal resize events into worker resize messages.
- Updating the display when terminal output or GUI state changes.

### 2.2 Unified `poll()` loop

The main GUI loop builds a descriptor list containing the X connection descriptor (`ConnectionNumber(display)`) and the output pipe for every live tab. It then calls `poll()`.

When the X descriptor is readable, the GUI drains queued X events using `XPending()` and `XNextEvent()` and dispatches them to the event handlers. When a tab output descriptor is readable, hung up or in error, the corresponding PTY-output handler reads and processes the available bytes.

This design allows the GUI to wait for either user-interface activity or output from any tab without a separate blocking read loop per tab. Output from inactive tabs is still collected; switching tabs changes which session is rendered, not which sessions are monitored.

The normal `poll()` timeout is indefinite. Matrix mode uses a finite timeout so animation can advance even when no file descriptor becomes readable. An interrupted `poll()` (`EINTR`) causes the loop to retry.

After handling ready descriptors, the loop checks worker-process exit status with nonblocking `waitpid(..., WNOHANG)`, marks dead sessions, closes their tabs and redraws when state has changed.

### 2.3 Terminal screen model

`TerminalSession` holds the terminal state for one tab. Its screen is a two-dimensional `std::vector<std::vector<TerminalCell>>`; scrollback is a `std::deque<std::vector<TerminalCell>>`.

A `TerminalCell` stores a character and rendering attributes: foreground and background colors, bold, underline and reverse-video flags. The session also stores cursor coordinates, saved cursor coordinates, current colors, title, and ANSI parser state/buffer.

The parser tracks ground, escape, CSI and OSC states. Incoming bytes from the worker are fed into this parser, which updates the in-memory screen model. The renderer then paints the visible state through Xlib. This is a practical ANSI subset, not a complete xterm protocol implementation.

## 3. Thread mode and process mode

### 3.1 Common worker contract

`TerminalSession::startShell()` creates three unidirectional pipes for each tab:

| Channel | GUI end | Worker end | Purpose |
|---|---|---|---|
| Input | write | read | Keyboard bytes and terminal control sequences |
| Output | read | write | Bytes read from the PTY master |
| Resize | write | read | `TabResize { cols, rows }` messages |

The pipe descriptors are marked close-on-exec. The GUI's output-pipe read end is made nonblocking. The worker owns the PTY master; the GUI never reads or writes the PTY directly.

Both execution modes call the same `runTabWorker()` function. That function uses `forkpty()` to create a PTY and launch Bash as a child attached to its slave side. It then polls three descriptors: PTY master, GUI input pipe and GUI resize pipe.

### 3.2 Thread mode (default)

In thread mode, `startShell()` launches `runTabWorker()` using `std::thread`. The worker runs in the GUI process, so it shares the process address space and file-descriptor table with the GUI. It nevertheless communicates with the GUI through the same three pipes used in process mode.

The thread owns its worker-side pipe ends and the PTY master. The GUI thread owns the GUI-side ends. Because threads share a descriptor table, worker-side descriptors are closed by the worker when it exits rather than being closed by the GUI immediately after thread creation.

**Advantages:** lower worker-startup overhead and no extra worker process to reap.

**Trade-off:** worker code shares the GUI process's address space. A worker defect can affect the whole application process. The pipe boundary limits direct interaction with GUI state, but does not provide process-level fault isolation.

### 3.3 Process mode

In process mode, `startShell()` calls `fork()` to create a worker process. The child closes the GUI-side pipe ends, closes unrelated descriptors, and enters `runTabWorker()`. The parent stores the worker PID and closes the worker-side pipe ends.

The worker process then calls `forkpty()` to create the Bash shell child. Thus process mode has a worker process between the GUI and the shell:

```text
GUI parent process
    |
    +-- worker process (one per tab)
            |
            +-- Bash shell child (created by forkpty)
```

The parent tracks the worker PID and reaps it with `waitpid(..., WNOHANG)`. This mode isolates the PTY bridge worker from the GUI address space and makes worker lifetime explicit.

**Advantages:** stronger fault and address-space isolation for the worker.

**Trade-off:** additional process creation, IPC and child-reaping work.

### 3.4 Shared state and synchronization

The two modes deliberately use the same pipe protocol. No GUI terminal-grid object, tab object or C++ container is shared as a live cross-thread/cross-process data structure with the worker. This avoids needing a large set of locks around the GUI model.

Synchronization is handled at the boundaries:

- **Input, output and resize:** pipes provide ordered byte/message streams. The worker and GUI block or poll on their respective pipe ends.
- **Command-history ring:** POSIX shared memory (`shm_open` + `mmap(MAP_SHARED)`) exposes a fixed shared structure. A process-shared POSIX semaphore (`sem_init` with `pshared = 1`) protects updates to the ring index and slot writes.
- **Bash history file:** tabs use a common per-user history file. `PROMPT_COMMAND` enables `histappend` and runs `history -a` and `history -n` while holding an exclusive `flock` on a lock file. The lock serializes append/read operations across tabs.
- **GUI screen state:** updated and rendered by the GUI event loop. Worker threads/processes send bytes, not pointers or mutations to the GUI's C++ containers.

The shared-memory object is intentionally left available when an instance exits so concurrently running Terminator instances can continue using it. Its name includes the user ID.

## 4. PTY, process and job management

### 4.1 PTY creation and shell startup

`runTabWorker()` initializes a `winsize` structure from the tab's current dimensions and calls `forkpty()`. This creates a PTY master/slave pair and gives the shell child a controlling terminal. In the child, the code sets terminal-related environment variables and launches interactive Bash with a private rc file:

- `TERM=xterm-256color`
- `COLORTERM=truecolor`
- A per-user Bash history file and history configuration.
- A private rc file under `/tmp` that sources the user's `~/.bashrc`, then installs Terminator-specific shell helpers and Readline completion settings.

Bash is executed with `execlp("bash", "bash", "--rcfile", rc_path, "-i", ...)`. The application does not parse or execute shell command strings itself.

### 4.2 Worker supervision

The worker polls its PTY master and the two GUI-to-worker pipes. Input bytes are copied to the PTY master. PTY output is copied to the GUI output pipe. A closed input pipe, PTY EOF, unrecoverable I/O error or completed shell causes the worker loop to terminate.

The worker checks the Bash PID with `waitpid(..., WNOHANG)`. During shutdown it sends `SIGHUP` to the shell process group, then escalates to `SIGTERM` if the shell has not exited, and waits for the child. The PTY and worker pipe descriptors are then closed and the private rc file is removed.

The GUI separately supervises process-mode worker PIDs. On tab closure, the session closes its pipe descriptors, sends `SIGTERM` to a worker process if present, and waits for it; the implementation escalates to `SIGKILL` if the worker has not exited at the first nonblocking check. Thread mode joins the worker thread.

### 4.3 Shell job control

Job control is implemented by Bash and the operating system, not by a Terminator-owned job table. Since each tab has its own interactive Bash and controlling PTY, Bash maintains a separate job table and foreground process group for each tab.

Commands such as `jobs`, `fg`, `bg`, and shell backgrounding with `&` are interpreted by Bash. The terminal forwards the relevant control bytes to the PTY; the PTY's terminal line discipline and foreground process group provide normal terminal job-control behavior.

## 5. IPC, pipes, `dup2()` and redirection

### 5.1 Terminator's IPC pipes

The three pipes created per tab are application IPC channels, not shell pipeline pipes:

- **Input pipe:** GUI writes terminal input; worker reads it and writes it to the PTY master.
- **Output pipe:** worker reads from the PTY master and writes terminal output; GUI reads it and updates the screen.
- **Resize pipe:** GUI writes a fixed-size `TabResize` structure; worker reads it and changes the PTY window size.

`writeAll()` handles partial writes and retries when interrupted by `EINTR`. The PTY master is nonblocking. The worker uses `poll()` to multiplex its three descriptors rather than blocking indefinitely on one direction.

### 5.2 Shell pipelines and redirection

A key architectural decision is that Bash performs shell parsing and command execution. Terminator does **not** implement its own pipeline parser, command AST, redirection engine or per-command `dup2()` setup.

When a user enters a command such as:

```bash
cat input.txt | grep error > errors.txt
```

the input line reaches Bash through the PTY. Bash parses the pipeline and redirection syntax, creates the required shell pipeline file descriptors and child processes, and performs the descriptor wiring (using the normal POSIX mechanisms, including `pipe()` and `dup2()` as appropriate) before executing the commands.

Consequently, `<`, `>`, `>>`, pipelines, quoting, expansions, command substitution and compound shell syntax follow Bash semantics. The application provides the terminal interface and PTY transport; it does not claim to own the shell's internal `dup2()` calls.

## 6. Command history and auto-completion

### 6.1 Shared-memory command ring

The C++ side defines:

```cpp
static constexpr size_t HISTORY_CAPACITY = 10000;
static constexpr size_t HISTORY_LINE_SIZE = 4096;

struct SharedHistory {
    sem_t lock;
    uint64_t next;
    char lines[HISTORY_CAPACITY][HISTORY_LINE_SIZE];
};
```

The shared object is created/opened with `shm_open()`, sized with `ftruncate()` when first created, and mapped with `mmap(..., MAP_SHARED, ...)`. The first creator initializes the structure and a process-shared semaphore.

When input is sent to a tab, `TerminalSession::sendInput()` maintains a `pending_command` string. It adds ordinary printable bytes, removes a trailing byte for Backspace/Delete, and when it sees carriage return or newline it appends a nonempty line to the shared ring. The slot is selected with `next++ % HISTORY_CAPACITY`, giving the ring a fixed maximum of 10,000 entries. Each stored line is limited to 4,095 characters plus the terminating NUL byte.

The semaphore serializes the index increment and slot write. The ring is an auxiliary shared command log; it is not the data source used to render Bash's interactive `history` builtin.

### 6.2 Bash history file

The actual interactive Bash history is configured separately. Tabs set `HISTFILE` to `/tmp/terminator_history_<uid>`, configure `HISTSIZE`, `HISTFILESIZE`, duplicate handling and timestamps, and install a `PROMPT_COMMAND`.

At each prompt, that command acquires an exclusive `flock`, runs `history -a` to append new entries and `history -n` to import entries added by other tabs, then releases the lock. This is the mechanism that makes Bash history visible across tabs while respecting Bash's history implementation.

### 6.3 Auto-completion

Terminator does not maintain a custom completion trie, candidate vector or directory-scanning completion engine. When the user presses Tab, the X11 keyboard handler sends a literal tab byte (`\t`) to the active session. Bash Readline performs command, path and programmable completion.

The private Bash rc setup enables programmable completion, binds Ctrl+I to `complete`, enables case-insensitive completion and installs default filename/bash completion behavior. Ambiguous matches are displayed by Readline when Tab is pressed again. A completion function is also registered for the `terminator-bash` helper so script names can be completed.

This keeps completion behavior aligned with Bash and the user's shell configuration rather than duplicating shell parsing in the GUI.

## 7. Signal handling and terminal control

### 7.1 Ctrl+C and Ctrl+Z

The application does not install custom handlers that translate Ctrl+C or Ctrl+Z into signals for individual commands. Instead, those key combinations are sent as terminal control bytes through the input pipe and PTY master.

The PTY slave is the shell's controlling terminal. Its terminal line discipline recognizes the configured interrupt and suspend characters and delivers `SIGINT` or `SIGTSTP` to the foreground process group. Bash then updates its job table and displays the normal job-control status. This is why foreground commands can be interrupted or suspended and later resumed with `fg` or `bg`.

### 7.2 Resize signal

When the GUI resizes a tab, it sends a `TabResize` message to the worker. The worker applies the new dimensions to the PTY using `ioctl(TIOCSWINSZ)` and sends `SIGWINCH` to the shell process group. Bash and foreground programs can then respond to the changed terminal size.

### 7.3 Shutdown and broken pipes

The worker sends `SIGHUP` to the shell process group during shutdown and may follow with `SIGTERM` if needed. The GUI ignores `SIGPIPE` globally so a worker writing to an output pipe whose GUI reader has closed does not terminate the entire GUI process; the resulting write failure is handled as a worker shutdown condition.

`poll()` interrupted by a signal is retried when `errno == EINTR`. Child processes are reaped with `waitpid()` to avoid leaving worker or shell children as zombies.

## 8. Other design decisions

### 8.1 Shell-native semantics

Delegating command interpretation to Bash avoids implementing a partial shell that would need to reproduce quoting, expansions, pipelines, redirection, job control, builtins and scripting semantics. Terminator's responsibility is terminal I/O and display.

### 8.2 Private Bash startup file

Each shell receives a private rc file in `/tmp`. The file sources the user's `~/.bashrc` and then applies Terminator's completion and helper setup. This avoids editing the user's persistent Bash configuration. The worker removes the temporary rc file on normal shutdown.

### 8.3 `terminator-bash` helper and `multiWatch`

The startup rc defines `terminator-bash`, which stores `.tbash` files in `~/.terminator/scripts` by default (overridable with `TERMINATOR_SCRIPT_DIR`). It can create, list and execute scripts. Script execution is delegated to Bash (`bash -- <script> <args...>`); `.tbash` is a naming convention, not a separate restricted interpreter.

The `multiWatch` Bash function starts as loop for each supplied command. Each loop captures the command output and exit status, prints a timestamp and separators, then sleeps for approximately two seconds before repeating. The function installs an INT trap that kills and waits for its loop processes.

### 8.4 File-descriptor hygiene

Pipe descriptors are marked `FD_CLOEXEC`. In process mode, the forked worker closes the GUI-side pipe ends and unrelated descriptors before starting the worker loop. This reduces accidental descriptor inheritance and ensures pipe EOF is observable when the intended writer closes its end.

### 8.5 Error and lifecycle handling

The implementation checks pipe creation, `fork()`, `forkpty()`, `poll()`, `read()` and `write()` outcomes. It handles partial writes and `EINTR`, uses nonblocking reads where appropriate, detects PTY EOF/hangup, and removes dead tabs. This keeps normal shell exit and tab closure integrated with the GUI lifecycle.

## 9. Important boundaries and limitations

The following distinctions are important when describing the implementation:

- **Pipelines and redirection:** supported through Bash. Terminator itself is not the component parsing `|`, `<`, `>`, or calling `dup2()` for each command.
- **Job management:** Bash owns job tables and foreground/background process groups. Terminator owns tab/worker lifecycle, not a parallel job-control subsystem.
- **Auto-completion:** Bash Readline owns interactive completion. Terminator forwards Tab.
- **History:** there are two mechanisms: a semaphore-protected shared-memory ring populated from submitted input, and Bash's shared history file synchronized with `flock`. The ring is not the interactive `history` builtin's backing store.
- **Signal handling:** the application explicitly ignores `SIGPIPE`; interactive Ctrl+C/Ctrl+Z behavior is provided by the PTY and shell's normal job-control machinery. Resize uses `SIGWINCH`.
- **Terminal compatibility:** the screen parser implements a practical ANSI subset and should not be described as a complete xterm emulator.
- **Worker modes:** both modes use the same pipe-based protocol and PTY-backed Bash worker; process mode changes the worker's hosting boundary, not shell semantics.

## 10. Key implementation types and APIs

| Type / API | Responsibility |
|---|---|
| `TerminalCell` | Character plus foreground/background and text attributes |
| `TerminalSession` | Per-tab terminal model, pipes, worker PID/thread, input tracking and lifecycle |
| `SharedHistory` | Fixed-size shared-memory command ring and process-shared semaphore |
| `TabResize` | Fixed-size terminal dimension message sent over resize pipe |
| `X11Terminal` | Xlib window, tab management, event handling, rendering and main event loop |
| `runTabWorker()` | PTY owner; bridges three IPC pipes and the shell |
| `forkpty()` | Creates PTY and launches the interactive shell child |
| `poll()` | Multiplexes X events/PTY output in GUI and PTY/input/resize in worker |
| `shm_open()` / `mmap()` | Creates/maps shared command-history memory |
| `sem_wait()` / `sem_post()` | Protects shared ring updates |
| `flock` | Serializes Bash history-file synchronization |
| `ioctl(TIOCSWINSZ)` / `SIGWINCH` | Propagates terminal resize |
| `waitpid()` | Reaps shell and process-mode worker children |

## 11. End-to-end data flow

1. The user presses a key or clicks in the X11 window.
2. The GUI event handler interprets application hotkeys; ordinary terminal input is written to the active tab's input pipe.
3. The tab worker reads the input pipe and writes the bytes to the PTY master.
4. Bash reads from the PTY slave, performs line editing, completion, history and command interpretation, and launches commands as needed.
5. Bash output is written to the PTY slave and becomes readable on the PTY master.
6. The worker reads PTY output and writes it to the tab's output pipe.
7. The GUI's `poll()` loop notices output readiness, reads the bytes, updates the ANSI parser and terminal grid, then redraws the window.
8. If the user resizes the window, the GUI sends a resize message; the worker updates the PTY dimensions and signals the shell process group.

This pipeline keeps the GUI responsive while allowing each tab's independent shell to run interactively and concurrently.
