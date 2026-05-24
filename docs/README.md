# ttychatter

User-friendly Gemini chat clients for the text-based terminal.

This project is built around the idea that a browser is not required for serious AI productivity. Sending text messages, attaching files, receiving generated artifacts, storing conversations, choosing models, editing prompts, and managing sessions can all be done from the terminal with user-owned files and plain-text logs.

Repository:

```text
https://github.com/remfan1994/ttychatter
```

Update the URL above if your published repository path changes.

---

## Project focus

ttychatter is not a collection of curl examples and is not intended primarily as developer API sample code.

The project goal is:

```text
robust end-user Gemini clients for terminal users
```

The clients are intended to be usable by people who want a practical chat application in the terminal, not just by programmers experimenting with API endpoints.

The project favors:

- terminal-native workflows
- plain-text session logs
- local file ownership
- transparent configuration
- persistent sessions
- attachment-oriented interaction
- model discovery from Google metadata
- maintainable, heavily commented source code
- multiple implementation tracks for different systems

The project avoids:

- Electron
- browser-only workflows
- hidden databases
- opaque state
- unnecessary framework dependencies
- one-shot API demo scripts presented as applications

---

## Current implementation tracks

```text
bash/bash-only/ttychatter-gemini-bash
    Dependency-light Bash client with no Python requirement.

bash/python3/ttychatter-gemini-python3
    Bash client with Python 3 helper snippets for stronger JSON, parsing,
    attachment, model-cache, and memory behavior.

ncurses/python/ttychatter-gemini-ncurses-python
    Full-screen Python ncurses reference implementation.

ncurses/c/ttychatter-gemini-ncurses
    Planned future native C ncurses implementation.
```

The Python ncurses version is currently the behavioral reference for the future C version.

The Bash/Python3 version is the practical line-oriented shell client.

The Bash-only version is the dependency-light client for systems where Python 3 is unavailable or unwanted.

---

## Repository layout

The repository uses a flat implementation layout. The provider name `gemini`
remains in the executable names because that is the user-facing namespace that
prevents command collisions and leaves room for future provider families, but
the repo directories stay simple.

```text
bash/
  bash-only/
    ttychatter-gemini-bash
    ttychatter-gemini-bash.1
    CHANGELOG.md

  python3/
    ttychatter-gemini-python3
    ttychatter-gemini-python3.1
    CHANGELOG.md

ncurses/
  python/
    ttychatter-gemini-ncurses-python
    ttychatter-gemini-ncurses-python.1
    CHANGELOG.md

  c/
    ANNOUNCED.md

docs/
  PYTHON_NCURSES_STABILIZATION.md
  BASH_RUNTIME_COMMAND_MODE.md
  RELEASE_CANDIDATE_AUDIT.md
  NAMESPACE_MIGRATION.md

README.md
LICENSE.txt
.gitignore
```

---

## Installation quick start

Install all current clients into `~/.local`:

```sh
./install.sh
```

Install only one track:

```sh
./install.sh --bash-only
./install.sh --bash-python3
./install.sh --ncurses-python
```

Verify the repository before installing:

```sh
scripts/smoke-test.sh
```

Uninstall installed executables and man pages:

```sh
./uninstall.sh
```

The installer and uninstaller do not write or delete API keys, configuration,
session logs, or attachments.  User data remains under the user-controlled
configuration and session directories.

See:

```text
docs/INSTALLATION.md
docs/MAINTAINERS_GUIDE.md
```

---

## Shared config file

All current Gemini clients use the shared ttychatter/Gemini config path:

```text
~/.config/ttychatter/gemini/config
```

Example:

```text
MODEL=gemini-2.5-flash
CONTEXT_TURNS=12
SESSION_DIR=~/.gpt
ATTACHMENT_DIR=~/.gpt/attachments
GEMINI_API_KEY=your_api_key_here
```

Common optional keys:

```text
EDITOR=vim
MONOCHROME=0
STARTUP_NOTICE=1
INPUT_PLACEHOLDER=Type your chat message here...
MODEL_TEST_PROMPT=Please reply with a short sentence confirming this model is available for text generation.
MAX_ATTACHMENT_BYTES=1048576
MODEL_FILTER_GENERATE_CONTENT=1
MODEL_FILTER_REQUIRE_TOKENS=1
MODEL_FILTER_HIDE_PREVIEW=0
MODEL_MIN_INPUT_TOKENS=0
MODEL_MIN_OUTPUT_TOKENS=0
MODEL_SORT_ORDER=name
```

Legacy key:

```text
HISTORY_LIMIT=12
```

`HISTORY_LIMIT` is accepted as a legacy alias for `CONTEXT_TURNS`. The preferred name is `CONTEXT_TURNS` because it describes the setting more accurately: it controls how many recent user/assistant exchanges are sent back to Gemini as conversation context.

