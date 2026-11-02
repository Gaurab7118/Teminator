# Terminator — A Custom Terminal Emulator

**Computing Lab (CS69201) · Term Project**

---

## What Is Terminator?

Terminator is a high-performance, multi-tabbed terminal emulator and Unix shell implemented from scratch in **C (C11) for Linux / X11**. It renders its own graphical X11 window, manages concurrent tab shells across thread and process architectures, and provides a full shell execution engine with multiline parsing, I/O redirection, arbitrary-length pipelines, job control, 3-tier searchable history, filename auto-completion, parallel command monitoring (`multiWatch`), and direct Bash script interpretation.

The project directly exercises core POSIX system calls: `fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `open`/`read`/`write`, `select`/`poll`, `signal`/`sigaction`/`kill`, together with POSIX threads (`pthreads`), semaphores (`sem_t`), and shared memory (`shm_open`).

---

## Quickstart

### Prerequisites & Dependencies

Install development libraries on your Linux distribution (or WSL2 Ubuntu):

```bash
# Debian / Ubuntu / WSL2
sudo apt-get update && sudo apt-get install -y build-essential libx11-dev

# Fedora / RHEL
sudo dnf install -y gcc make libX11-devel

# Arch Linux
sudo pacman -S --needed base-devel libx11
```

### Build & Run in One Command

To compile all files and immediately launch Terminator:

```bash
make run
```

If you prefer a completely clean rebuild from scratch and launch:

```bash
make clean && make run
```

*Note: Building automatically generates both `./terminator` and `./terminator.exe` (mirrored to `../terminator.exe` for project submission portals).*

---

## Execution Modes

Terminator supports dual execution architectures selectable at launch:

### 1. Thread Mode (Default)
Each terminal tab runs as an independent POSIX worker thread (`pthread`) within the main process. Shell-GUI communication is achieved through inter-thread pipe pairs, and shared resources (such as command history) are protected with POSIX mutexes:

```bash
./terminator
# or via Makefile:
make run
```

### 2. Process Mode
Each terminal tab runs as an independent child process created via `fork()`. Command history is stored in POSIX shared memory (`/dev/shm`), protected with process-shared semaphores:

```bash
./terminator useProcess=True
# or via Makefile:
make run-proc
```

*Both modes present an identical graphical interface and user experience.*

---

## Key Features & Task Breakdown

### Task 1 — X11 GUI, Tab Engine & Aesthetics
- **Display Theme:** Pure black background (`#000000`) and high-contrast white text (`#ffffff`).
- **Natural Terminal Flow:** The active prompt starts at the top beneath previous output and smoothly stays pinned to the bottom row once the screen fills.
- **Interactive Tab Bar:** Click tab labels to switch, click `x` to close, or click `[+]` to open new tabs.
- **Mouse Text Selection & Clipboard:** Click and drag with the left mouse button to highlight text; releasing automatically copies it to both the X11 primary selection and system clipboard. Middle-click (or wheel click) pastes at the cursor.
- **Event Multiplexing:** Non-busy-waiting GUI event loop using `select()` multiplexing X11 events, shell output pipes, and a self-pipe notification mechanism for instant background updates.

### Task 2 — Built-in and External Commands
- **External Execution:** Spawns binaries via `fork()` + `execvp()`, accurately tracking exit status in `$?`.
- **Built-in Commands:**
  - `cd [dir]` — Changes directory with `~` home expansion and updates tab title.
  - `export VAR=val` — Sets environment variables with live variable expansion.
  - `history` — Lists previously executed commands.
  - `jobs`, `fg [%id]`, `bg [%id]` — Job control management.
  - `source file.sh` — Executes a shell script directly in the current shell environment.
  - `multiWatch [...]` — Parallel command monitor.
  - `clear` — Clears the active tab buffer.
  - `exit [n]` — Closes active tab or terminates application on last tab.

