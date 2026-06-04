
# C ncurses version announced

A native C ncurses implementation is planned for gemini-terminal-tools.

The C version will not begin until the Python ncurses version is considered stable enough to serve as the behavioral reference implementation.

Current policy:

- Python ncurses is the prototype and feature-complete reference.
- Bash remains the lightweight scriptable implementation.
- C ncurses will be built after the Python interface stabilizes.
- The C version should preserve the same session format, config keys, memory behavior, model-testing workflow, and attachment conventions where practical.

The C version should prioritize:

- portability
- speed
- low dependencies
- traditional Unix build/install flow
- predictable ncurses behavior
- clean separation between UI, config, sessions, API, and attachments

Do not start C implementation until the Python ncurses branch has completed its stabilization pass.