---

## API key setup

Preferred:

```sh
export GEMINI_API_KEY="your_key"
```

Convenient but less secure:

```text
GEMINI_API_KEY=your_key
```

inside:

```text
~/.config/ttychatter/gemini/config
```

The ncurses version can accept a runtime-only API key through F3.

The Bash versions include command-line and runtime-command helpers for API-key setup.

Do not commit API keys.

Do not publish logs or attachments containing secrets.

---

## Model discovery and model selection

Google's model list can change. These tools do not claim to permanently know the best model.

Models are discovered through Google's model list endpoint:

```sh
curl "https://generativelanguage.googleapis.com/v1beta/models?key=YOUR_API_KEY"
```

The clients use this metadata to list and filter models that support text generation.

### Bash/Python3

```sh
bash/python3/ttychatter-gemini-python3 --update-models
bash/python3/ttychatter-gemini-python3 --models
bash/python3/ttychatter-gemini-python3 --select-model
bash/python3/ttychatter-gemini-python3 --test-model gemini-2.5-flash
```

### Bash-only

```sh
bash/bash-only/ttychatter-gemini-bash --update-models
bash/bash-only/ttychatter-gemini-bash --models
bash/bash-only/ttychatter-gemini-bash --select-model
bash/bash-only/ttychatter-gemini-bash --test-model gemini-2.5-flash
```

### Python ncurses

```sh
ncurses/python/ttychatter-gemini-ncurses-python --models
ncurses/python/ttychatter-gemini-ncurses-python --test-model gemini-2.5-flash
```

Inside the ncurses interface, press F2 for model tools.

---

## Bash runtime command mode

Both Bash clients include runtime colon-command mode.

Inside a running Bash chat, type a local command beginning with `:` and press Ctrl+D. Colon commands are handled by the client. They are not sent to Gemini, not logged as chat, and not stored in conversation memory.

Examples:

```text
:help
:models
:update-models
:select-model
:test-model gemini-2.5-flash
:model gemini-2.5-flash
:model-save gemini-2.5-flash
:config
:set CONTEXT_TURNS 20
:unset MODEL
:set-api-key
:forget-api-key
:memory
:clear-memory
:attach /path/to/file.txt
:attachments
:editor
:credits
:doctor
:rename android-backup
:quit
```

In the Bash/Python3 version, memory editing is also available:

```text
:edit-memory
```

To send a literal Gemini message that begins with a colon, escape it with a backslash:

```text
\:this goes to Gemini
```

This command mode is the line-oriented equivalent of the ncurses function-key menus.

---

## Bash-only client

Path:

```text
bash/bash-only/ttychatter-gemini-bash
```

Current line:

```text
ttychatter-gemini-bash 0.5.0-bash-only
```

Purpose:

```text
as many end-user features as possible without Python
```

Dependencies:

- Bash
- curl
- common Unix tools such as sed, awk, grep, tr, base64, mktemp
- optional jq for stronger JSON handling

The Bash-only edition is not a curl demo and is not intended as a developer-only sample. It is a real terminal chat client with lighter dependencies.

Strengths:

- no Python dependency
- interactive chat loop
- config support
- model list/update/select/test
- runtime colon commands
- rolling context
- session logs
- attachment attempts
- startup Project Lead notice

Limitations:

- JSON parsing is best-effort without jq
- media support is more limited than the Python3 and ncurses editions
- no full-screen UI
- no mouse or curses menus

---

## Bash/Python3 client

Path:

```text
bash/python3/ttychatter-gemini-python3
```

Current line:

```text
ttychatter-gemini-python3 3.2
```

Purpose:

```text
practical line-oriented shell client with Python 3 helpers
```

Dependencies:

- Bash
- curl
- Python 3

Python 3 is used for tasks where shell alone is fragile:

- JSON generation
- JSON parsing
- URL quoting
- model-cache handling
- attachment parsing
- memory/context parsing

This version has a stronger implementation than Bash-only while still preserving a simple line-oriented terminal workflow.

---

## Python ncurses client

Path:

```text
ncurses/python/ttychatter-gemini-ncurses-python
```

Current line:

```text
ttychatter-gemini-ncurses-python 0.9.3
```

Purpose:

```text
full-screen terminal UI and behavioral reference for future C ncurses
```

Dependencies:

- Python 3
- curses support in Python

Core controls:

```text
F1      command/help menu
F2      model tools
F3      API key manager
F4      session browser
F5      options
F6      memory
F7      files/attachments
F8      external editor
F9      credits/about
F10     quit
Ctrl+G  send message
```

The ncurses version supports:

