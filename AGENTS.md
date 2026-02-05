# AGENTS.md

VED is an emacs-style editor that runs on the terminal.

## Constraints

- Write C23
- UTF-8 only. No other charset is supported.
- Event-driven and non-blocking
- Ensure it runs on any POSIX
- Do not depend on system libraries that are not generally available

## Code Style

- 2-space indents
- Max line length of 80 chars
- Struct names in `CamelCase`
- Function and variable names in `snake_case`
- Macros in `SCREAMING_CASE`
