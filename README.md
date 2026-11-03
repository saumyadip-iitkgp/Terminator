# Terminator — The X11 Terminal

Terminator is a lightweight, terminal emulator written in C++17 for
Linux/X11. Each tab runs an interactive terminall session.
Bash commands, auto-completion , pipelines, redirections,
signals, and job-control.are supported

## Features

- Multiple independent Bash sessions in one window (up to 32 tabs).
- Clickable tabs, per-tab close buttons, and a new-tab control.
- X11 `CLIPBOARD` and `PRIMARY` selection integration.
- Mouse text selection, `Ctrl+Shift+C` / `Ctrl+Shift+V`, middle-click paste,
  and `Shift+Insert` paste.
- Bash  command, path, and programmable completion.
- Shell-native pipelines (`|`) and input/output redirection (`<`, `>`, `>>`).
- Interactive foreground jobs and Bash job control (`Ctrl+C`, `Ctrl+Z`,
  `fg`, and `bg`).
- Configurable keyboard shortcuts through a GUI; settings are saved in
  `~/.terminatorrc`.
- Built-in `terminator-bash` runner for reusable `.tbash` scripts.
- `multiWatch` helper for running multiple commands.
- ANSI coloring, autoscroll 
- PTY uses `poll()` and either thread-based and process-based tab workers.

## Requirements

A Linux desktop session with an X11 display is required. You will need:

- A C++17 compiler such as `g++`.
- X11 development headers and libraries.
- PTY support (`libutil` on systems where it is provided separately).
- POSIX threads support as required by the system.
- Bash. `flock` is used for synchronizing shared Bash history between tabs.

On Debian/Ubuntu, the usual build dependencies can be installed with:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

If your distribution packages `libutil` separately, install its development
package as well. Package names vary by distribution.

## Build

Save the source as `terminator.cpp`, then compile:

```bash
g++ -std=c++17 -O2 -Wall -Wextra terminator.cpp -o terminator -lX11 -lutil
```

```from makefile
make
```

## Launch

Start in the default thread-worker mode:

```bash
./terminator
```

Start with a separate worker process for each tab:

```bash
./terminator --process
```

The equivalent long and compatibility forms are:

```bash
./terminator --use-process
./terminator useProcess=True
```

Explicitly select thread mode:

```bash
./terminator --thread
./terminator useProcess=False
```

The default is thread mode. The process-mode option changes how tab workers
are hosted; both modes run Bash sessions through PTYs.

## Everyday terminal use

Once the window opens, type commands as you would in a normal interactive
Bash terminal.

```bash
pwd
ls -lah
cd ~/Projects
mkdir -p build
g++ --version
```

### Pipes and redirection

Pipes and redirections are interpreted by Bash:

```bash
# Send command output to a file.
ls -lah > files.txt

# Append output rather than replacing the file.
date >> files.txt

# Read input from a file.
sort < names.txt

# Combine commands with a pipeline.
ps aux | grep firefox


You can combine pipes and redirections in the usual Bash way:

```bash
find . -type f -name '*.cpp' 2>/dev/null | sort > cpp-files.txt
```

### Interactive programs

Run interactive command-line programs directly:

```bash
nano notes.txt
top
htop
python3
```

Use `Ctrl+C` to interrupt a foreground command. Use `Ctrl+Z` to suspend a
foreground job, then use Bash job-control commands:

```bash
# Start a long-running command, then press Ctrl+Z.
sleep 200

# Inspect jobs suspended or running in this shell.
jobs

# Resume the current job in the foreground.
fg

# Resume a suspended job in the background.
bg