- scrollable transcript
- wrapped input
- mousewheel scrolling
- clickable command strip
- model cache and filters
- model testing
- runtime API-key entry
- session browser and rename
- options form
- memory view/edit
- attachment browser
- external editor support
- diagnostics through `--doctor`

---

## Project Lead startup notice

New blank sessions may display a temporary transcript-style notice from:

```text
remfan1994
```

Notice text:

```text
I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994
```

Behavior:

- shown only in new blank sessions
- removed when the user starts chatting
- not written to session logs
- not sent to Gemini
- not stored in conversation memory

This can be controlled with:

```text
STARTUP_NOTICE=1
STARTUP_NOTICE=0
```

---

## Sessions and memory

Sessions are plain text log files.

Default:

```text
~/.gpt/<session>.log
```

Configurable:

```text
SESSION_DIR=/path/to/sessions
```

The permanent session log is not the same thing as the active memory/context buffer.

```text
session log
    Permanent archive.

context memory
    Recent window sent back to Gemini.
```

`CONTEXT_TURNS` controls the recent memory window.

The Bash clients use memory snapshots and/or recent log entries to rebuild context when reopening sessions.

The ncurses client stores memory snapshots and can view/edit memory through F6.

---

## Attachments

Code blocks returned by Gemini can be extracted to files.

Default attachment directory:

```text
~/.gpt/attachments
```

Configurable:

```text
ATTACHMENT_DIR=/path/to/attachments
```

Local file attachment support varies by implementation:

- ncurses/python supports the richest file/attachment workflow.
- bash/python3 supports stronger text/file attachment behavior than Bash-only.
- bash-only attempts attachment support with lighter dependencies.

Media support depends on:

- selected Gemini model
- Gemini API support
- file MIME type
- file size
- response parts returned by the model

---

## Diagnostics

Python ncurses:

```sh
ncurses/python/ttychatter-gemini-ncurses-python --doctor
```

Bash/Python3:

```sh
bash/python3/ttychatter-gemini-python3 --doctor
```

Bash-only:

```sh
bash/bash-only/ttychatter-gemini-bash --doctor
```

The diagnostics are intended to distinguish local environment problems from project bugs.

---

## Man pages

Man page filenames follow the executable names:

```text
bash/bash-only/ttychatter-gemini-bash.1
bash/python3/ttychatter-gemini-python3.1
ncurses/python/ttychatter-gemini-ncurses-python.1
```

Future C version:

```text
ncurses/c/ttychatter-gemini-ncurses.1
```

Example install:

```sh
mkdir -p ~/.local/share/man/man1
cp bash/bash-only/ttychatter-gemini-bash.1 ~/.local/share/man/man1/
cp bash/python3/ttychatter-gemini-python3.1 ~/.local/share/man/man1/
cp ncurses/python/ttychatter-gemini-ncurses-python.1 ~/.local/share/man/man1/
```

Then:

```sh
man ttychatter-gemini-bash
man ttychatter-gemini-python3
man ttychatter-gemini-ncurses-python
```

---

## C ncurses version

The C ncurses version is announced but not started.

Planned executable:

```text
ncurses/c/ttychatter-gemini-ncurses
```

The C implementation should begin only after the Python ncurses version is stable enough to serve as the reference implementation.

See:

```text
ncurses/c/ANNOUNCED.md
```

---

## Documentation and code commentary

This project intentionally uses verbose source comments.

The comments are for future-proofing and maintenance, not merely education. Future maintainers, fork authors, bug fixers, and port authors should be able to understand why the code exists, what assumptions it makes, and where the fragile areas are.

The code is expected to be readable by strangers.

Additional maintainer documents are included under `docs/`:

```text
docs/BEHAVIOR_CONTRACT.md
    Shared behavior contract for all implementations, especially the future C port.

docs/CONFIG_REFERENCE.md
    Shared config-key reference and compatibility notes.

docs/SESSION_LOG_FORMAT.md
    Session log and context snapshot format guidance.

docs/ATTACHMENT_BEHAVIOR.md
    Attachment extraction, local attachment, inline data, and safety guidance.

docs/MAINTAINER_COMMENTARY_POLICY.md
    Project policy for intentionally verbose source commentary.

docs/RELEASE_CANDIDATE_AUDIT.md
    Stabilization checklist before treating a release as C-reference-ready.
```

A local smoke-test helper is also included:

```sh
scripts/check-project.sh
```

It checks local syntax, version output, and help output without requiring a network connection or API key.

---

## Known generated files to ignore

The repository should not include Python cache files or local runtime artifacts.

Examples:

```text
__pycache__/
*.pyc
.gpt/
```

A `.gitignore` is included for this purpose.

---

## License

See:

```text
LICENSE.txt
```
