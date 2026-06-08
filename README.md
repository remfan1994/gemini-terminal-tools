# ttychatter

`ttychatter` is a terminal-first OpenRouter chat client family. The project keeps a plain text log as the durable session record, reconstructs context from that log, and offers several front ends that share the same config, model cache, session directory, attachments directory, and session metadata.

The current implementations are:

- `ttychatter` - C command-line client, the most complete implementation.
- `ttychatter.python` - Bash plus Python 3 helper client, close to C feature parity for line-oriented use.
- `ttychatter.bash` - Bash-only client for constrained systems; intentionally scaled down where JSON or edit/branch behavior would be fragile.
- `ttychatter.ncurses` - Python ncurses TUI for interactive terminal use.

The design equation is:

```text
session = timestamped log file + metadata + user/assistant turns
memory  = reconstructed context from saved logs
ui      = one of C, Bash, Bash+Python, or ncurses
```

The storage layer is intentionally boring. The user-facing layer is allowed to be friendly.

## Features

Core features across the family:

- Interactive chat sessions.
- Batch prompt from a file, stdin, or `--prompt TEXT`.
- Timestamped session logs under the shared session directory.
- Friendly session titles stored as metadata and displayed by `--list`.
- Metadata retitling with `--rename-session SESSION TITLE` and `:rename TITLE` where interactive commands are available.
- Optional AI-generated title metadata, off by default.
- OpenRouter model cache, model listing, model filtering, favorites, selection, and model testing.
- Attachments where supported by the implementation and model.
- Session search.
- Branching from an earlier turn.
- Export to text, Markdown, or JSON where supported.
- Streaming output where supported.
- Local status/progress messages that report client work, not hidden model thoughts.
- External voice input and voice output hooks.
- XDG-compatible config/cache/data paths.

The C client is the reference implementation. The Python-helper client follows it most closely. Bash-only and ncurses scale features down according to their interface constraints.

## Install

### C client

```sh
make
sudo install -m 755 ttychatter /usr/local/bin/ttychatter
sudo install -m 644 ttychatter.1 /usr/local/share/man/man1/ttychatter.1
```

Required build/runtime libraries are `libcurl` and `json-c`.

### Script clients

```sh
chmod +x ttychatter.python ttychatter.bash ttychatter.ncurses
install -m 755 ttychatter.python ~/.local/bin/ttychatter.python
install -m 755 ttychatter.bash ~/.local/bin/ttychatter.bash
install -m 755 ttychatter.ncurses ~/.local/bin/ttychatter.ncurses
```

`ttychatter.bash` requires Bash and common Unix tools. `ttychatter.python` requires Bash and Python 3. `ttychatter.ncurses` requires Python 3 with the curses module.

## Configuration

The shared config file is:

```text
$XDG_CONFIG_HOME/ttychatter/openrouter/config
```

If `XDG_CONFIG_HOME` is unset, this becomes:

```text
~/.config/ttychatter/openrouter/config
```

The file is line-oriented:

```text
KEY=VALUE
```

Common keys:

```text
MODEL=openrouter/auto
CONTEXT_TURNS=12
SESSION_DIR=~/.local/share/ttychatter/openrouter/sessions
ATTACHMENT_DIR=~/.local/share/ttychatter/openrouter/attachments
MODEL_CACHE_FILE=~/.cache/ttychatter/openrouter/models.json
MODEL_FAVORITES_FILE=~/.config/ttychatter/openrouter/model-favorites
STARTUP_NOTICE=1
STREAM=0
STATUS_UPDATES=0
SESSION_AUTO_TITLE=0
SESSION_TITLE_MODEL=openrouter/auto
SESSION_TITLE_MAX_WORDS=8
VOICE_INPUT_CMD=
VOICE_OUTPUT_CMD=
VOICE_OUTPUT=0
SEND_INPUT=ctrl_d
THEME=default
CODE_ATTACHMENT_MIN_LINES=5
MAX_ATTACHMENT_BYTES=1048576
```

`MODEL` is the normal conversation model. `SESSION_TITLE_MODEL` is only used when `SESSION_AUTO_TITLE=1`; it is a secondary model for generating short title metadata. Title generation is off by default.

API keys can come from:

```sh
export OPENROUTER_API_KEY=...
```

or from config:

```text
OPENROUTER_API_KEY=...
```