### Task 3 — Multiline Input, Quoting & Expansions
- **Line Continuation:** Recognizes trailing backslash (`\`) and multiline unclosed single or double quotes, prompting with `> `.
- **Quote Rules:** Single quotes (`'...'`) preserve literal text; double quotes (`"..."`) expand variables (`$VAR`, `$?`, `$HOME`) and command substitutions.
- **Single-Line Merging:** Multiline continuations seamlessly join into a unified command string with space separation.
- **Command Substitution:** Supports nested `$(...)` and backticks ``` `cmd` ```, executing subshells and replacing expressions with their trimmed stdout.

### Task 4 — I/O Redirection
- **Input Redirection (`<`):** Redirects standard input from a file. Pre-execution checks report missing input files before creating child processes.
- **Output Redirection (`>` and `>>`):** Truncate-overwrite (`>`) and append (`>>`) redirection using `dup2()`.
- **Combined Redirection:** Seamlessly combine input and output redirection in a single command (e.g. `cat < in.txt > out.txt`).

### Task 5 — Multi-Stage Pipelines
- **Arbitrary Pipeline Depth:** Handles any number of piped commands (`cmd1 | cmd2 | ... | cmdN`).
- **Resource Management:** Creates $N-1$ pipe pairs for $N$ processes and correctly reaps all child processes upon completion.
- Combinable with I/O redirections on initial and final stages.

### Task 6 — Parallel Command Monitor (`multiWatch`)
- **Syntax Support:** Supports both quoted JSON arrays `multiWatch ["cmd1", "cmd2"]` and unquoted token arrays `multiWatch [date, uptime]`.
- **Periodic Execution:** Runs all commands in parallel every 2 seconds, displaying timestamped blocks and dividers.
- **Non-blocking I/O:** Monitors child process pipes using `poll()` without busy-waiting.
- **Clean Interruption:** Shows `[multiWatch active -- Press Ctrl+C to stop]`. Pressing `Ctrl+C` terminates watched child processes cleanly without killing the parent shell.

### Task 7 — Hotkey Bindings & Configuration
- Full set of Bash-style line editing shortcuts and window navigation hotkeys.
- **Configurable via `~/.terminatorrc`:** Load and persist custom key bindings and palette configurations across restarts.

### Task 8 — Signals & POSIX Job Control
- **`Ctrl+C` (`SIGINT`):** Interrupts the active foreground process without crashing the terminal.
- **`Ctrl+Z` (`SIGTSTP`):** Suspends the foreground process into a stopped background job.
- **Background Execution (`&`):** Appending `&` runs commands in the background with immediate job ID and PID reporting (`[1] 12345`).
- **Job Management:** `jobs` displays active jobs; `fg %N` brings jobs to foreground; `bg %N` resumes stopped background jobs with `SIGCONT`.
- Asynchronous notification on job completion (`[1]+ Done cmd`).

### Task 9 — 3-Tier Searchable History
- Persistent command history across sessions.
- Interactive `Ctrl+R` reverse search implementing 3-tier matching:
  1. **Tier A:** Exact match (most recent).
  2. **Tier B:** Longest Common Subsequence (LCS) substring match.
  3. **Tier C:** "No match" notification if no candidate exists.
- Shared history synchronized across all tabs in both thread mode and process mode.

### Task 10 — Filename Auto-Completion
- Pressing `Tab` completes file and directory names relative to current working directory.
- **Single Match:** Instantly completes the filename inline.
- **Multiple Matches:** Extends input to the Longest Common Prefix (LCP) and displays a numbered selection menu.

### Task 11 — Direct Bash Script Interpreter
- Directly parses and executes `.sh` scripts via custom Abstract Syntax Tree (AST) evaluator without delegating to `/bin/bash`.
- Supports parameter passing (`$1`, `$2`, `$@`, `$#`), variable assignment, exit statuses (`$?`), and comments.
- Supports conditionals (`if`/`elif`/`else`/`fi`) with bracket test expressions (`[ ... ]`), including file checks (`-f`, `-d`), string tests (`=`, `!=`, `-z`, `-n`), and numeric comparisons (`-eq`, `-ne`, `-lt`, `-gt`, `-le`, `-ge`).

---

## Keyboard & Mouse Shortcuts

### Line Editing & Shell Controls
| Key | Action |
|:---|:---|
| `←` / `→` | Move cursor left / right |
| `Ctrl + A` / `Ctrl + E` | Jump to start / end of input line |
| `↑` / `↓` | Browse command history backwards / forwards |
| `Ctrl + U` | Clear entire input line |
| `Ctrl + K` | Delete from cursor to end of line |
| `Ctrl + W` | Delete word before cursor |
| `Ctrl + L` | Clear terminal screen |
| `Ctrl + D` | Exit shell / send EOF |
| `Ctrl + R` | Interactive 3-tier history search |
| `Tab` | Trigger filename auto-completion |
| `Ctrl + C` | Send `SIGINT` (interrupt foreground process / stop `multiWatch`) |
| `Ctrl + Z` | Send `SIGTSTP` (suspend foreground process to background) |

### Clipboard & Mouse Controls
| Action | Description |
|:---|:---|
| **Left Click + Drag** | Select text in terminal (automatically copies to clipboard on release) |
| **Middle Click** | Paste clipboard contents at active cursor position |
| **`Ctrl + Shift + C`** | Copy current selection / input to clipboard |
| **`Ctrl + Shift + V`** | Paste clipboard contents into input |
| **Mouse Wheel Up / Down** | Scroll output buffer up / down |
| **`PageUp` / `PageDown`** | Scroll terminal view by pages |

### Tab Management
| Shortcut / Action | Description |
|:---|:---|
| **Left Click `[+]`** / `Ctrl + T` | Open new tab |
| **Left Click Tab Label** | Switch to clicked tab |
| **`Ctrl + PageDown`** / `Ctrl + Tab` | Switch to next tab |
| **`Ctrl + PageUp`** / `Ctrl + Shift + Tab` | Switch to previous tab |
| **`Alt + 1` ... `Alt + 9`** | Directly jump to tab 1 through 9 |
| **Left Click `x`** / `Ctrl + Shift + W` | Close active tab |

---

## Automated Test Suites

The repository contains two comprehensive automated test suites:

### 1. Unit & Integration Test Suite (`make test`)
Executes 59 unit assertions covering multiline parsing, quoting rules, variable expansion, substitutions, redirection, pipelines, multiWatch argument parsing, hotkeys, jobs, shared memory history, LCS search, auto-completion, and script conditionals:

```bash
make test
```

### 2. End-to-End Task Verification (`make verify`)
Runs live end-to-end execution of all 11 assignment tasks, validating tab lifecycle, built-ins, pipelines, redirections, multiWatch polling, job control, and script sourcing:

```bash
make verify
```

---

## Project Structure

```text
terminator/
├── Makefile              Build system (make, make clean, make test, make verify, make run)
├── README.md             User manual and project documentation
├── DESIGN.md             Architecture and detailed design document
├── .terminatorrc         Hotkey and terminal configuration file
├── include/              C Header Files
│   ├── terminator.h      Global types, structures, and limits
│   ├── tab.h             Tab lifecycle and pipe definitions
│   ├── gui.h             X11 GUI rendering and mouse/keyboard prototypes
│   ├── shell.h           Command execution engine prototypes
│   ├── builtins.h        Built-in command declarations
│   ├── parser.h          Tokenizer, quote parsing, and variable expansion
│   ├── redirect.h        I/O redirection prototypes (<, >, >>)
│   ├── pipes.h           Pipeline creation and process chaining
│   ├── multiwatch.h      Parallel process monitoring engine
│   ├── hotkeys.h         Keybinding lookup and dispatch table
│   ├── jobs.h            Signal handlers and background jobs table
│   ├── history.h         Shared memory history and 3-tier LCS search
│   ├── autocomplete.h    Filename auto-completion and LCP matching
│   ├── script.h          Bash AST interpreter and [ ... ] evaluator
│   └── task1.h .. task11.h Syllabus modular task wrappers
├── src/                  C Source Files
│   ├── main.c            Application entry point and flag parsing
│   ├── task1.c           Task 1: X11 GUI, tabs, mouse selection, and event loop
│   ├── task2.c           Task 2: Built-ins, fork/execvp, and command routing
│   ├── task3.c           Task 3: Multiline parser, quotes, variables, and substitutions
│   ├── task4.c           Task 4: I/O Redirection (<, >, >>)
│   ├── task5.c           Task 5: Multi-stage pipeline execution
│   ├── task6.c           Task 6: multiWatch parallel polling engine
│   ├── task7.c           Task 7: Hotkey dispatcher and ~/.terminatorrc loader
│   ├── task8.c           Task 8: Signal handling and POSIX job control
│   ├── task9.c           Task 9: Searchable history (thread and process modes)
│   ├── task10.c          Task 10: Filename auto-completion and menu selection
│   └── task11.c          Task 11: Direct Bash script interpreter (AST evaluation)
├── tests/                Automated Verification Suites
│   ├── test_suite.c      59-assertion unit & integration test suite
│   └── verify_all_tasks.c End-to-end task and action verification harness
└── test_scripts/         Bash Script Interpreter Test Files
    ├── check.sh          Conditionals and file existence tests
    ├── test_features.sh  Script arguments, arithmetic, and assignments
    └── test_redirect.sh  Script-level redirection and pipelines
```

---

## Authors

Computing Lab (CS69201) Term Project
