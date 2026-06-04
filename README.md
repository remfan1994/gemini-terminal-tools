# ttychatter

`ttychatter` is a terminal-first OpenRouter chat client with plain-text
sessions, XDG-compatible storage, model browsing, attachments, search,
branching, export, optional streaming, and optional external voice hooks.

The project ships four related front ends that share the same config and
session conventions:

```text
implementation         role
--------------         ----
ttychatter             native C command-line client; current feature leader
ttychatter.python      Bash + Python 3 helper client; closest script parity
ttychatter.bash        dependency-light Bash-only client; scaled-down parity
ttychatter.ncurses     Python ncurses full-screen TUI
```

The design goal is:

```text
storage = plain files
configuration = simple KEY=VALUE text
memory = transcript replay + optional metadata
interface = terminal-native, scriptable, recoverable
```

No browser, SDK project, database server, or daemon is required.

## Requirements

Core command-line requirements vary by implementation.

```text
implementation         required runtime
--------------         ----------------
ttychatter             C compiler for building, libcurl, json-c
ttychatter.python      bash, curl, python3, sed/awk/grep/coreutils
ttychatter.bash        bash, curl, sed/awk/grep/coreutils
ttychatter.ncurses     python3 with curses support
```

Optional helpers:

```text
gpg       encrypted API-key storage
base64    binary attachment handling in shell editions
file      better MIME detection
editor    EDITOR/VISUAL for editor-prompt and edit-turn workflows
whisper   or another command for speech-to-text hooks
piper     or another command for text-to-speech hooks
```

## Quick start

Build the C client:

```sh
make
./ttychatter --help
```

Run one of the script editions directly:

```sh
./ttychatter.python --help
./ttychatter.bash --help
./ttychatter.ncurses --help
```

Set an API key by environment:

```sh
export OPENROUTER_API_KEY='sk-or-...'
```

or store it through the client:

```sh
ttychatter --set-api-key
ttychatter.python --set-api-key
ttychatter.bash --set-api-key
```

Run a new interactive session:

```sh
ttychatter
```

Send a file once and auto-save the session:

```sh
ttychatter input.txt
```

Send a file once and write an explicit log:

```sh
ttychatter input.txt output.log
```

Send direct prompt text:

```sh
ttychatter --prompt 'Summarize this repository layout.'
```

Read stdin as one prompt:

```sh
printf 'Explain this error log.\n' | ttychatter
```

Use demo mode without network access:

```sh
ttychatter --demo --prompt 'test prompt'
ttychatter.python --demo --prompt 'test prompt'
ttychatter.bash --demo --prompt 'test prompt'
```

## XDG storage contract

All implementations should use the same default storage layout:

```text
config:
  ${XDG_CONFIG_HOME:-~/.config}/ttychatter/openrouter/config

encrypted API key:
  ${XDG_CONFIG_HOME:-~/.config}/ttychatter/openrouter/api-key.gpg

model favorites:
  ${XDG_CONFIG_HOME:-~/.config}/ttychatter/openrouter/model-favorites

sessions:
  ${XDG_DATA_HOME:-~/.local/share}/ttychatter/openrouter/sessions

attachments:
  ${XDG_DATA_HOME:-~/.local/share}/ttychatter/openrouter/attachments

model cache:
  ${XDG_CACHE_HOME:-~/.cache}/ttychatter/openrouter/models.json
```

The important compatibility rule is:

```text
create in one edition -> list/resume/retitle/branch/export in another edition
```

For example, a session created by `ttychatter.bash` should appear in
`ttychatter --list` and `ttychatter.ncurses --list` because the session log and
metadata format are shared.

## Configuration

The config file is simple `KEY=VALUE` text.

```text
MODEL=openrouter/auto
CONTEXT_TURNS=12
SESSION_AUTO_TITLE=0
SESSION_TITLE_MODEL=openrouter/auto
SESSION_TITLE_MAX_WORDS=8
STREAM=0
STATUS_UPDATES=0
THEME=default
SEND_INPUT=ctrl_d
CODE_ATTACHMENT_MIN_LINES=5
VOICE_INPUT_CMD=
VOICE_OUTPUT_CMD=
VOICE_OUTPUT=0
```

Control planes follow this pattern where practical:

```text
persistent default = config key
one-shot override  = command-line option
runtime control    = :colon-command in interactive mode
```

Examples:

```text
config:       STREAM=1
CLI:          --stream / --no-stream
interactive:  :stream on / :stream off
```

```text
config:       STATUS_UPDATES=1
CLI:          --status / --no-status / --progress / --no-progress
interactive:  :status on / :status off
```

## Model workflow

List models:

```sh
ttychatter --models
ttychatter --models --model-type free
ttychatter --models --min-input-tokens 100000
ttychatter --models --min-output-tokens 16000
```

Refresh the cache:

```sh
ttychatter --update-models
```

Test a model:

```sh
ttychatter --test-model openrouter/auto
```

Save a model:

```sh
ttychatter --test-model openrouter/auto --save
```

Favorites:

```sh
ttychatter --favorite-model openrouter/auto
ttychatter --favorites
ttychatter --unfavorite-model openrouter/auto
```

The old autoscan model workflow is intentionally not the current workflow.
The user now lists, filters, selects, tests, and saves deliberately.

## Sessions and logs

Automatic sessions use timestamped filenames under the shared sessions
directory:

```text
session-YYYY-MM-DD-HH-MM-SS.log
session-YYYY-MM-DD-HH-MM-SS-001.log
```

The filename is a stable storage identity. Human-friendly names are metadata.
`--list` displays the metadata title when present.

Retitle a session without renaming the file:

```sh
ttychatter --rename-session session-name 'Friendly Title'
```

In interactive mode:

```text
:rename Friendly Title
```

Optional AI-generated session titles are off by default:

```text
SESSION_AUTO_TITLE=0
```

Advanced users can enable a separate title-generation model:

```text
SESSION_AUTO_TITLE=1
SESSION_TITLE_MODEL=openrouter/auto
SESSION_TITLE_MAX_WORDS=8
```

The conversation model remains `MODEL`. The title model is only for generating
session-title metadata.

New logs begin with the project pre-chat line. Context rebuilding skips that
line and other system notice material so it is visible in logs without being
replayed as conversation memory.

## Memory model

The model does not remember on its own. ttychatter reconstructs context from
saved data.

```text
next request context = selected transcript turns + optional snapshots + config policy
```

The main memory control is:

```text
CONTEXT_TURNS=12
```

Plain-text logs remain recoverable even if ttychatter is not available.

## Searching sessions

List sessions:

```sh
ttychatter --list
```

Search logs:

```sh
ttychatter --search 'phrase'
ttychatter.python --search 'phrase' --all-sessions
```

The search surface includes filenames, title metadata, and transcript text.

## Branching and editing

List message turns:

```sh
ttychatter --turns session-name
```

Create a branch through a turn number:

```sh
ttychatter --branch session-name 4
```

Edit a user turn and branch from the edited message:

```sh
ttychatter --edit-turn session-name 3
```

The rule is append-only history discipline:

```text
editing old history -> create a branch
never silently rewrite later conversation turns
```

Bash-only has basic branch/export support but intentionally does not implement
full `--edit-turn`. Use the C client or `ttychatter.python` for that workflow.
The ncurses edition exposes branch/export from the TUI and CLI, while full
edit-turn UI remains delegated to C/Python.

## Export

Export a session:

```sh
ttychatter --export session-name --format markdown
ttychatter --export session-name --format text
ttychatter --export session-name --format json
```

Write to a file:

```sh
ttychatter --export session-name --format markdown --output session.md
```

Bash-only supports text and markdown export. C, Python-helper, and ncurses
support text, markdown, and JSON.

## Attachments

Attach a file to the next request:

```sh
ttychatter --attach notes.txt --prompt 'Use this file.'
```

Interactive command:

```text
:attach notes.txt
```

Generated code blocks can be extracted into attachment files. The threshold is
controlled by:

```text
CODE_ATTACHMENT_MIN_LINES=5
```

Model support for non-text attachment types depends on the selected OpenRouter
model and provider behavior.

## Streaming and status updates

Streaming means visible assistant output is printed as it arrives:

```sh
ttychatter --stream --prompt 'Write a short explanation.'
```

Disable it:

```sh
ttychatter --no-stream
```

Status/progress messages are local client-side reports, not hidden model
thoughts:

```sh
ttychatter --status --prompt 'Refactor this function.'
ttychatter --progress --prompt 'Refactor this function.'
```

Interactive commands:

```text
:stream on
:stream off
:status on
:status off
:progress on
:progress off
```

Only provider-returned visible content should be shown as model content.
The client should not claim to reveal hidden chain-of-thought.