or from the encrypted GPG key file where supported:

```text
$XDG_CONFIG_HOME/ttychatter/openrouter/api-key.gpg
```

The programs also read the older `~/.config/ttychatter/config` and `~/.config/ttychatter/api-key.gpg` locations as compatibility fallbacks when the provider-specific files are absent.

## XDG paths

Default shared paths:

```text
config:      $XDG_CONFIG_HOME/ttychatter/openrouter/config
api key gpg: $XDG_CONFIG_HOME/ttychatter/openrouter/api-key.gpg
favorites:   $XDG_CONFIG_HOME/ttychatter/openrouter/model-favorites
models:      $XDG_CACHE_HOME/ttychatter/openrouter/models.json
sessions:    $XDG_DATA_HOME/ttychatter/openrouter/sessions/
attachments: $XDG_DATA_HOME/ttychatter/openrouter/attachments/
```

Fallbacks when XDG variables are unset:

```text
config:      ~/.config/ttychatter/openrouter/config
api key gpg: ~/.config/ttychatter/openrouter/api-key.gpg
favorites:   ~/.config/ttychatter/openrouter/model-favorites
models:      ~/.cache/ttychatter/openrouter/models.json
sessions:    ~/.local/share/ttychatter/openrouter/sessions/
attachments: ~/.local/share/ttychatter/openrouter/attachments/
```

The four implementations are intended to be cross-compatible. A session created by one implementation should appear in `--list` and be resumable or searchable from the others.

## Basic use

Start a new interactive session:

```sh
ttychatter
```

Explicit interactive mode:

```sh
ttychatter -i
ttychatter --interactive
```

Send a text file once and auto-save the session:

```sh
ttychatter input.txt
```

Send a text file once and choose the output log:

```sh
ttychatter input.txt output.log
```

Send direct prompt text:

```sh
ttychatter --prompt "Explain this error" 
```

Read stdin as one batch prompt:

```sh
echo "Summarize this" | ttychatter
```

Use a named session:

```sh
ttychatter --session notes --prompt "Continue from these notes"
ttychatter -i notes
```

Force a new timestamped session:

```sh
ttychatter --new
```

## Sessions and titles

Automatic session filenames stay mechanical:

```text
session-YYYY-MM-DD-HH-MM-SS.log
session-YYYY-MM-DD-HH-MM-SS-001.log
```

The friendly title is metadata inside the log:

```text
[12:34:56] system: session-title: Research Notes
```

List sessions:

```sh
ttychatter --list
```

Retitle a session without renaming the file:

```sh
ttychatter --rename-session session-2026-06-04-12-00-00 "Research Notes"
```

Inside interactive mode:

```text
:rename Research Notes
:list
```

New logs begin with the project pre-chat line:

```text
Everyone is encouraged to get the cruelty-free vegetarian alternatives and remember the bloodguilt curse from the Bible... http://bloodguiltcurse.net
```

That line is visible to the user and stored in the log, but session context rebuilding skips it so it is not replayed as conversation memory.

## Models

Refresh the model cache:

```sh
ttychatter --update-models
```

List cached models:

```sh
ttychatter --models
```

Useful filters:

```sh
ttychatter --models --model-type free
ttychatter --models --min-input-tokens 100000
ttychatter --models --min-output-tokens 16000
ttychatter --models --show-preview
ttychatter --models --hide-preview
ttychatter --models --favorites-only
```

Select or test models:

```sh
ttychatter --select-model --save
ttychatter --test-model openrouter/auto
ttychatter --test-model openrouter/auto --save
```

Favorites:

```sh
ttychatter --favorite-model openrouter/auto
ttychatter --favorites
ttychatter --unfavorite-model openrouter/auto
```

## Attachments

Attach files to a prompt where the implementation and selected model support it:

```sh
ttychatter --attach notes.txt --prompt "Use this file"
ttychatter --attach image.png --prompt "Describe this image"
```

Interactive command where available:

```text
:attach notes.txt
:attachments
:clear-attachments
```

Large response code blocks can be extracted into the shared attachment directory. The threshold is controlled by:

```text
CODE_ATTACHMENT_MIN_LINES=5
```

## Search, turns, branching, and export

Search sessions:

```sh
ttychatter --search "kernel panic" --all-sessions
```

Show numbered turns:

```sh
ttychatter --turns notes
```

Branch after a turn:

```sh
ttychatter --branch notes 4
```

Edit a previous user turn and branch from there where supported:

```sh
ttychatter --edit-turn notes 3
```

Export:

```sh
ttychatter --export notes --format markdown --output notes.md
ttychatter --export notes --format text
ttychatter --export notes --format json
```

The Bash-only edition does not claim full edit-turn or JSON export support. Use the C or Python-helper client for those.

## Streaming and status

Streaming shows assistant output as it arrives:

```sh
ttychatter --stream --prompt "Write a short example"
```

Disable streaming:

```sh
ttychatter --no-stream
```

Status/progress updates report local client work:

```sh
ttychatter --status --prompt "Summarize this file"
```

Config equivalents:

```text
STREAM=1
STATUS_UPDATES=1
```

Interactive commands where available:

```text
:stream on
:stream off
:status on
:status off
:progress on
:progress off
```

Status messages are not hidden chain-of-thought. They are local progress reports such as loading context, preparing a request, waiting for the provider, and saving the log.

## Voice hooks

Voice features are external command hooks. ttychatter does not embed a microphone or speaker stack.

Example transcription hook:

```text
VOICE_INPUT_CMD=whisper-cli -m ~/models/ggml-base.en.bin -f %f -otxt -of -
```

Example TTS hook.  Without `%f`, ttychatter pipes the assistant reply to the command's standard input.  With `%f`, ttychatter writes the reply to a temporary text file and substitutes that path:

```text
VOICE_OUTPUT_CMD=your-tts-command
```

Command line:

```sh
ttychatter --voice-input speech.wav
ttychatter --transcribe speech.wav    # alias for --voice-input, not a transcript-only mode
ttychatter --speak --prompt "Read this response aloud"
```

Interactive commands where available:

```text
:voice speech.wav
:transcribe speech.wav
:speak on
:speak off
:speak last
```

## Runtime colon commands

Interactive line-oriented clients support local commands beginning with `:`. Common commands include:

```text
:help
:rename TITLE
:list
:turns
:branch N
:fork N
:edit N
:edit-turn N
:models
:update-models
:select-model
:test-model MODEL
:favorite MODEL
:unfavorite MODEL
:attach FILE
:memory
:search TEXT
:search-all TEXT
:stream on|off
:status on|off
:progress on|off
:voice FILE
:transcribe FILE
:speak on|off|last
:config
:set KEY VALUE
:unset KEY
:credits
:quit
```

The exact set varies by implementation. Use `:help` inside the running client.

## Implementation parity

Expected parity model:

```text
C                 reference implementation; full CLI feature target
Bash+Python       close line-oriented parity with C
Bash-only         dependency-light; scaled down for shell limitations
ncurses           interactive TUI; batch-only features are intentionally limited
```

High-level matrix:

```text
feature                  C       python  bash    ncurses
interactive chat          full    full    full    full
batch prompt              full    full    full    limited
XDG shared paths          full    full    full    full
session titles            full    full    full    full
AI title metadata         full    full    partial  config/UI support
metadata rename           full    full    full    full
models/cache/favorites    full    full    partial  full
attachments               full    full    partial  full
session search            full    full    simple   limited
turn listing              full    full    full     full
branch from turn          full    full    full     full
edit-turn branch          full    full    no       no
export text/markdown      full    full    full     full
export JSON               full    full    no       full
streaming                 full    full    plumbing TUI-dependent
status/progress           full    full    full     status bar
voice hooks               full    full    plumbing external hooks
mouse support             no      no      no       yes
```

`partial` means the feature exists in a reduced form or depends on optional tools.

## Diagnostics

Run local diagnostics:

```sh
ttychatter --doctor
```

Show config summary:

```sh
ttychatter --config
```

Use demo mode for local testing without an API key:

```sh
ttychatter --demo --prompt "hello"
ttychatter --demo --update-models
ttychatter --demo --models
```

## Project discipline

For future changes:

- Update `--help`, man pages, and this README together.
- Keep user-visible settings configurable through config keys and CLI flags.
- Add interactive colon commands when the feature makes sense in an interactive client.
- Keep session logs append-oriented; do not silently mutate history.
- Prefer metadata retitling over filename renaming.
- Keep XDG paths shared across implementations.
- Do not print API keys in diagnostics.
- Do not describe status updates as model thoughts or hidden reasoning.
