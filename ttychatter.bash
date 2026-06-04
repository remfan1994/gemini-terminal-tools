#!/usr/bin/env bash

# ==========================================================
# ttychatter.bash (bash-only edition)
# ----------------------------------------------------------
# Dependency-light OpenRouter terminal chat client.
#
# PROJECT ROLE
#   This edition exists for users who want the project goal -- a friendly,
#   usable terminal AI chat client -- without depending on Python 3. It is not
#   a developer curl example and it is not a one-shot wrapper. It is an actual
#   end-user interactive chat program, scaled to the tools likely present on
#   old or constrained systems that can still run curl over HTTPS.
#
# REQUIRED TOOLS
#   bash, curl, sed, awk, grep, tr, printf, mktemp
#
# OPTIONAL TOOLS
#   base64  - needed for binary/media file attachment support
#   file    - improves MIME type detection
#
# RUNTIME COMMAND MODE
#   Bash-only cannot offer ncurses function-key screens, so it now provides
#   colon commands inside the chat loop. A submitted input beginning with : is
#   handled locally as a client command instead of being sent to OpenRouter.
#
# DESIGN LIMIT
#   JSON is the hard part. Without Python or jq, JSON parsing is best-effort.
#   The script still tries to provide sessions, model listing, model testing,
#   memory, logs, code-block extraction, and attachment support.
# ==========================================================


# ==========================================================
# MAINTAINER COMMENTARY: BASH-ONLY PROJECT CONTRACT
# ----------------------------------------------------------
# This file is not a toy curl example.  It is the dependency-light end-user
# client in the ttychatter family.  The mission is still the same:
# provide a friendly terminal chat client for AI productivity without forcing
# a browser or a large runtime stack.
#
# The difference from bash/python3/ttychatter.python is dependency
# posture.  This edition must avoid Python.  It can rely on curl and ordinary
# shell-era tools because any machine that can realistically talk to OpenRouter over
# HTTPS already needs a working curl/TLS stack, but it should not assume Python
# 3 is installed.  Optional helpers such as jq, base64, and file may improve
# behavior, but the program should remain usable without them where possible.
#
# This creates maintenance tension: OpenRouter speaks JSON, and shell is not good
# at JSON.  The comments in this file are therefore especially important.  When
# you see a sed/awk parser, understand that it is a compatibility compromise,
# not a claim that regex is the ideal JSON engine.  If jq is available, use it.
# If not, the fallback should be conservative, human-readable, and honest about
# limitations.
#
# The bash-only edition should stay as featureful as it can be while remaining
# light on dependencies.  Do not intentionally reduce it to a one-shot prompt
# sender.  Interactive chat, sessions, config, logs, model selection, memory,
# and attachments all belong here if they can be implemented responsibly.
# ==========================================================

set -o pipefail

PROGRAM="ttychatter.bash"
VERSION="0.14.0-bash-only-parity"
CONFIG_DIR="$HOME/.config/ttychatter/openrouter"
CONFIG_FILE="$CONFIG_DIR/config"
MODEL_CACHE_FILE="$CONFIG_DIR/models-cache.json"
API_KEY_GPG_FILE="$CONFIG_DIR/api-key.gpg"
CONFIG_READ_FILE="$CONFIG_FILE"
SESSION_DIR="$HOME/.local/share/ttychatter/openrouter/sessions"
ATTACHMENT_DIR=""
ATTACHMENT_DIR_CONFIGURED=0
MODEL=""
MODEL_TYPE_FILTER="all"
MODEL_SORT_ORDER="name"
MODEL_MIN_INPUT_TOKENS=0
MODEL_MIN_OUTPUT_TOKENS=0
MODEL_FILTER_REQUIRE_TOKENS=0
MODEL_FILTER_HIDE_PREVIEW=0
MODEL_FAVORITES_FILE="$CONFIG_DIR/model-favorites.txt"
THEME="default"
SEND_INPUT="ctrl_d"
CODE_ATTACHMENT_MIN_LINES=5
FALLBACK_MODEL="openrouter/auto"
CONTEXT_TURNS=8
OPENROUTER_API_KEY="${OPENROUTER_API_KEY:-${TTYCHATTER_API_KEY:-}}"
MODEL_TEST_PROMPT="Please reply with a short sentence confirming this model is available for text generation."
DEMO_MODE=0
STARTUP_NOTICE=1
PROJECT_LEAD_NAME="remfan1994"
PROJECT_LEAD_NOTICE="Everyone is encouraged to get the cruelty-free vegetarian alternatives and remember the bloodguilt curse from the Bible... http://bloodguiltcurse.net"
TC_PRECHAT_LINE="$PROJECT_LEAD_NOTICE"
USER_NAME="$(whoami 2>/dev/null || printf user)"
CONTEXT_BUFFER=()
ATTACH_FILES=()
ACTION="chat"
SESSION_NAME=""
RESUME=0
SAVE=0
TEST_MODEL=""
SET_KEY=""
SET_VALUE=""
UNSET_KEY=""
RUNTIME_SEND_OVERRIDE=""
LOOPBACK=0
LOOPBACK_FILE=""
SESSION_AUTO_TITLE=0
SESSION_TITLE_MODEL="openrouter/auto"
SESSION_TITLE_MAX_WORDS=8
STREAM=0
STATUS_UPDATES=0
VOICE_INPUT_CMD=""
VOICE_OUTPUT_CMD=""
VOICE_OUTPUT=0
PROMPT_TEXT=""
INPUT_FILE=""
OUTPUT_PATH=""
INTERACTIVE=0
FORCE_NEW=0
TURN_SESSION=""
TURN_NUMBER=""
EXPORT_SESSION=""
EXPORT_FORMAT="markdown"
VOICE_INPUT_PATH=""
SPEAK_OVERRIDE=""
SEARCH_QUERY=""
SEARCH_ALL=0

# ==========================================================
# BASIC HELPERS
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function timestamp
# Produces timestamps for prompts and logs. The log reader expects this
# stable shape.
# ----------------------------------------------------------
timestamp() { date +"%H:%M:%S"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function trim
# trim is part of the dependency-light client behavior. Changes should
# preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
trim() { printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function truthy
# truthy is part of the dependency-light client behavior. Changes should
# preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
truthy() { case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in 1|yes|y|true|on) return 0;; *) return 1;; esac; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function normalize_model_id
# normalize_model_id is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
normalize_model_id() { case "$1" in models/*) printf '%s\n' "${1#models/}";; *) printf '%s\n' "$1";; esac; }
sanitize_session_name() {
  local cleaned
  cleaned="$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_' | sed 's/^[_ .-]*//; s/[_ .-]*$//')"
  [ -n "$cleaned" ] || cleaned="session"
  printf '%s\n' "$cleaned"
}
effective_model() { if [ -n "$MODEL" ]; then normalize_model_id "$MODEL"; else printf '%s
' "$FALLBACK_MODEL"; fi; }
model_label() { if [ -n "$MODEL" ]; then normalize_model_id "$MODEL"; else printf '%s (fallback; no MODEL configured)
' "$FALLBACK_MODEL"; fi; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function json_escape_stream
# json_escape_stream is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
json_escape_stream() { awk 'BEGIN{first=1;ORS=""}{gsub(/\\/,"\\\\");gsub(/\"/,"\\\"");gsub(/\r/,"");if(!first)printf "\\n";printf "%s",$0;first=0}'; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function json_unescape_basic
# json_unescape_basic is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
json_unescape_basic() { sed 's/\\n/\
/g; s/\\t/	/g; s/\\"/"/g; s/\\\\/\\/g'; }

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function decrypt_gpg_key_file
# Optional GPG key loading keeps encrypted API-key storage available even in
# the dependency-light Bash edition. This function intentionally invokes the
# user's normal gpg command instead of trying to implement cryptography in
# shell. If gpg is missing or decryption fails, the function simply returns
# failure and the normal API-key error path explains what is missing.
# ----------------------------------------------------------
decrypt_gpg_key_file() {
  local path="$1"
  [ -f "$path" ] || return 1
  command -v gpg >/dev/null 2>&1 || return 1
  gpg --quiet --decrypt "$path" 2>/dev/null | tr -d '\r' | sed '/^[[:space:]]*$/d' | head -n 1
}


# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function encrypt_gpg_key_file
# Bash-only encrypted API-key storage. This function uses the external gpg
# command as the cryptographic engine because implementing encryption in Bash
# would be both insecure and contrary to the purpose of using the user's normal
# Unix tools. The temporary plaintext file is chmod 0600 where possible and is
# always removed after gpg returns. If gpg is unavailable or fails, callers must
# not silently save plaintext; they should leave the key runtime-only or ask the
# user explicitly.
# ----------------------------------------------------------
encrypt_gpg_key_file() {
  local path="$1"
  local secret="$2"
  local tmp
  local status

  command -v gpg >/dev/null 2>&1 || { printf '%s\n' "gpg not found; encrypted API-key storage unavailable" >&2; return 1; }
  mkdir -p "$(dirname "$path")"
  tmp="$(mktemp "${TMPDIR:-/tmp}/ttychatter-api-key.XXXXXX")" || return 1
  chmod 600 "$tmp" 2>/dev/null || true
  printf '%s\n' "$secret" > "$tmp"

  gpg --symmetric --armor --yes --output "$path" "$tmp"
  status=$?
  rm -f "$tmp"
  if [ "$status" -eq 0 ]; then
    chmod 600 "$path" 2>/dev/null || true
    return 0
  fi
  return "$status"
}

# ==========================================================
# CONFIG FILE
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function load_config
# Reads the shared config format using only shell tools. Keep it simple so
# old systems and future C code can understand the same settings.
# ----------------------------------------------------------
load_config() {
  [ -f "$CONFIG_READ_FILE" ] || return 0
  while IFS= read -r line || [ -n "$line" ]; do
    line="$(trim "$line")"
    case "$line" in ''|\#*) continue ;; esac
    case "$line" in *=*) key="$(trim "${line%%=*}")"; value="$(trim "${line#*=}")" ;; *) continue ;; esac
    case "$key" in
      MODEL) MODEL="$(normalize_model_id "$value")" ;;
      CONTEXT_TURNS|HISTORY_LIMIT) CONTEXT_TURNS="$value" ;;
      OPENROUTER_API_KEY|API_KEY) OPENROUTER_API_KEY="${OPENROUTER_API_KEY:-$value}" ;;
      API_KEY_GPG_FILE|OPENROUTER_API_KEY_GPG_FILE) API_KEY_GPG_FILE="$value" ;;
      SESSION_DIR) SESSION_DIR="${value/#\~/$HOME}" ;;
      ATTACHMENT_DIR) ATTACHMENT_DIR="${value/#\~/$HOME}"; ATTACHMENT_DIR_CONFIGURED=1 ;;
      MODEL_TEST_PROMPT) [ -n "$value" ] && MODEL_TEST_PROMPT="$value" ;;
      MODEL_TYPE_FILTER) MODEL_TYPE_FILTER="$value" ;;
      MODEL_SORT_ORDER) MODEL_SORT_ORDER="$value" ;;
      MODEL_MIN_INPUT_TOKENS) MODEL_MIN_INPUT_TOKENS="$value" ;;
      MODEL_MIN_OUTPUT_TOKENS) MODEL_MIN_OUTPUT_TOKENS="$value" ;;
      MODEL_FILTER_REQUIRE_TOKENS) MODEL_FILTER_REQUIRE_TOKENS="$value" ;;
      MODEL_FILTER_HIDE_PREVIEW) MODEL_FILTER_HIDE_PREVIEW="$value" ;;
      MODEL_FAVORITES_FILE) MODEL_FAVORITES_FILE="${value/#\~/$HOME}" ;;
      THEME) THEME="$value" ;;
      SEND_INPUT) SEND_INPUT="$value" ;;
      CODE_ATTACHMENT_MIN_LINES) CODE_ATTACHMENT_MIN_LINES="$value" ;;
      DEMO_MODE) DEMO_MODE="$value" ;;
      STARTUP_NOTICE) STARTUP_NOTICE="$value" ;;
      SESSION_AUTO_TITLE) SESSION_AUTO_TITLE="$value" ;;
      SESSION_TITLE_MODEL) SESSION_TITLE_MODEL="$(normalize_model_id "$value")" ;;
      SESSION_TITLE_MAX_WORDS) SESSION_TITLE_MAX_WORDS="$value" ;;
      STREAM) STREAM="$value" ;;
      STATUS_UPDATES|PROGRESS_UPDATES) STATUS_UPDATES="$value" ;;
      VOICE_INPUT_CMD|TRANSCRIBE_CMD) VOICE_INPUT_CMD="$value" ;;
      VOICE_OUTPUT_CMD|TTS_CMD) VOICE_OUTPUT_CMD="$value" ;;
      VOICE_OUTPUT|TTS) VOICE_OUTPUT="$value" ;;
    esac
  done < "$CONFIG_READ_FILE"
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function save_config_value
# Updates one KEY=VALUE setting while preserving the rest of the config
# file. This is the shell-only equivalent of the richer config writers
# elsewhere.
# ----------------------------------------------------------
save_config_value() {
  local key="$1" value="$2" tmp
  mkdir -p "$CONFIG_DIR"
  tmp="$(mktemp "${CONFIG_FILE}.tmp.XXXXXX")"
  if [ -f "$CONFIG_FILE" ]; then
    awk -v k="$key" -v v="$value" 'BEGIN{done=0} $0 ~ "^" k "=" {print k "=" v; done=1; next} {print} END{if(!done) print k "=" v}' "$CONFIG_FILE" > "$tmp"
  else
    printf '%s=%s\n' "$key" "$value" > "$tmp"
  fi
  mv "$tmp" "$CONFIG_FILE"
  chmod 600 "$CONFIG_FILE" 2>/dev/null || true
}


# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function remove_config_key
# Deletes one setting from the config file.  This is deliberately simple and
# line-oriented because the bash-only edition must stay usable without Python
# or structured config parsers.  It is used by --unset and by runtime colon
# commands such as :unset and :forget-api-key.
# ----------------------------------------------------------
remove_config_key() {
  local key="$1" tmp
  [ -f "$CONFIG_FILE" ] || return 0
  tmp="$(mktemp "${CONFIG_FILE}.tmp.XXXXXX")"
  awk -v k="$key" '$0 !~ "^" k "=" { print }' "$CONFIG_FILE" > "$tmp"
  mv "$tmp" "$CONFIG_FILE"
  chmod 600 "$CONFIG_FILE" 2>/dev/null || true
}


# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function set_api_key_command
# Provides the dependency-light client with the same security choice as the
# ncurses GUI: runtime-only by default, optional GPG-encrypted storage when gpg
# exists, and plaintext storage only after a clear warning.  This is deliberately
# interactive because secrets should not be written because of an accidental
# command-line flag.  The bash-only edition cannot show a checkbox, but the
# sequence of prompts mirrors the GUI decision points.
# ----------------------------------------------------------
set_api_key_command() {
  local key encrypt_answer answer gpg_path

  printf 'Paste OpenRouter API key: '
  IFS= read -r -s key
  printf '\n'
  [ -n "$key" ] || { printf '%s\n' "No key entered."; return 1; }
  OPENROUTER_API_KEY="$key"

  if command -v gpg >/dev/null 2>&1; then
    printf 'Encrypt with gpg? [y/N] '
    IFS= read -r encrypt_answer
    case "$encrypt_answer" in
      y|Y|yes|YES)
        gpg_path="$API_KEY_GPG_FILE"
        if encrypt_gpg_key_file "$gpg_path" "$key"; then
          save_config_value "API_KEY_GPG_FILE" "$gpg_path"
          remove_config_key "OPENROUTER_API_KEY"
          remove_config_key "TTYCHATTER_API_KEY"
          printf 'saved encrypted API key to %s\n' "$gpg_path"
          return 0
        fi
        printf '%s\n' "Encrypted save failed; key is active for this run only."
        return 1
        ;;
    esac
  else
    printf '%s\n' "gpg not found; encrypted API-key storage unavailable."
  fi

  printf '%s\n' "WARNING: key stored in plaintext if saved to config."
  printf 'Save plaintext OPENROUTER_API_KEY to %s? [y/N] ' "$CONFIG_FILE"
  IFS= read -r answer
  case "$answer" in
    y|Y|yes|YES)
      save_config_value "OPENROUTER_API_KEY" "$key"
      printf 'saved OPENROUTER_API_KEY to %s\n' "$CONFIG_FILE"
      ;;
    *)
      printf '%s\n' "API key active for this run only."
      ;;
  esac
}

forget_api_key_command() {
  remove_config_key OPENROUTER_API_KEY
  remove_config_key TTYCHATTER_API_KEY
  printf '%s\n' "removed plaintext API-key config entries if present"
}

load_config
if [ -z "$OPENROUTER_API_KEY" ]; then
  OPENROUTER_API_KEY="$(decrypt_gpg_key_file "$API_KEY_GPG_FILE" || true)"
fi
case "$CONTEXT_TURNS" in ''|*[!0-9]*) CONTEXT_TURNS=8 ;; esac
[ "$CONTEXT_TURNS" -lt 1 ] && CONTEXT_TURNS=8
case "$MODEL_MIN_INPUT_TOKENS" in ''|*[!0-9]*) MODEL_MIN_INPUT_TOKENS=0 ;; esac
case "$MODEL_MIN_OUTPUT_TOKENS" in ''|*[!0-9]*) MODEL_MIN_OUTPUT_TOKENS=0 ;; esac
case "$CODE_ATTACHMENT_MIN_LINES" in ''|*[!0-9]*) CODE_ATTACHMENT_MIN_LINES=5 ;; esac
[ "$CODE_ATTACHMENT_MIN_LINES" -lt 1 ] && CODE_ATTACHMENT_MIN_LINES=5
[ "$ATTACHMENT_DIR_CONFIGURED" -eq 0 ] && ATTACHMENT_DIR="$SESSION_DIR/attachments"
mkdir -p "$CONFIG_DIR" "$SESSION_DIR" "$ATTACHMENT_DIR"

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function require_api_key
# require_api_key is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
require_api_key() {
  if truthy "$DEMO_MODE"; then
    return 0
  fi
  if [ -z "$OPENROUTER_API_KEY" ]; then
    printf '%s\n' "ERROR: OPENROUTER_API_KEY not set." >&2
    printf '%s\n' "Set it with: export OPENROUTER_API_KEY=your_key" >&2
    printf '%s\n' "or save it in: $CONFIG_FILE"
    return 1
  fi
  return 0
}

