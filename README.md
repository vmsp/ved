# Vitor's Editor

Vitor's Editor (VED) is a POSIX-compliant, MicroEmacs-style editor for terminal
sessions. It is written in C23, runs in raw terminal mode, requires no external
dependencies and uses an event-driven, non-blocking input loop.

![Screenshot of VED editing this repository's `src/render.c`](/screenshot.png)

## Features

- Emacs-style navigation and editing
- UTF-8 aware cursor movement and word operations
- Kill ring (`C-k`, `C-w`, `M-d`, `M-Backspace`) + yank (`C-y`, `M-y`)
- Region selection with mark (`C-Space` / `C-@`) and mouse drag
- File finder (`M-p`) with `.gitignore`-aware fuzzy matching
- Tree-sitter highlighting for C (`.c`, `.h`)
- OSC 52 clipboard copy on region copy/kill
- Mouse scrolling and selections

## Requirements

- POSIX system
- `make`
- C compiler with C23 support

## Installing

Install to `/usr/local/bin` by default:

```sh
make
sudo make install
```

Use a custom prefix:

```sh
make PREFIX="$HOME/.local/bin" install
```

Uninstall:

```sh
sudo make uninstall
```

## Usage

Open an existing file:

```sh
./ved path/to/file.c
```

Start with an empty buffer:

```sh
./ved
```

Save the current buffer:

- `C-x C-s`
- If no file is associated yet, VED opens a `Save as:` prompt

Exit VED with `C-x C-c`.

## Keybinds

### Movement

- `C-p` / `C-n`: previous / next line
- `C-b` / `C-f`: previous / next character
- `C-a` / `C-e`: beginning / end of line
- `M-b` / `M-f`: previous / next word
- Arrow keys: move cursor (or navigate finder list)

### Editing

- `Enter`: insert newline
- `Tab`: insert tab in Makefile mode, otherwise 2 spaces
- `Backspace`: delete backward
- `C-d`: delete forward

### Mark, Region, Kill Ring

- `C-Space` (`C-@`): set/clear mark
- `C-w`: kill region
- `M-w`: copy region
- `C-k`: kill to end of line (or join with next line)
- `M-d`: kill next word
- `M-Backspace`: kill previous word
- `C-y`: yank (paste last kill)
- `M-y`: rotate kill ring after `C-y`

### Features

- `M-p`: open file finder (from project root containing `.git/`)
- `C-s`: incremental search forward
- `C-r`: incremental search backward
- `C-x C-s`: save file
- `C-g`: cancel pending `C-x`, close prompt, clear mark, clear state
- `C-x C-c`: exit editor

### Mouse

- Left click: move cursor and set mark anchor
- Drag: update region selection
- Scroll wheel: scroll buffer

## tmux

When using VED from inside tmux, you'll want to enable terminal passthrough and
clipboard integration:

```sh
set -g allow-passthrough on
set -g set-clipboard on
```

## License

Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>.

Distributed unser the MIT license. See [LICENSE](LICENSE).