## Voice hooks

Voice features are external command hooks. ttychatter does not try to own the
microphone or speaker stack.

Speech-to-text command config:

```text
VOICE_INPUT_CMD=whisper-cli %f
```

Text-to-speech command config:

```text
VOICE_OUTPUT_CMD=piper --model voice.onnx --output_file %f
VOICE_OUTPUT=1
```

Use a voice input file:

```sh
ttychatter --voice-input input.wav
```

Transcribe only:

```sh
ttychatter --transcribe input.wav
```

Speak assistant output:

```sh
ttychatter --speak --prompt 'Say one sentence.'
```

Interactive commands:

```text
:voice input.wav
:speak on
:speak off
:speak last
```

`%f` is replaced with the temporary input/output file path where supported.

## Runtime colon commands

Interactive command-line editions accept local commands beginning with `:`.
Common commands include:

```text
:help
:rename TITLE
:turns
:branch N
:edit N
:stream on|off
:status on|off
:models
:favorites
:favorite MODEL
:unfavorite MODEL
:config
:set KEY VALUE
:unset KEY
:attach FILE
:search TEXT
:search-all TEXT
:voice FILE
:speak on|off|last
```

Use `\:` at the beginning of an input if the message itself should start with a
literal colon.

## ncurses edition

`ttychatter.ncurses` is the full-screen TUI. It shares the session/config
layout with the other editions and adds screen navigation, model/session
browsers, editor integration, and mouse-aware TUI behavior where the terminal
supports it.

Useful noninteractive ncurses commands:

```sh
ttychatter.ncurses --list
ttychatter.ncurses --turns session-name
ttychatter.ncurses --branch session-name 4
ttychatter.ncurses --export session-name --format markdown
```

## Diagnostics

Run diagnostics:

```sh
ttychatter --doctor
ttychatter.python --doctor
ttychatter.bash --doctor
ttychatter.ncurses --doctor
```

Diagnostics should redact API-key material. They should report paths, cache
state, editor availability, terminal readiness, and network/API readiness where
applicable.

## Compatibility matrix

```text
feature                         C       python   bash     ncurses
-------                         -       ------   ----     -------
shared XDG config/session dirs  full    full     full     full
plain text session logs         full    full     full     full
title metadata in --list        full    full     full     full
metadata retitle                full    full     full     full
optional AI title metadata      full    full     basic    config/metadata
batch prompt/input/output       full    full     full     not primary
interactive chat                full    full     full     full-screen
model list/cache/filter         full    full     basic    full
favorites                       full    full     full     full
attachments                     full    full     basic    full
session search                  full    full     basic    partial/basic
streaming                       full    full     option   TUI-dependent
status/progress                 full    full     full     status bar
turn listing                    full    full     full     full
branching                       full    full     basic    full
edit-turn branching             full    full     unsupported unsupported
export text/markdown/json       full    full     text/md  full
voice input/output hooks        full    full     plumbing TUI-dependent
```

`basic`, `partial`, and `plumbing` are intentional scale-downs, not abandoned
features. The Bash-only edition preserves low dependency cost; the ncurses
edition prioritizes interactive TUI workflows.

## Development discipline

When a user-visible feature changes, update these surfaces together:

```text
code
--help text
man page
README
smoke tests or make check
```

For shared behavior, test at least this matrix:

```text
create in bash    -> list in C/Python/ncurses
create in Python  -> list in C/Bash/ncurses
create in C       -> list in Bash/Python/ncurses
retitle in one    -> list in all
branch in one     -> turns/export in another
XDG env override  -> no files written to default HOME paths
```

## License
Insha-Allah License Agreement (IALA) found at: https://remfan1994.github.io/insha-allah-license-agreement/index.html
Partial license follows...

Insha-Allah License Agreement
Effective immediately upon acknowledgment by the Licensee.

0 SHORT VERSION APPROPRIATE FOR THE MUSLIM

0.1 Allah is the grantor and revoker of all rights. I, the Licensor, am relieved of liability by mentioning the name of Allah, particularly in connection with Its will [INSHA-ALLAH LICENSE AGREEMENT] to the MOST MERCY. Responsibility for your actions with this content rests with Allah and yourself. Beyond this, there are no restrictions. By reading this license, you acknowledge that I am not liable, that your powers and limitations come from Allah, and that Allah is the guardian over myself, you, and all whom your actions affect.