# ==========================================================
# HELP / CREDITS
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function help_menu
# Primary built-in documentation for users who may not have installed man
# pages.
# ----------------------------------------------------------
help_menu() {
  cat <<EOF
$PROGRAM (bash-only edition) - dependency-light OpenRouter terminal chat client

USAGE
    $PROGRAM
    $PROGRAM input.txt
    $PROGRAM input.txt output.log
    $PROGRAM -i [session]
    $PROGRAM --prompt TEXT
    $PROGRAM --input FILE [--output FILE]
    $PROGRAM --session SESSION
    $PROGRAM --resume <session>
    $PROGRAM --list
    $PROGRAM --rename-session SESSION TITLE
    $PROGRAM --search QUERY [--all-sessions]
    $PROGRAM --models [--model-type TYPE]
    $PROGRAM --favorites
    $PROGRAM --favorite-model MODEL
    $PROGRAM --unfavorite-model MODEL
    $PROGRAM --config
    $PROGRAM --set KEY VALUE
    $PROGRAM --unset KEY
    $PROGRAM --theme THEME
    $PROGRAM --send-input MODE
    $PROGRAM --code-attachment-min-lines N
    $PROGRAM --update-models
    $PROGRAM --select-model [--save]
    $PROGRAM --test-model <model> [--save]
    $PROGRAM --set-api-key
    $PROGRAM --forget-api-key
    $PROGRAM --attach FILE [session]
    $PROGRAM --stream | --no-stream
    $PROGRAM --status | --no-status
    $PROGRAM --voice-input FILE | --transcribe FILE
    $PROGRAM --speak | --no-speak
    $PROGRAM --turns SESSION
    $PROGRAM --branch SESSION TURN
    $PROGRAM --export SESSION [--format markdown|text] [--output FILE]
    $PROGRAM --loopback [session]
    $PROGRAM --loopback-file PATH [session]
    $PROGRAM --credits
    $PROGRAM --doctor
    $PROGRAM --version
    $PROGRAM --help

GPG API KEY STORAGE
    --set-api-key offers runtime-only, GPG-encrypted, or plaintext config storage.
    If gpg is not installed, the command says "gpg not found" and skips encrypted storage.
    Plaintext saves warn before writing OPENROUTER_API_KEY to config.

LOOPBACK OUTPUT
    --loopback routes interface text to stderr and writes AI responses to stdout.
    This is for piping ttychatter output into another program.

    --loopback-file PATH appends each AI response to PATH.

INPUT
    Type or paste a message, then send according to SEND_INPUT.
    SEND_INPUT=ctrl_d keeps multiline Ctrl+D behavior.
    SEND_INPUT=enter reads one line per message.

SESSION TITLES
    Logs keep timestamped filenames. --list shows session-title metadata when present.
    --rename-session changes title metadata, not the filename.
    Optional AI title metadata is off by default: SESSION_AUTO_TITLE=0.

OPENROUTER HELP
    Model docs:      https://openrouter.ai/docs/models
    Models API:      https://openrouter.ai/docs/api-reference/models/get-models
    Chat completions: https://openrouter.ai/docs/api-reference/chat/send-chat-completion-request

RUNTIME COMMAND MODE
    During a running chat, submit a line beginning with ':' to run a local
    command instead of sending a message to the AI backend.

    Examples:
        :help
        :rename notes
        :models
        :favorites
        :favorite openrouter/auto
        :unfavorite openrouter/auto
        :select-model
        :model openrouter/auto
        :memory
        :attach /path/to/file.txt
        :editor
        :credits

MODEL FILTER ALIASES
    --routers                       Show router/free routes in model list
    --show-preview | --hide-preview Control model preview text
    --require-tokens | --allow-missing-tokens
                                    Control token metadata filtering
    --min-input-tokens N             Filter by input-token capacity when available
    --min-output-tokens N            Filter by output-token capacity when available
    --unbookmark-model MODEL         Alias for --unfavorite-model

COMPATIBILITY / TESTING
    --demo                           Use local demo responses and demo model rows
    --interactive                    Alias for -i
    --new                            Force a fresh timestamped session
    --progress | --no-progress       Aliases for --status/--no-status
    --edit-turn SESSION TURN         Not implemented in bash-only; use C or python helper

NOTES
    This edition uses Bash and basic Unix tools only. JSON parsing is
    best-effort without Python or jq, but the program remains an end-user chat
    client with sessions, memory, logs, model selection, and attachments.
EOF
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function credits
# credits is part of the dependency-light client behavior. Changes should
# preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
credits() {
  cat <<'EOF'
ttychatter
GitHub: https://github.com/remfan1994/ttychatter

Project focus:
  User-friendly and robust AI chat in the text terminal. Browsers and browser
  interfaces are not required for sending text messages and attachments back
  and forth to an AI.

Project Lead notice:
  Everyone is encouraged to get the cruelty-free vegetarian alternatives and remember the bloodguilt curse from the Bible... http://bloodguiltcurse.net
EOF
}

demo_models_json() {
  cat <<'JSON'
{"data":[{"id":"demo/auto","name":"Demo Auto Router","description":"Local demo model. No network request is made.","context_length":8192,"architecture":{"modality":"text"}},{"id":"demo/free","name":"Demo Free Route","description":"Local demo route for UI testing.","context_length":4096,"architecture":{"modality":"text"}},{"id":"demo/code-helper","name":"Demo Code Helper","description":"Local demo row for code-block and attachment testing.","context_length":32768,"architecture":{"modality":"text"}}]}
JSON
}

demo_ai_response() {
  local prompt="$1"
  printf '%s\n' "Demo mode is active; no OpenRouter request was made."
  printf 'Prompt excerpt: %s\n' "$(printf '%s' "$prompt" | tr '\n' ' ' | cut -c 1-140)"
  printf '\nShort inline code block:\n```text\nok\n```\n'
  printf '\nLong code block for attachment-threshold testing:\n```sh\necho ttychatter demo mode\necho no provider request was made\necho long code blocks should become attachments\necho done\n```\n'
}

# ==========================================================
# MODEL LIST / CACHE
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function fetch_models_json
# fetch_models_json is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
fetch_models_json() { if truthy "$DEMO_MODE"; then demo_models_json; return 0; fi; require_api_key || return 1; curl -sS --max-time 60 -H "Authorization: Bearer $OPENROUTER_API_KEY" "https://openrouter.ai/api/v1/models"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function update_models_cache
# update_models_cache is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
update_models_cache() { local json; json="$(fetch_models_json)" || return 1; printf '%s' "$json" > "$MODEL_CACHE_FILE"; printf 'updated model cache: %s\n' "$MODEL_CACHE_FILE"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function models_json_source
# models_json_source is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
models_json_source() { if truthy "$DEMO_MODE"; then demo_models_json; return 0; fi; if [ -f "$MODEL_CACHE_FILE" ]; then cat "$MODEL_CACHE_FILE"; else fetch_models_json; fi; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function parse_models_basic
# parse_models_basic is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
parse_models_basic() {
  if command -v jq >/dev/null 2>&1; then
    jq -r '
      (.data // .models // [])[]
      | . as $m
      | [
          ($m.id // $m.name // ""),
          ($m.name // $m.id // ""),
          (($m.context_length // $m.contextLength // "")|tostring),
          (($m.max_completion_tokens // $m.maxCompletionTokens // "")|tostring),
          (if (($m.id // $m.name // "") == "openrouter/auto" or ($m.id // $m.name // "") == "openrouter/free" or ($m.id // $m.name // "") == "demo/auto" or ($m.id // $m.name // "") == "demo/free" or (($m.id // $m.name // "") | test(":free$"))) then "router" else "" end)
        ]
      | select(.[0] != "")
      | @tsv
    '
    return
  fi

  # Fallback parser for systems without jq. OpenRouter's model catalog is JSON,
  # and shell is not a JSON language. This fallback intentionally extracts only
  # model IDs from common compact/pretty-printed shapes. Users who need richer
  # token metadata on a dependency-light system should install jq; the program
  # remains useful without it because model ID selection and testing still work.
  tr ',' '\n' | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1\t\1\t\t\t/p'
}
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function list_models
# Shows model candidates using jq when possible and sed/grep fallback
# otherwise. This is intentionally best-effort in bash-only.
# ----------------------------------------------------------

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function model_type_filter_output
# OpenRouter has both fixed model IDs and router-like entries such as
# openrouter/auto and openrouter/free.  Bash has no dropdown UI, so this helper
# gives the command line the same conceptual feature as the ncurses router
# selector: users can ask for routers, fixed models, free routes, or auto.
# The parser uses visible metadata columns produced by parse_models_table rather
# than making a second network request.
# ----------------------------------------------------------
model_type_filter_output() {
  local type="${MODEL_TYPE_FILTER:-all}"
  awk -F '\t' -v t="$type" '
    BEGIN { IGNORECASE=1 }
    t == "all" { print; next }
    { id=$1; badges=$5 }
    t == "routers" && (id ~ /^openrouter\// || badges ~ /router|free/) { print; next }
    t == "fixed" && !(id ~ /^openrouter\// || badges ~ /router|free/) { print; next }
    t == "free" && (id == "openrouter/free" || id ~ /:free$/ || badges ~ /free/) { print; next }
    t == "auto" && (id == "openrouter/auto") { print; next }
  '
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function decorate_filter_sort_models
# The ncurses model browser can show badges, bookmarks, and sort orders inside
# a table.  The bash-only edition cannot provide a clickable table, but it can
# still honor the same underlying choices.  This function therefore performs
# three jobs after parse_models_basic emits tab-separated rows:
#   1. add a "favorite" badge when the model ID appears in the favorites file;
#   2. apply basic token and type filters without requiring Python;
#   3. sort the rows using ordinary Unix sort where possible.
# This gives old machines a useful approximation of the ncurses model browser
# while preserving the low-dependency contract.
# ----------------------------------------------------------
decorate_filter_sort_models() {
  awk -F '\t' -v OFS='\t' \
      -v favfile="$MODEL_FAVORITES_FILE" \
      -v type="${MODEL_TYPE_FILTER:-all}" \
      -v minin="${MODEL_MIN_INPUT_TOKENS:-0}" \
      -v minout="${MODEL_MIN_OUTPUT_TOKENS:-0}" \
      -v require_tokens="${MODEL_FILTER_REQUIRE_TOKENS:-0}" '
    BEGIN {
      IGNORECASE=1
      while ((getline line < favfile) > 0) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
        if (line != "") fav[line]=1
      }
    }
    {
      id=$1; inTok=$3+0; outTok=$4+0; badges=$5
      if (fav[id]) badges=(badges=="" ? "favorite" : badges ",favorite")
      isRouter=(id ~ /^openrouter\// || badges ~ /router|free/)
      isFree=(id == "openrouter/free" || id ~ /:free$/ || badges ~ /free/)
      if (type == "routers" && !isRouter) next
      if (type == "fixed" && isRouter) next
      if (type == "free" && !isFree) next
      if (type == "auto" && id != "openrouter/auto") next
      if (require_tokens ~ /^(1|yes|true|on)$/ && inTok <= 0) next
      if (inTok < minin) next
      if (outTok < minout) next
      $5=badges
      print
    }' | sort_models_basic
}

sort_models_basic() {
  case "$MODEL_SORT_ORDER" in
    favorites)
      awk -F '\t' 'BEGIN{OFS="\t"} {print (($5 ~ /favorite/) ? 0 : 1), $0}' | sort -t "$(printf '\t')" -k1,1n -k2,2 | cut -f2-
      ;;
    input|input-desc|input-tokens|tokens|context)
      sort -t "$(printf '\t')" -k3,3nr -k1,1
      ;;
    output|output-desc)
      sort -t "$(printf '\t')" -k4,4nr -k1,1
      ;;
    display)
      sort -t "$(printf '\t')" -k2,2 -k1,1
      ;;
    *)
      sort -t "$(printf '\t')" -k1,1
      ;;
  esac
}

list_models() { local json; json="$(models_json_source)" || return 1; printf '%s' "$json" | parse_models_basic | decorate_filter_sort_models; }

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function select_model
# Interactive line-oriented model choice. It should remain usable even on
# old terminals.
# ----------------------------------------------------------
select_model() {
  local lines choice selected model display input output answer status
  mapfile -t lines < <(list_models)
  [ "${#lines[@]}" -gt 0 ] || { printf '%s\n' "No OpenRouter models found."; return 1; }
  local i=1 line
  for line in "${lines[@]}"; do IFS=$'\t' read -r model display input output <<< "$line"; printf '%3d) %s - %s (input:%s output:%s)\n' "$i" "$model" "$display" "${input:-?}" "${output:-?}"; i=$((i+1)); done
  printf 'Select model number: '; IFS= read -r choice
  case "$choice" in ''|*[!0-9]*) printf '%s\n' "Invalid selection."; return 1 ;; esac
  [ "$choice" -ge 1 ] && [ "$choice" -le "${#lines[@]}" ] || { printf '%s\n' "Selection out of range."; return 1; }
  selected="${lines[$((choice-1))]}"; IFS=$'\t' read -r model display input output <<< "$selected"
  printf 'Selected: %s\n' "$model"; printf 'Test selected model? [Y/n] '; IFS= read -r answer
  case "$answer" in n|N|no|NO) status=0 ;; *) test_model "$model"; status=$? ;; esac
  if [ "$SAVE" -eq 1 ] && [ "$status" -eq 0 ]; then save_config_value MODEL "$model"; printf 'saved MODEL=%s\n' "$model"; fi
}

# ==========================================================
# OPENROUTER REQUESTS
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function build_prompt_with_attachments
# The bash-only edition deliberately keeps attachment handling simple. Text
# attachments are merged into the prompt because every chat model can read text.
# Binary/media files are represented by a visible notice instead of pretending
# this low-dependency shell path can safely build every possible multipart or
# multimodal request. The Python/ncurses edition remains the stronger media path.
# ----------------------------------------------------------
build_prompt_with_attachments() {
  local prompt="$1" file name mime
  printf '%s' "$prompt"
  for file in "${ATTACH_FILES[@]}"; do
    [ -f "$file" ] || { printf '\n\n[Attachment unavailable: %s]' "$file"; continue; }
    name="$(basename "$file")"
    if command -v file >/dev/null 2>&1; then mime="$(file -b --mime-type "$file" 2>/dev/null)"; else mime=""; fi
    case "$mime:$file" in
      text/*:*|*:*.txt|*:*.md|*:*.log|*:*.sh|*:*.c|*:*.h|*:*.py|*:*.json|*:*.yaml|*:*.yml)
        printf '\n\n[Attached text file: %s]\n' "$name"
        cat "$file"
        ;;
      *)
        printf '\n\n[Attachment not sent inline by ttychatter.bash: %s (%s)]' "$name" "${mime:-unknown MIME}"
        ;;
    esac
  done
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function build_openrouter_payload
# Builds the OpenRouter chat-completions JSON request. JSON construction in
# bash-only is necessarily more fragile than in the Python-helper edition, so
# this function keeps all manual escaping in one place. If jq is installed, it
# is used for correct JSON generation. Otherwise a conservative escape routine
# is used.
# ----------------------------------------------------------
build_openrouter_payload() {
  local model="$1" prompt="$2"
  if command -v jq >/dev/null 2>&1; then
    jq -n --arg model "$model" --arg text "$prompt" '{model:$model, messages:[{role:"user", content:$text}]}'
  else
    printf '{"model":"%s","messages":[{"role":"user","content":"%s"}]}' "$(printf '%s' "$model" | json_escape_stream)" "$(printf '%s' "$prompt" | json_escape_stream)"
  fi
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function extract_openrouter_text
# Extracts text from OpenRouter's chat-completions response. jq is preferred.
# The sed fallback is intentionally best-effort and documented as such because
# shell regular expressions cannot fully parse JSON.
# ----------------------------------------------------------
extract_openrouter_text() {
  if command -v jq >/dev/null 2>&1; then
    jq -r 'if .error then (.error.message // .error | tostring) else ([.choices[]?.message.content] | map(if type=="string" then . else tostring end) | join("\n")) end'
  else
    tr '\n' ' ' | sed -n 's/^.*"content"[[:space:]]*:[[:space:]]*"\(.*\)"[[:space:]]*[,}].*$/\1/p' | json_unescape_basic
  fi
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function call_model
# Single OpenRouter generation request. All curl/OpenRouter behavior should
# stay centralized here so endpoint, authentication, timeout, and error changes
# do not get scattered across the interactive loop and colon commands.
# ----------------------------------------------------------
call_model() {
  local model="$1" prompt="$2" full_prompt payload response text status
  if truthy "$DEMO_MODE"; then demo_ai_response "$prompt"; return 0; fi
  require_api_key || return 1
  full_prompt="$(build_prompt_with_attachments "$prompt")"
  payload="$(build_openrouter_payload "$(normalize_model_id "$model")" "$full_prompt")"
  response="$(printf '%s' "$payload" | curl -sS --max-time 120 -H 'Content-Type: application/json' -H "Authorization: Bearer $OPENROUTER_API_KEY" -H 'HTTP-Referer: https://github.com/remfan1994/ttychatter' -H 'X-Title: ttychatter' -d @- 'https://openrouter.ai/api/v1/chat/completions' 2>&1)"
  status=$?; [ "$status" -eq 0 ] || { printf 'curl error: %s\n' "$response"; return 1; }
  text="$(printf '%s' "$response" | extract_openrouter_text)"
  [ -n "$text" ] || text="(could not extract text response; raw response follows)\n$response"
  printf '%s\n' "$text"
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function test_model
# User-operated test for one selected model. Do not resurrect automatic model
# probing in this edition; users should choose a model, test it, and decide.
# ----------------------------------------------------------
test_model() { local model="$1"; printf 'Model: %s\nPrompt: %s\n\n' "$model" "$MODEL_TEST_PROMPT"; call_model "$model" "$MODEL_TEST_PROMPT"; }

# ==========================================================
# MEMORY / LOGS / ATTACHMENTS
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function resolve_session_path
# resolve_session_path is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
resolve_session_path() { printf '%s/%s.log
' "$SESSION_DIR" "$1"; }

resolve_session_arg() {
  case "$1" in
    */*|*.log) printf '%s
' "$1" ;;
    *) printf '%s/%s.log
' "$SESSION_DIR" "$(sanitize_session_name "$1")" ;;
  esac
}

session_title_clean() {
  local max="${2:-8}"
  case "$max" in ''|*[!0-9]*) max=8 ;; esac
  [ "$max" -lt 1 ] && max=8
  printf '%s
' "$1" | tr '
	' '   ' | awk -v max="$max" '
    { gsub(/[[:cntrl:]]/," "); gsub(/[[:space:]]+/," "); sub(/^[[:space:]]+/,""); sub(/[[:space:]]+$/,"");
      lower=tolower($0); if (lower ~ /^session[ -]?title:/ || lower ~ /^title:/) sub(/^[^:]*:[[:space:]]*/,"");
      gsub(/^["`*.-]+/,""); gsub(/["`*.-]+$/,"");
      n=split($0,w," "); out=""; for(i=1;i<=n && i<=max;i++){ if(w[i]!="") out=(out?out" ":"") w[i]; }
      if(length(out)>96) out=substr(out,1,96); sub(/[[:space:]]+$/,"",out); if(out!="") print out; }'
}

session_title_from_file() {
  local path="$1" raw
  [ -f "$path" ] || return 1
  raw="$(awk 'BEGIN{IGNORECASE=1} /session-title:/ { line=$0; sub(/^.*session-title:[[:space:]]*/,"",line); val=line } END{ if(val!="") print val }' "$path")"
  [ -n "$raw" ] || return 1
  session_title_clean "$raw" "$SESSION_TITLE_MAX_WORDS"
}

write_session_title_metadata() {
  local path="$1" title clean
  title="$2"
  clean="$(session_title_clean "$title" "$SESSION_TITLE_MAX_WORDS")"
  [ -n "$clean" ] || return 1
  mkdir -p "$(dirname "$path")"
  printf '[%s] system: session-title: %s
' "$(timestamp)" "$clean" >> "$path"
}

ensure_prechat_line() {
  local path="$1"
  mkdir -p "$(dirname "$path")"
  if [ ! -s "$path" ]; then
    printf '%s
' "$TC_PRECHAT_LINE" > "$path"
  fi
}

list_sessions() {
  local f base title shown=0
  mkdir -p "$SESSION_DIR"
  printf '%-42s %s
' "TITLE" "FILE"
  printf '%-42s %s
' "-----" "----"
  for f in "$SESSION_DIR"/*.log; do
    [ -e "$f" ] || continue
    base="$(basename "$f" .log)"
    title="$(session_title_from_file "$f" 2>/dev/null || true)"
    [ -n "$title" ] || title="$base"
    printf '%-42.42s %s
' "$title" "$(basename "$f")"
    shown=1
  done
  [ "$shown" -eq 1 ] || printf '%s
' "no saved sessions"
}
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function add_context
# Maintains rolling conversation memory for chat continuity.
# ----------------------------------------------------------
add_context() { CONTEXT_BUFFER+=("$1"); local max=$((CONTEXT_TURNS*2)); while [ "${#CONTEXT_BUFFER[@]}" -gt "$max" ]; do CONTEXT_BUFFER=("${CONTEXT_BUFFER[@]:1}"); done; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function build_context
# build_context is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
build_context() { local item out=""; for item in "${CONTEXT_BUFFER[@]}"; do [ -n "$out" ] && out+=$'\n'; out+="$item"; done; printf '%s' "$out"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function load_context
# load_context is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
load_context() { [ -f "$SESSION" ] || return 0; mapfile -t CONTEXT_BUFFER < <(grep -E '^(User|Assistant): ' "$SESSION" | tail -n $((CONTEXT_TURNS*2))); }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function write_context_snapshot
# write_context_snapshot is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
write_context_snapshot() { local item ts="$1"; printf '[%s] system: CONTEXT_BEGIN\n' "$ts" >> "$SESSION"; for item in "${CONTEXT_BUFFER[@]}"; do printf '%s\n' "$item" >> "$SESSION"; done; printf '[%s] system: CONTEXT_END\n' "$ts" >> "$SESSION"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function extract_code_blocks
# Saves generated fenced code to files when possible, keeping terminal
# output readable.
# ----------------------------------------------------------
extract_code_blocks() {
  awk -v dir="$ATTACHMENT_DIR" -v session="$SESSION_NAME" -v min_lines="$CODE_ATTACHMENT_MIN_LINES" '
    BEGIN { inblock=0; count=0; content=""; linecount=0; lang="text" }
    /^```/ {
      if (!inblock) {
        inblock=1
        lang=$0
        sub(/^```/, "", lang)
        if (lang == "") lang="text"
        content=""
        linecount=0
        next
      }
      if (linecount < min_lines) {
        print "```" lang
        printf "%s", content
        print "```"
      } else {
        count++
        file=dir "/" session "-" lang "-" sprintf("%02d", count) ".txt"
        printf "%s", content > file
        close(file)
        print "[attachment saved]"
        print "file: " file
      }
      inblock=0
      content=""
      linecount=0
      next
    }
    {
      if (inblock) {
        content = content $0 ORS
        if (length($0) > 0) linecount++
      } else {
        print
      }
    }
    END {
      if (inblock) {
        print "```" lang
        printf "%s", content
      }
    }
  '
}



# ----------------------------------------------------------
# MAINTAINER COMMENTARY: model favorites in bash-only
# The dependency-light edition still needs persistent model bookmarks.  We use
# a plain one-id-per-line file so this feature works without Python or jq.  The
# format is intentionally boring: it can be edited with sed, cat, vi, or any
# ancient text tool available on old systems.
# ----------------------------------------------------------
ensure_favorites_dir() {
  mkdir -p "$(dirname "$MODEL_FAVORITES_FILE")" 2>/dev/null || true
}

list_favorite_models() {
  [ -f "$MODEL_FAVORITES_FILE" ] && grep -v '^[[:space:]]*$' "$MODEL_FAVORITES_FILE" || true
}

favorite_model_command() {
  local model="$1"
  [ -n "$model" ] || { printf 'ERROR: model id required.\n' >&2; return 2; }
  ensure_favorites_dir
  touch "$MODEL_FAVORITES_FILE"
  if ! grep -Fxq "$model" "$MODEL_FAVORITES_FILE" 2>/dev/null; then
    printf '%s\n' "$model" >> "$MODEL_FAVORITES_FILE"
  fi
  printf 'favorited model: %s\n' "$model"
}

unfavorite_model_command() {
  local model="$1" tmp
  [ -n "$model" ] || { printf 'ERROR: model id required.\n' >&2; return 2; }
  [ -f "$MODEL_FAVORITES_FILE" ] || return 0
  tmp="$(mktemp "${MODEL_FAVORITES_FILE}.tmp.XXXXXX")"
  grep -Fxv "$model" "$MODEL_FAVORITES_FILE" > "$tmp" || true
  mv "$tmp" "$MODEL_FAVORITES_FILE"
  printf 'unfavorited model: %s\n' "$model"
}


# ----------------------------------------------------------
# MAINTAINER COMMENTARY: bash-only style output
# Theme support in a shell client is necessarily modest.  The goal is parity
# with user configuration, not a fake curses renderer.  We only emit ANSI
# styling when stdout is an interactive terminal.  This protects loopback and
# pipeline users from receiving escape sequences in machine-consumed output.
# ----------------------------------------------------------
ai_style_start() { [ -t 1 ] || return 0; case "$THEME" in dim|default|solarized_dark) printf '[2m';; high_contrast) printf '[1m';; mono) ;; esac; }
ai_style_end() { [ -t 1 ] && [ "$THEME" != "mono" ] && printf '[0m' || true; }
print_ai_output() { ai_style_start; printf '%s

' "$1"; ai_style_end; }

# ==========================================================
# RUNTIME COLON-COMMAND MODE
# ----------------------------------------------------------
# The bash-only edition needs a way to expose client operations while the chat
# loop is running. Ncurses can use F-key screens; this edition deliberately has
# no such UI layer. Colon commands are the dependency-light equivalent. They
# are local client commands, not AI messages. They are not logged, not sent
# to the model, and not stored in memory.
#
# This mechanism is essential to keeping the bash-only edition from becoming
# a mere curl wrapper. Users can manage sessions, models, memory, attachments,
# editor workflow, and config without quitting the client.
# ==========================================================

runtime_command_help() {
  cat <<EOF
Runtime commands are local ttychatter.bash commands.
They are not sent to OpenRouter.

General:
  :help                   Show this menu
  :quit                   Exit chat
  :credits                Show credits
  :doctor                 Run diagnostics

Sessions:
  :list                   List saved sessions with friendly titles
  :rename TITLE           Set friendly session-title metadata

Models:
  :models                 Show model list
  :update-models          Refresh model cache from OpenRouter
  :select-model           Numbered model selector
  :test-model MODEL       Test one model
  :model MODEL            Use MODEL for this running chat only
  :model-save MODEL       Use MODEL and save MODEL=... to config
  :favorites              List favorite/bookmarked models
  :favorite MODEL         Add MODEL to favorites
  :unfavorite MODEL       Remove MODEL from favorites

Config and API key:
  :config                 Print config summary
  :set KEY VALUE          Save config value
  :unset KEY              Remove config key
  :set-api-key            Prompt and save API key
  :forget-api-key         Remove API key from config

Memory:
  :memory                 Show current memory/context
  :clear-memory           Clear current memory/context

Files and editor:
  :attach FILE            Attach FILE to next message
  :attachments            List saved attachments
  :editor                 Compose a prompt in VISUAL/EDITOR/vi/nano and send it
  :turns                  List message numbers
  :branch N               Branch current session after message N
  :search TEXT            Search current session
  :search-all TEXT        Search all saved sessions
  :stream on|off          Toggle requested streaming mode
  :status on|off          Toggle local status updates
  :voice FILE             Transcribe FILE with VOICE_INPUT_CMD and send
  :speak on|off|last      Toggle/speak with VOICE_OUTPUT_CMD

Use Ctrl+D after a colon command, just as you do for chat messages.
Type \: at the beginning if you really want to send a message beginning with :.
EOF
}

print_config_summary() {
  printf 'config file: %s\n' "$CONFIG_FILE"
  printf 'MODEL=%s\n' "${MODEL:-}"
  printf 'CONTEXT_TURNS=%s\n' "$CONTEXT_TURNS"
  printf 'SESSION_DIR=%s\n' "$SESSION_DIR"
  printf 'ATTACHMENT_DIR=%s\n' "$ATTACHMENT_DIR"
  printf 'MODEL_TEST_PROMPT=%s\n' "$MODEL_TEST_PROMPT"
  printf 'STARTUP_NOTICE=%s\n' "$STARTUP_NOTICE"
  printf 'SESSION_AUTO_TITLE=%s\n' "$SESSION_AUTO_TITLE"
  printf 'SESSION_TITLE_MODEL=%s\n' "$SESSION_TITLE_MODEL"
  printf 'SESSION_TITLE_MAX_WORDS=%s\n' "$SESSION_TITLE_MAX_WORDS"
  printf 'STREAM=%s\n' "$STREAM"
  printf 'STATUS_UPDATES=%s\n' "$STATUS_UPDATES"
  printf 'VOICE_INPUT_CMD=%s\n' "$VOICE_INPUT_CMD"
  printf 'VOICE_OUTPUT_CMD=%s\n' "$VOICE_OUTPUT_CMD"
  printf 'VOICE_OUTPUT=%s\n' "$VOICE_OUTPUT"
  printf 'THEME=%s\n' "$THEME"
  printf 'SEND_INPUT=%s\n' "$SEND_INPUT"
  printf 'CODE_ATTACHMENT_MIN_LINES=%s\n' "$CODE_ATTACHMENT_MIN_LINES"
  printf 'MODEL_FAVORITES_FILE=%s\n' "$MODEL_FAVORITES_FILE"
  printf 'MODEL_SORT_ORDER=%s\n' "$MODEL_SORT_ORDER"
  printf 'MODEL_MIN_INPUT_TOKENS=%s\n' "$MODEL_MIN_INPUT_TOKENS"
  printf 'MODEL_MIN_OUTPUT_TOKENS=%s\n' "$MODEL_MIN_OUTPUT_TOKENS"
  if [ -n "$OPENROUTER_API_KEY" ]; then printf 'OPENROUTER_API_KEY=(loaded)\n'; else printf 'OPENROUTER_API_KEY=(not set)\n'; fi
}

editor_command_basic() {
  if [ -n "${VISUAL:-}" ]; then printf '%s\n' "$VISUAL"; return 0; fi
  if [ -n "${EDITOR:-}" ]; then printf '%s\n' "$EDITOR"; return 0; fi
  command -v nano >/dev/null 2>&1 && { printf '%s\n' "nano"; return 0; }
  command -v vi >/dev/null 2>&1 && { printf '%s\n' "vi"; return 0; }
  printf '%s\n' "vi"
}

compose_with_editor_basic() {
  local tmp editor
  tmp="$(mktemp)" || return 1
  editor="$(editor_command_basic)"
  $editor "$tmp"
  cat "$tmp"
  rm -f "$tmp"
}

rename_current_session() {
  local new_title="$1"
  [ -n "$new_title" ] || { printf '%s
' "usage: :rename NEW_TITLE"; return 1; }
  ensure_prechat_line "$SESSION"
  write_session_title_metadata "$SESSION" "$new_title" || return 1
  printf 'session title set: %s
' "$(session_title_from_file "$SESSION" 2>/dev/null || printf '%s' "$new_title")"
}

rename_session_command() {
  local target="$1" new_title="$2" path
  [ -n "$target" ] && [ -n "$new_title" ] || { printf '%s
' "ERROR: --rename-session requires SESSION TITLE" >&2; return 2; }
  path="$(resolve_session_arg "$target")"
  [ -f "$path" ] || { printf 'ERROR: session not found: %s
' "$target" >&2; return 1; }
  write_session_title_metadata "$path" "$new_title" || return 1
  printf 'session title set: %s
' "$(session_title_from_file "$path" 2>/dev/null || printf '%s' "$new_title")"
}

show_current_memory_runtime() {
  local item
  [ "${#CONTEXT_BUFFER[@]}" -eq 0 ] && { printf '%s\n' "current memory is empty"; return 0; }
  printf '%s\n' "current memory/context buffer:"
  printf '%s\n' "----------------------------------------"
  for item in "${CONTEXT_BUFFER[@]}"; do printf '%s\n' "$item"; done
  printf '%s\n' "----------------------------------------"
}

list_attachments_runtime() {
  local file
  [ -d "$ATTACHMENT_DIR" ] || { printf 'attachment directory missing: %s\n' "$ATTACHMENT_DIR"; return 1; }
  for file in "$ATTACHMENT_DIR"/*; do
    [ -e "$file" ] || { printf 'no attachments found in %s\n' "$ATTACHMENT_DIR"; return 0; }
    basename "$file"
  done
}

apply_runtime_config_change() {
  local key="$1" value="$2"
  case "$key" in
    MODEL) MODEL="$(normalize_model_id "$value")"; if truthy "$DEMO_MODE" && [ -z "$MODEL" ]; then MODEL="demo/auto"; fi
ACTIVE_MODEL="$(effective_model)" ;;
    CONTEXT_TURNS|HISTORY_LIMIT) CONTEXT_TURNS="$value" ;;
    SESSION_DIR) SESSION_DIR="${value/#\~/$HOME}"; mkdir -p "$SESSION_DIR" ;;
    ATTACHMENT_DIR) ATTACHMENT_DIR="${value/#\~/$HOME}"; mkdir -p "$ATTACHMENT_DIR" ;;
    MODEL_TEST_PROMPT) MODEL_TEST_PROMPT="$value" ;;
    STARTUP_NOTICE) STARTUP_NOTICE="$value" ;;
    THEME) THEME="$value" ;;
    SEND_INPUT) SEND_INPUT="$value" ;;
    CODE_ATTACHMENT_MIN_LINES) CODE_ATTACHMENT_MIN_LINES="$value" ;;
    MODEL_FAVORITES_FILE) MODEL_FAVORITES_FILE="${value/#\~/$HOME}" ;;
    MODEL_SORT_ORDER) MODEL_SORT_ORDER="$value" ;;
    MODEL_MIN_INPUT_TOKENS) MODEL_MIN_INPUT_TOKENS="$value" ;;
    MODEL_MIN_OUTPUT_TOKENS) MODEL_MIN_OUTPUT_TOKENS="$value" ;;
    MODEL_FILTER_REQUIRE_TOKENS) MODEL_FILTER_REQUIRE_TOKENS="$value" ;;
  esac
}


status_update() {
  truthy "$STATUS_UPDATES" || return 0
  printf '[%s] status: %s\n' "$(timestamp)" "$*" >&2
}

shell_quote() { printf '%q' "$1"; }

command_with_file_placeholder() {
  local template="$1" file="$2" q
  q="$(shell_quote "$file")"
  case "$template" in *%f*) printf '%s\n' "${template//%f/$q}" ;; *) printf '%s %s\n' "$template" "$q" ;; esac
}

run_voice_input_command() {
  local path="$1" cmd out
  [ -n "$path" ] || { printf '%s\n' "ERROR: --voice-input requires a file" >&2; return 2; }
  [ -n "$VOICE_INPUT_CMD" ] || { printf '%s\n' "ERROR: VOICE_INPUT_CMD is not configured" >&2; return 1; }
  cmd="$(command_with_file_placeholder "$VOICE_INPUT_CMD" "$path")"
  out="$(eval "$cmd")" || return 1
  printf '%s\n' "$out"
}

run_voice_output_command() {
  local text="$1"
  [ -n "$VOICE_OUTPUT_CMD" ] || { printf '%s\n' "ERROR: VOICE_OUTPUT_CMD/TTS_CMD is not configured" >&2; return 1; }
  printf '%s\n' "$text" | eval "$VOICE_OUTPUT_CMD"
}

generate_session_title() {
  local user_text="$1" ai_text="$2" prompt title saved
  truthy "$SESSION_AUTO_TITLE" || return 0
  [ -n "$(session_title_from_file "$SESSION" 2>/dev/null || true)" ] && return 0
  if truthy "$DEMO_MODE"; then
    title="$(session_title_clean "$user_text" "$SESSION_TITLE_MAX_WORDS")"
  else
    [ -n "$OPENROUTER_API_KEY" ] || return 0
    [ -n "$SESSION_TITLE_MODEL" ] || return 0
    prompt="Generate a concise topical title for this ttychatter session. Reply with only the title, no quotes, no explanation, maximum $SESSION_TITLE_MAX_WORDS words.\n\nUser: $user_text\nAssistant: $ai_text"
    saved=("${ATTACH_FILES[@]}")
    ATTACH_FILES=()
    title="$(call_model "$SESSION_TITLE_MODEL" "$prompt" 2>/dev/null | head -n 1)"
    ATTACH_FILES=("${saved[@]}")
    title="$(session_title_clean "$title" "$SESSION_TITLE_MAX_WORDS")"
  fi
  [ -n "$title" ] && write_session_title_metadata "$SESSION" "$title"
}

turns_command() {
  local path="$1"
  path="$(resolve_session_arg "$path")"
  [ -f "$path" ] || { printf 'session not found: %s\n' "$path" >&2; return 1; }
  printf '%-5s %-10s %s\n' "MSG" "ROLE" "PREVIEW"
  awk '
    /^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] / {
      line=$0; sub(/^\[[^]]*\] /,"",line); speaker=line; sub(/:.*/,"",speaker);
      if (speaker=="system" || speaker=="remfan1994") next;
      role=(speaker=="AI" || speaker=="OpenRouter") ? "assistant" : "user";
      content=line; sub(/^[^:]*:[[:space:]]*/,"",content); gsub(/[[:space:]]+/," ",content);
      n++; if(length(content)>78) content=substr(content,1,75)"...";
      printf "%-5d %-10s %s\n", n, role, content;
    }' "$path"
}

branch_session_command() {
  local source="$1" turn="$2" src branch
  [ -n "$source" ] && [ -n "$turn" ] || { printf '%s\n' "usage: --branch SESSION TURN" >&2; return 2; }
  src="$(resolve_session_arg "$source")"
  [ -f "$src" ] || { printf 'session not found: %s\n' "$src" >&2; return 1; }
  branch="$(resolve_session_path "session-$(date +%Y-%m-%d-%H-%M-%S)")"
  ensure_prechat_line "$branch"
  printf '[%s] system: branch-from: %s\n[%s] system: branch-turn: %s\n[%s] system: branch-reason: manual-branch\n' "$(timestamp)" "$src" "$(timestamp)" "$turn" "$(timestamp)" >> "$branch"
  awk -v max="$turn" '
    function ismsg(line, speaker) { if (line !~ /^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] /) return 0; speaker=line; sub(/^\[[^]]*\] /,"",speaker); sub(/:.*/,"",speaker); return !(speaker=="system" || speaker=="remfan1994"); }
    { if (ismsg($0)) { count++; keep=(count<=max) } if (keep && count<=max) print }
  ' "$src" >> "$branch"
  printf '%s\n' "$branch"
}

export_session_command() {
  local name="$1" fmt="${2:-markdown}" out="$3" path title tmp
  [ -n "$name" ] || { printf '%s\n' "usage: --export SESSION [--format markdown|text] [--output FILE]" >&2; return 2; }
  path="$(resolve_session_arg "$name")"
  [ -f "$path" ] || { printf 'session not found: %s\n' "$path" >&2; return 1; }
  title="$(session_title_from_file "$path" 2>/dev/null || basename "$path" .log)"
  tmp="$(mktemp)" || return 1
  case "$fmt" in
    text)
      printf 'Title: %s\nSession: %s\n\n' "$title" "$path" > "$tmp"
      awk '/^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] / { line=$0; sub(/^\[[^]]*\] /,"",line); speaker=line; sub(/:.*/,"",speaker); if(speaker=="system"||speaker=="remfan1994") next; sub(/^[^:]*:[[:space:]]*/,"",line); print speaker ":\n" line "\n" }' "$path" >> "$tmp"
      ;;
    json)
      printf '%s\n' "bash-only JSON export is intentionally omitted; use ttychatter.python or the C edition." >&2; rm -f "$tmp"; return 2 ;;
    *)
      printf '# %s\n\nSource: `%s`\n\n' "$title" "$path" > "$tmp"
      awk '/^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] / { line=$0; sub(/^\[[^]]*\] /,"",line); speaker=line; sub(/:.*/,"",speaker); if(speaker=="system"||speaker=="remfan1994") next; sub(/^[^:]*:[[:space:]]*/,"",line); label=(speaker=="AI"?"Assistant":"User"); print "## " label "\n\n" line "\n" }' "$path" >> "$tmp"
      ;;
  esac
  if [ -n "$out" ]; then cat "$tmp" > "$out"; else cat "$tmp"; fi
  rm -f "$tmp"
}

search_session_file() {
  local file="$1" query="$2"
  [ -f "$file" ] || { printf 'no such session log: %s\n' "$file" >&2; return 1; }
  grep -in -- "$query" "$file" 2>/dev/null | sed "s#^#$(basename "$file"):#" || true
}

search_all_sessions() {
  local query="$1" file found=0
  for file in "$SESSION_DIR"/*.log; do
    [ -e "$file" ] || continue
    if grep -iq -- "$query" "$file" 2>/dev/null; then
      found=1
      printf '%s\n' "--- $(basename "$file") ---"
      grep -in -- "$query" "$file" 2>/dev/null || true
    fi
  done
  [ "$found" -eq 1 ] || printf 'no matches for: %s\n' "$query"
}

search_command() {
  local query="$1" all="$2" target="${3:-}"
  [ -n "$query" ] || { printf '%s\n' "ERROR: --search requires text" >&2; return 2; }
  if [ "$all" -eq 1 ]; then
    search_all_sessions "$query"
  else
    [ -n "$target" ] || target="$SESSION"
    search_session_file "$target" "$query"
  fi
}

edit_turn_command() {
  printf '%s\n' "--edit-turn is not implemented in ttychatter.bash; use ttychatter.python or the C edition for edit-and-branch." >&2
  return 2
}

handle_runtime_command() {
  local raw="$1" command rest key value editor_buffer
  case "$raw" in
    \\:*) RUNTIME_SEND_OVERRIDE="${raw#\\}"; return 2 ;;
    :*) ;;
    *) return 1 ;;
  esac
  raw="${raw#:}"
  command="${raw%%[[:space:]]*}"
  if [ "$command" = "$raw" ]; then rest=""; else rest="${raw#*[[:space:]]}"; fi

  case "$command" in
    ""|help|h|\?) runtime_command_help ;;
    quit|exit) printf '%s\n' "exiting"; exit 0 ;;
    credits) credits ;;
    doctor) doctor_command ;;
    list|sessions) list_sessions ;;
    rename) rename_current_session "$rest" ;;
    turns) turns_command "$SESSION_NAME" ;;
    branch|fork) branch_session_command "$SESSION_NAME" "$rest" ;;
    search) [ -n "$rest" ] && search_command "$rest" 0 "$SESSION" || printf '%s
' "usage: :search TEXT" ;;
    search-all|search_all) [ -n "$rest" ] && search_command "$rest" 1 "$SESSION" || printf '%s
' "usage: :search-all TEXT" ;;
    stream) case "$rest" in on|1|yes|true) STREAM=1; printf '%s
' "streaming requested" ;; off|0|no|false) STREAM=0; printf '%s
' "streaming disabled" ;; *) printf '%s
' "usage: :stream on|off" ;; esac ;;
    status|progress) case "$rest" in on|1|yes|true) STATUS_UPDATES=1; printf '%s
' "status updates enabled" ;; off|0|no|false) STATUS_UPDATES=0; printf '%s
' "status updates disabled" ;; *) printf '%s
' "usage: :status on|off" ;; esac ;;
    voice|transcribe) [ -n "$rest" ] && { voice_text="$(run_voice_input_command "$rest")" && send_one_message "$voice_text"; } || printf '%s
' "usage: :voice FILE" ;;
    speak) case "$rest" in on) SPEAK_OVERRIDE=1; printf '%s
' "voice output enabled" ;; off) SPEAK_OVERRIDE=0; printf '%s
' "voice output disabled" ;; last) [ -n "${LAST_AI_OUTPUT:-}" ] && run_voice_output_command "$LAST_AI_OUTPUT" || printf '%s
' "no previous AI output" ;; *) printf '%s
' "usage: :speak on|off|last" ;; esac ;;
    models) list_models ;;
    routers) MODEL_TYPE_FILTER="routers"; list_models ;;
    update-models|update_models) update_models_cache ;;
    select-model|select_model) select_model ;;
    test-model|test_model) [ -n "$rest" ] && test_model "$rest" || printf '%s\n' "usage: :test-model MODEL" ;;
    model) [ -n "$rest" ] && { MODEL="$(normalize_model_id "$rest")"; if truthy "$DEMO_MODE" && [ -z "$MODEL" ]; then MODEL="demo/auto"; fi
ACTIVE_MODEL="$(effective_model)"; printf 'active model: %s\n' "$ACTIVE_MODEL"; } || printf 'active model: %s\n' "$(model_label)" ;;
    model-save) [ -n "$rest" ] && { MODEL="$(normalize_model_id "$rest")"; if truthy "$DEMO_MODE" && [ -z "$MODEL" ]; then MODEL="demo/auto"; fi
ACTIVE_MODEL="$(effective_model)"; save_config_value MODEL "$ACTIVE_MODEL"; printf 'saved MODEL=%s\n' "$ACTIVE_MODEL"; } || printf '%s\n' "usage: :model-save MODEL" ;;
    config) print_config_summary ;;
    set)
      key="${rest%%[[:space:]]*}"; if [ "$key" = "$rest" ]; then value=""; else value="${rest#*[[:space:]]}"; fi
      [ -n "$key" ] || { printf '%s\n' "usage: :set KEY VALUE"; return 0; }
      save_config_value "$key" "$value"; apply_runtime_config_change "$key" "$value"; printf 'set %s=%s\n' "$key" "$value"
      ;;
    unset) [ -n "$rest" ] && { remove_config_key "$rest"; [ "$rest" = "MODEL" ] && { MODEL=""; if truthy "$DEMO_MODE" && [ -z "$MODEL" ]; then MODEL="demo/auto"; fi
ACTIVE_MODEL="$(effective_model)"; }; printf 'removed config key if present: %s\n' "$rest"; } || printf '%s\n' "usage: :unset KEY" ;;
    set-api-key|api-key) set_api_key_command ;;
    forget-api-key) forget_api_key_command ;;
    memory|show-memory) show_current_memory_runtime ;;
    clear-memory) CONTEXT_BUFFER=(); write_context_snapshot "$(timestamp)"; printf '%s\n' "current memory cleared" ;;
    attach) [ -n "$rest" ] && { ATTACH_FILES+=("$rest"); printf 'queued attachment: %s\n' "$rest"; } || printf '%s\n' "usage: :attach FILE" ;;
    attachments) list_attachments_runtime ;;
    editor)
      editor_buffer="$(compose_with_editor_basic)"
      [ -n "$editor_buffer" ] && send_one_message "$editor_buffer" || printf '%s\n' "editor returned empty prompt"
      ;;
    *) printf 'unknown runtime command: :%s\n' "$command"; printf '%s\n' "type :help for available commands" ;;
  esac
  return 0
}

# ==========================================================
# ARGUMENTS
# ==========================================================

POSITIONAL_ARGS=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help|help) ACTION=help;;
    --version) ACTION=version;;
    --demo) DEMO_MODE=1;;
    -i|--interactive) INTERACTIVE=1;;
    -n|--new) FORCE_NEW=1;;
    -p|--prompt) shift; PROMPT_TEXT="${1:-}";;
    --input) shift; INPUT_FILE="${1:-}";;
    -o|--output) shift; OUTPUT_PATH="${1:-}";;
    -s|--session) shift; SESSION_NAME="${1:-}";;
    --stream) STREAM=1;; --no-stream) STREAM=0;;
    --status|--progress) STATUS_UPDATES=1;; --no-status|--no-progress) STATUS_UPDATES=0;;
    --voice-input|--transcribe) shift; VOICE_INPUT_PATH="${1:-}";;
    --speak) VOICE_OUTPUT=1;; --no-speak) VOICE_OUTPUT=0;;
    --turns) ACTION=turns; shift; TURN_SESSION="${1:-}";;
    --branch) ACTION=branch; shift; TURN_SESSION="${1:-}"; shift; TURN_NUMBER="${1:-}";;
    --edit-turn) ACTION=edit-turn; shift; TURN_SESSION="${1:-}"; shift; TURN_NUMBER="${1:-}";;
    --export) ACTION=export; shift; EXPORT_SESSION="${1:-}";;
    --search) ACTION=search; shift; SEARCH_QUERY="${1:-}";;
    --all-sessions) SEARCH_ALL=1;;
    --format) shift; EXPORT_FORMAT="${1:-markdown}";;
    --rename-session) ACTION=rename; shift; RENAME_OLD="${1:-}"; shift; RENAME_NEW="${1:-}";;
    --loopback) LOOPBACK=1;;
    --loopback-file) shift; LOOPBACK_FILE="${1:-}"; [ -n "$LOOPBACK_FILE" ] || { printf 'ERROR: --loopback-file requires a path.\n' >&2; exit 2; };;
    --doctor) ACTION=doctor;; --credits) ACTION=credits;; --list) ACTION=list;; --models) ACTION=models;; --routers) ACTION=models; MODEL_TYPE_FILTER="routers";;
    --favorites) ACTION=favorites;; --favorite-model) ACTION=favorite; shift; TEST_MODEL="${1:-}";; --unfavorite-model|--unbookmark-model) ACTION=unfavorite; shift; TEST_MODEL="${1:-}";;
    --model-type) shift; MODEL_TYPE_FILTER="${1:-all}";; --sort) shift; MODEL_SORT_ORDER="${1:-$MODEL_SORT_ORDER}";; --min-input-tokens) shift; MODEL_MIN_INPUT_TOKENS="${1:-$MODEL_MIN_INPUT_TOKENS}";; --min-output-tokens) shift; MODEL_MIN_OUTPUT_TOKENS="${1:-$MODEL_MIN_OUTPUT_TOKENS}";;
    --require-tokens) MODEL_FILTER_REQUIRE_TOKENS=1;; --allow-missing-tokens) MODEL_FILTER_REQUIRE_TOKENS=0;; --theme) shift; THEME="${1:-default}";; --send-input) shift; SEND_INPUT="${1:-ctrl_d}";; --code-attachment-min-lines) shift; CODE_ATTACHMENT_MIN_LINES="${1:-5}";;
    --config) ACTION=config;; --set) ACTION=set-config; shift; SET_KEY="${1:-}"; shift; SET_VALUE="${1:-}";; --unset) ACTION=unset-config; shift; UNSET_KEY="${1:-}";;
    --update-models) ACTION=update-models;; --select-model) ACTION=select;; --test-model) ACTION=test; shift; TEST_MODEL="${1:-}";; --set-api-key) ACTION=set-key;; --forget-api-key) ACTION=forget-key;; --save) SAVE=1;;
    --attach) shift; ATTACH_FILES+=("${1:-}");; --resume) RESUME=1; shift; SESSION_NAME="${1:-}";; --) shift; while [ "$#" -gt 0 ]; do POSITIONAL_ARGS+=("$1"); shift; done; break;;
    -*) printf 'unknown option: %s\n' "$1" >&2; exit 2;;
    *) POSITIONAL_ARGS+=("$1");;
  esac; shift
done

if [ "${#POSITIONAL_ARGS[@]}" -eq 1 ] && [ -z "$SESSION_NAME" ] && [ -z "$PROMPT_TEXT" ] && [ -z "$INPUT_FILE" ]; then
  if [ -f "${POSITIONAL_ARGS[0]}" ]; then INPUT_FILE="${POSITIONAL_ARGS[0]}"; else SESSION_NAME="${POSITIONAL_ARGS[0]}"; fi
elif [ "${#POSITIONAL_ARGS[@]}" -eq 2 ] && [ -z "$INPUT_FILE" ] && [ -z "$OUTPUT_PATH" ]; then
  INPUT_FILE="${POSITIONAL_ARGS[0]}"; OUTPUT_PATH="${POSITIONAL_ARGS[1]}"
elif [ "${#POSITIONAL_ARGS[@]}" -gt 0 ]; then
  printf 'unexpected argument: %s\n' "${POSITIONAL_ARGS[*]}" >&2; exit 2
fi

# The action dispatcher below deliberately stays near the bottom of the file.
# By the time we reach it, config has been loaded and helper functions exist.
# This keeps command behavior easy to audit: parse flags, dispatch one action,
# or fall through into the interactive chat loop.
case "$ACTION" in
  turns) turns_command "$TURN_SESSION"; exit $?;;
  branch) branch_session_command "$TURN_SESSION" "$TURN_NUMBER"; exit $?;;
  edit-turn) edit_turn_command "$TURN_SESSION" "$TURN_NUMBER"; exit $?;;
  export) export_session_command "$EXPORT_SESSION" "$EXPORT_FORMAT" "$OUTPUT_PATH"; exit $?;;
  search) if [ -z "$SESSION_NAME" ]; then SEARCH_ALL=1; fi; search_command "$SEARCH_QUERY" "$SEARCH_ALL" "$( [ -n "$SESSION_NAME" ] && resolve_session_arg "$SESSION_NAME" || true )"; exit $?;;
  rename) rename_session_command "$RENAME_OLD" "$RENAME_NEW"; exit $?;;
  help) help_menu; exit 0;; version) printf '%s version %s\n' "$PROGRAM" "$VERSION"; exit 0;; doctor) doctor_command; exit 0;; credits) credits; exit 0;; list) list_sessions; exit 0;; models) list_models; exit $?;; favorites) list_favorite_models; exit $?;; favorite) favorite_model_command "$TEST_MODEL"; exit $?;; unfavorite) unfavorite_model_command "$TEST_MODEL"; exit $?;; config) print_config_summary; exit 0;; set-config) [ -n "$SET_KEY" ] || { printf 'ERROR: --set requires KEY VALUE\n' >&2; exit 2; }; save_config_value "$SET_KEY" "$SET_VALUE"; exit $?;; unset-config) [ -n "$UNSET_KEY" ] || { printf 'ERROR: --unset requires KEY\n' >&2; exit 2; }; remove_config_key "$UNSET_KEY"; exit $?;; update-models) update_models_cache; exit $?;; select) select_model; exit $?;; test) [ -n "$TEST_MODEL" ] || { printf 'ERROR: --test-model requires a model\n' >&2; exit 2; }; test_model "$TEST_MODEL"; [ "$SAVE" -eq 1 ] && save_config_value MODEL "$TEST_MODEL"; exit $?;; set-key) set_api_key_command; exit $?;; forget-key) forget_api_key_command; exit $?;;
esac

# ==========================================================
# CHAT LOOP
# ==========================================================

require_api_key || exit 1
if [ "$FORCE_NEW" -eq 1 ] && { [ -n "$SESSION_NAME" ] || [ -n "$OUTPUT_PATH" ] || [ "$RESUME" -eq 1 ]; }; then
  printf '%s
' "ERROR: --new cannot be combined with --session, --output, --resume, or a session operand" >&2
  exit 2
fi
SESSION_ARG="$SESSION_NAME"
if [ -z "$SESSION_ARG" ]; then
  SESSION_NAME="session-$(date +%Y-%m-%d-%H-%M-%S)"
  SESSION="$(resolve_session_path "$SESSION_NAME")"
elif [ -n "$OUTPUT_PATH" ]; then
  SESSION_NAME="$(sanitize_session_name "$SESSION_ARG")"
  SESSION="$OUTPUT_PATH"
else
  case "$SESSION_ARG" in
    */*|*.log) SESSION="$(resolve_session_arg "$SESSION_ARG")"; SESSION_NAME="$(basename "$SESSION" .log)" ;;
    *) SESSION_NAME="$(sanitize_session_name "$SESSION_ARG")"; SESSION="$(resolve_session_path "$SESSION_NAME")" ;;
  esac
fi
SESSION_WAS_EMPTY=1
[ -s "$SESSION" ] && SESSION_WAS_EMPTY=0
[ "$RESUME" -eq 1 ] && [ ! -f "$SESSION" ] && { printf 'session not found: %s
' "$SESSION_NAME" >&2; exit 1; }
ensure_prechat_line "$SESSION"
load_context
if truthy "$DEMO_MODE" && [ -z "$MODEL" ]; then MODEL="demo/auto"; fi
ACTIVE_MODEL="$(effective_model)"
NOTICE_VISIBLE=0
truthy "$STARTUP_NOTICE" && [ "$SESSION_WAS_EMPTY" -eq 1 ] && NOTICE_VISIBLE=1

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: loopback output routing
# Loopback is for terminal users who want to pipe AI responses to another
# program while still seeing the normal ttychatter interface. A normal chat
# client prints prompts, separators, and status text; that output is useful to
# humans but contaminates pipelines. When --loopback is active, fd 3 preserves
# the original stdout for clean AI text, while ordinary interface output is
# redirected to stderr.
# ----------------------------------------------------------
exec 3>&1
if [ "$LOOPBACK" -eq 1 ]; then
  exec 1>&2
fi

printf '%s (bash-only)\n' "$PROGRAM"; printf 'session: %s\n' "$SESSION"; printf 'model: %s\n' "$(model_label)"; printf 'Ctrl+D to send, :help for commands, Ctrl+C to exit\n\n'
if [ "$NOTICE_VISIBLE" -eq 1 ]; then printf '%s\n\n' "$TC_PRECHAT_LINE"; fi
send_one_message() {
  local buffer="$1" ts context output cleaned
  ts="$(timestamp)"
  [ -z "$buffer" ] && { printf '%s
' "empty message ignored"; return 1; }
  printf '[%s] %s: %s
' "$ts" "$USER_NAME" "$buffer" >> "$SESSION"
  add_context "User: $buffer"
  context="$(build_context)"
  status_update "preparing request for $ACTIVE_MODEL"
  output="$(call_model "$ACTIVE_MODEL" "$context")"
  printf '[%s] AI: %s
' "$(timestamp)" "$output" >> "$SESSION"
  cleaned="$(printf '%s
' "$output" | extract_code_blocks)"
  add_context "Assistant: $cleaned"
  write_context_snapshot "$(timestamp)"
  if [ "$LOOPBACK" -eq 1 ]; then
    printf '%s\n' "$cleaned" >&3
  fi
  if [ -n "$LOOPBACK_FILE" ]; then
    printf '%s
' "$cleaned" >> "$LOOPBACK_FILE"
  fi
  LAST_AI_OUTPUT="$cleaned"
  generate_session_title "$buffer" "$cleaned"
  if truthy "$VOICE_OUTPUT" || [ "$SPEAK_OVERRIDE" = "1" ]; then run_voice_output_command "$cleaned" || true; fi
  if [ "$LOOPBACK" -eq 1 ]; then printf '%s
' "$cleaned"; else print_ai_output "$cleaned"; fi
}

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function read_user_input
# SEND_INPUT parity for the bash-only edition.  SEND_INPUT=enter sends one
# physical input line each time the user presses Enter.  The default
# SEND_INPUT=ctrl_d keeps the traditional multiline shell behavior where cat
# reads until EOF.  Ctrl+G cannot be implemented portably in ordinary cooked
# shell input, so it is accepted as a config idea but treated like ctrl_d here.
# ----------------------------------------------------------
read_user_input() {
  if [ "$SEND_INPUT" = "enter" ]; then
    IFS= read -r buffer
    return $?
  fi
  buffer="$(cat)"
  return 0
}

if [ -n "$VOICE_INPUT_PATH" ]; then
  PROMPT_TEXT="$(run_voice_input_command "$VOICE_INPUT_PATH")" || exit $?
fi
if [ -n "$INPUT_FILE" ]; then
  if [ "$INPUT_FILE" = "-" ]; then PROMPT_TEXT="$(cat)"; else PROMPT_TEXT="$(cat "$INPUT_FILE")" || exit 1; fi
fi
if [ -n "$PROMPT_TEXT" ] || { [ ! -t 0 ] && [ "$INTERACTIVE" -eq 0 ]; }; then
  if [ -z "$PROMPT_TEXT" ]; then PROMPT_TEXT="$(cat)"; fi
  send_one_message "$PROMPT_TEXT"
  exit $?
fi

while true; do
  ts="$(timestamp)"; printf '[%s] %s> ' "$ts" "$USER_NAME"; if ! read_user_input; then [ -t 0 ] || exit 0; continue; fi; if [ -z "$buffer" ]; then [ -t 0 ] || exit 0; continue; fi
  RUNTIME_SEND_OVERRIDE=""
  if handle_runtime_command "$buffer"; then
    continue
  else
    runtime_status=$?
    if [ "$runtime_status" -eq 2 ]; then buffer="$RUNTIME_SEND_OVERRIDE"; fi
  fi
  if [ "$NOTICE_VISIBLE" -eq 1 ]; then clear_screen; NOTICE_VISIBLE=0; fi
  send_one_message "$buffer"
done
