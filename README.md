# Terminator — A Custom Terminal Emulator

**Computing Lab (CS69201) · Term Project**

## What Is Terminator?

Terminator is a terminal emulator and shell implemented entirely in **C for Linux**. It renders its own X11 window with multiple tabs and runs commands with pipes, redirection, job control, history, auto-completion, and bash script interpretation.

The project exercises core POSIX system calls: `fork`, `execvp`, `wait`, `pipe`, `dup2`, `open`/`read`/`write`, `select`/`poll`, `signal`/`kill`, together with POSIX threads, semaphores, and shared memory.

## Dependencies

Install the required development libraries on your Linux system:

```bash
# Debian / Ubuntu
sudo apt-get install build-essential libx11-dev

# Fedora / RHEL
sudo dnf install gcc make libX11-devel

# Arch Linux
sudo pacman -S base-devel libx11
```

## Building

```bash
make
```

This produces the `./terminator` executable.

To clean build artefacts:

```bash
make clean
```

## Running

### Thread Mode (Default)

Each tab runs as a separate thread within one process. Shared state (e.g., history) is protected by mutexes.

```bash
./terminator
```

### Process Mode

Each tab runs as a separate forked process. History is stored in shared memory, protected by a process-shared semaphore.

```bash
./terminator useProcess=True
```

**Both modes behave identically to the user.**

## Features Implemented

### Task 1 — X11 GUI and Tabs
- Full X11 window rendering with Xlib
- Tab bar with clickable tabs showing `[id: cwd]`
- Open, close, and switch tabs (keyboard and mouse)
- Per-tab text buffer with scrolling
- Non-busy-waiting event loop using `select()`

### Task 2 — Built-in and External Commands
- External commands run via `fork()` + `execvp()`
- Built-in commands: `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg`, `source`, `export`
- Last exit status available as `$?`
- Unknown commands print an error message

### Task 3 — Multiline Input and Quoting
- Line continuation with trailing `\`
- Single and double quotes spanning multiple lines
- Single quotes are literal; double quotes expand `$VAR` and substitutions
- Command substitution with `$(...)` and backticks (nested, multi-line)

### Task 4 — I/O Redirection
- Input redirection: `< file`
- Output redirection: `> file`
- Combined: `< infile > outfile`
- Missing input file reported before execution

### Task 5 — Pipes
- Pipelines of any length: `cmd1 | cmd2 | cmd3 | ...`
- N commands use N−1 pipes and N child processes
- Combinable with redirection

### Task 6 — multiWatch
- `multiWatch ["cmd1", "cmd2", ...]` runs commands in parallel
- Uses `select()`/`poll()` (no busy-waiting)
- Output formatted with command name, unix timestamp, and separators
- Re-runs commands every 2 seconds
- Ctrl+C terminates all children; shell keeps running

### Task 7 — Hotkeys
- Full set of bash-style editing keys (see table below)
- Configurable via `~/.terminatorrc`
- Custom bindings take effect without recompiling
- Persist across restarts
- Invalid/conflicting bindings reported with fallback to defaults

| Key | Action |
|-----|--------|
| ← / → | Move within the line |
| Ctrl+A / Ctrl+E | Move to start / end of line |
| ↑ / ↓ | Browse command history |
| Ctrl+U / Ctrl+K | Delete to start / end of line |
| Ctrl+W | Delete previous word |
| Ctrl+L | Clear screen |
| Ctrl+D | Exit shell / send EOF |
| Ctrl+Shift+C / V | Copy / paste (X11 selections) |
| Ctrl+R | Search history |
| Ctrl+C | Interrupt foreground command |
| Ctrl+Z | Suspend foreground command |
| Ctrl+T | New tab |
| Ctrl+Shift+W | Close tab |
| Ctrl+PageDown/Up | Next / previous tab |

### Task 8 — Signals and Job Control
- Ctrl+C stops the running command; Terminator itself never stops
- Ctrl+Z suspends the running command
- `cmd &` starts a job in the background
- `jobs`, `fg %N`, `bg %N` for job management
- Signals delivered via `kill()` from X11 key events

### Task 9 — Searchable History
- Last 10,000 commands stored in `~/.terminator_history`
- `history` shows the most recent 1,000 commands
- Ctrl+R search with 3-tier matching:
  1. Exact match (most recent)
  2. Longest common substring (> 2 chars)
  3. No match message
- History shared across all tabs

### Task 10 — File-name Auto-completion
- Tab key completes the word before the cursor
- One match: complete it
- Multiple matches: extend to longest common prefix, show numbered list
- No match: do nothing
- Rest of the line preserved

### Task 11 — Running Bash Scripts
- Interprets scripts itself (does NOT hand to `/bin/bash`)
- Supports: comments, pipes, redirection, command substitution
- Variables: `NAME=value`, `$NAME`, `$1`, `$2`, `$#`, `$?`
- Conditionals: `if`/`elif`/`else`/`fi` with `[ ... ]` tests
- Tests: `-eq`, `-gt`, `-f`, `-d`, `=`, etc.
- `exit [n]`
- Syntax errors reported before execution