# Start a command in the background.
sleep 200 &
```

Job state belongs to the Bash session in the current tab. A different tab is
a separate shell session.

### Command and path completion

Press `Tab` to request Bash auot-completion:

```bash
cd /usr/lo<Tab>
```

If more than one completion is possible, press `Tab` again to display the
available candidates. Completion uses Bash's normal programmable-completion
and filename-completion behavior.

### Command history and line editing

Use the arrow keys to move through command history and edit the current
command line. Bash history is configured to be shared between Terminator tabs
using a common history file and a lock. The shell also uses timestamped
history entries.

## Tabs

- Click a tab to activate it.
- Click the `x` on a tab to close that tab.
- Click `+ NEW` to open another Bash session.
- Use the configured next-tab and previous-tab shortcuts to cycle through
  tabs.
- Each tab has its own working directory, shell variables, foreground
  command, and job table.

The implementation defines a maximum of 32 tabs.

## Default keyboard shortcuts

These are the defaults installed by the source. They can be changed in the
hotkey configuration window.

| Action | Default shortcut |
|---|---|
| New tab | `Ctrl+Shift+T` |
| Close active tab | `Ctrl+Shift+W` |
| Next tab | `Ctrl+Tab` |
| Previous tab | `Ctrl+Shift+Tab` |
| Open hotkey configuration | `Ctrl+Shift+K` |
| Toggle Matrix Rain mode | `F1` |
| Move cursor left / right | `Left` / `Right` |
| Previous / next history entry | `Up` / `Down` |
| Move to beginning / end of line | `Ctrl+A` / `Ctrl+E` |
| Delete to beginning / end of line | `Ctrl+U` / `Ctrl+K` |
| Delete previous word | `Ctrl+W` |
| Clear screen | `Ctrl+L` |
| Send EOF (exit on an empty prompt) | `Ctrl+D` |
| Copy selected text | `Ctrl+Shift+C` |
| Paste clipboard | `Ctrl+Shift+V` |

Additional selection/paste gestures:

- Drag with the mouse to select terminal text.
- Middle-click to paste the X11 `PRIMARY` selection.
- `Shift+Insert` also pastes the `PRIMARY` selection.

`Ctrl+C` and `Ctrl+Z` are sent to the active Bash session as terminal control
characters for interrupting and suspending foreground jobs. They are not the
GUI copy shortcut.

## Configure keyboard shortcuts

1. Press `Ctrl+Shift+K` (the default configuration shortcut).
2. Use `Up` and `Down` to select an action.
3. Press `Enter` to begin recording a new shortcut and press the desired key combination.
4. Press `Esc` to close the configuration window.

The configuration is saved automatically to:

```text
~/.terminatorrc
```

The file is loaded when Terminator starts. The configuration validates
shortcuts and rejects duplicate key combinations or unsupported modifier
bits. If the file is malformed, Terminator falls back to defaults and writes
a fresh configuration.

## Terminator Bash scripts (`.tbash`)

Terminator provides a small Bash script runner named `terminator-bash`.
Scripts created through the helper are stored by default in:

```text
~/.terminator/scripts/
```

They are ordinary Bash scripts and can use normal Bash syntax, arguments,
environment variables, loops, functions, pipes, and redirections.

### Create, list, and run scripts

Create a script template:

```bash
terminator-bash --create hello
```

This creates `~/.terminator/scripts/hello.tbash`.

List scripts:

```bash
terminator-bash --list
```

Run a script by name (the `.tbash` suffix is optional):

```bash
terminator-bash hello
terminator-bash hello.tbash
```

You can also pass a script path:

```bash
terminator-bash /scripts/hello.tbash
```

Pass arguments after the script name:

```bash
terminator-bash hello Alice
```

Display the helper's usage:

```bash
terminator-bash --help
```

The helper uses that directory for script creation, listing, and name lookup.

## `multiWatch`: repeat several commands

Every interactive Terminator Bash session defines that. It starts
separate repeating command loops, prints each command with a Unix timestamp,
and waits about two seconds between iterations. Press `Ctrl+C` to stop the
watch operation.

Example:

```bash
multiWatch "date" "uptime"
```

Monitor disk and memory usage together:

```bash
multiWatch "df -h" "free -h"
```


The commands are evaluated by Bash on each iteration. Only run commands you
trust, and be aware that a command which takes longer than the interval will
make its own loop run more slowly.

## Matrix Rain mode

Press `F1` to toggle the Matrix Rain visual effect. Press `F1` again to turn
it off. This is a display effect; it does not change the active Bash session.

## Scrolling

- Use pageup and pagedown to scroll

## Runtime files and session behavior

Terminator creates or uses the following per-user files:

| Path | Purpose |
|---|---|
| `~/.terminatorrc` | Saved GUI hotkey configuration |
| `~/.terminator/scripts/` | Default location for `.tbash` scripts |
| `/tmp/terminator_history_<uid>` | Bash history file shared by tabs |
| `/tmp/terminator_history_<uid>.lock` | Lock used when synchronizing Bash history |


The temporary Bash startup file sources `~/.bashrc` when present, then enables
Terminator's completion and helper functions. Terminator does not need you to
add those helpers manually to `~/.bashrc`.

## Troubleshooting

### `cannot open display` or no window appears

Run Terminator from a graphical X11 session and check:

```bash
echo "$DISPLAY"
```

This implementation uses Xlib directly. A Wayland desktop may need an
XWayland session with a valid `DISPLAY`.

### Missing X11 headers or linker errors

Install your distribution's X11 development package. On Debian/Ubuntu:

```bash
sudo apt install build-essential libx11-dev
```

Check that your compiler command includes the libraries shown in the Build
section. Some distributions do not require separate `-lutil` or `-lrt`
flags.

### `terminator-bash: command not found`

`terminator-bash` is a function defined in the interactive Bash sessions
started by Terminator. Run it inside a Terminator tab, not in an unrelated
terminal unless you separately define the function there.

### Script not found

List the available scripts and check the script directory:

```bash
terminator-bash --list
echo "$TERMINATOR_SCRIPT_DIR"
ls -la "$TERMINATOR_SCRIPT_DIR"
```

### Completion does not show the expected candidates

Press `Tab` once to complete. If the input is ambiguous, press `Tab` again
to list candidates. Programmable completions depend on the commands and
completion definitions installed in your Bash environment.