## Configuration

Create `~/.terminatorrc` to customise hotkey bindings:

```
# Format: ACTION = modifier+key
LINE_START = Ctrl+a
LINE_END   = Ctrl+e
COPY       = Ctrl+Shift+c
PASTE      = Ctrl+Shift+v
```

A sample configuration file is included in the repository root.

## Testing

Terminator includes an automated unit and integration test suite covering all 11 modules:

```bash
make test
```

This runs 59 individual test assertions validating multiline input, quoting, environment and command substitution, I/O redirection, multi-stage pipelines, multiWatch argument parsing, hotkeys, job control, thread-mode and process-mode shared history with 3-tier LCS search, filename auto-completion, and bash script AST execution.

## Project Structure

```
terminator/
├── Makefile              Build system (make, make clean, make test, make run)
├── README.md             User manual and project summary
├── DESIGN.md             Architecture and detailed design document
├── .terminatorrc         Sample hotkey configuration
├── include/              Header files
│   ├── terminator.h      Core shared types and constants
│   ├── task1.h ... task11.h Module task wrappers
│   ├── gui.h, tab.h      Task 1 headers
│   ├── shell.h, builtins.h Task 2 headers
│   ├── parser.h          Task 3 header
│   ├── redirect.h        Task 4 header
│   ├── pipes.h           Task 5 header
│   ├── multiwatch.h      Task 6 header
│   ├── hotkeys.h         Task 7 header
│   ├── jobs.h            Task 8 header
│   ├── history.h         Task 9 header
│   ├── autocomplete.h    Task 10 header
│   └── script.h          Task 11 header
├── src/                  Module source files
│   ├── main.c            Entry point & initialization
│   ├── task1.c           Task 1: X11 GUI and Tabs
│   ├── task2.c           Task 2: Built-ins & Shell command execution
│   ├── task3.c           Task 3: Multiline Input & Quoting
│   ├── task4.c           Task 4: I/O Redirection (<, >, >>)
│   ├── task5.c           Task 5: Pipeline execution
│   ├── task6.c           Task 6: multiWatch concurrent execution
│   ├── task7.c           Task 7: Hotkeys & ~/.terminatorrc config
│   ├── task8.c           Task 8: Signals & Job Control
│   ├── task9.c           Task 9: Searchable History (thread & process modes)
│   ├── task10.c          Task 10: Auto-completion
│   └── task11.c          Task 11: Bash Script Interpreter
├── tests/                Automated test suite
│   └── test_suite.c      Unit & integration tests (59 tests)
└── test_scripts/         Sample bash scripts for interpreter tests
    ├── check.sh          File check & compile
    ├── test_features.sh  Feature test suite
    └── test_redirect.sh  Redirection tests
```

## Known Limitations

- Loops (`for`, `while`) and functions in bash scripts are not supported (as per spec)
- The `>>` append redirection is supported but not explicitly required
- Globbing (`*.txt`) relies on the shell expanding via wordexp or manual glob

## Authors

Computing Lab (CS69201) Term Project
