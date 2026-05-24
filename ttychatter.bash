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
VERSION="0.7.0-bash-only"
CONFIG_DIR="$HOME/.config/ttychatter/openrouter"
CONFIG_FILE="$CONFIG_DIR/config"
MODEL_CACHE_FILE="$CONFIG_DIR/models-cache.json"
CONFIG_READ_FILE="$CONFIG_FILE"
SESSION_DIR="$HOME/.local/share/ttychatter/openrouter/sessions"
ATTACHMENT_DIR=""
ATTACHMENT_DIR_CONFIGURED=0
MODEL=""
FALLBACK_MODEL="openrouter/auto"
CONTEXT_TURNS=8
OPENROUTER_API_KEY="${OPENROUTER_API_KEY:-${TTYCHATTER_API_KEY:-}}"
MODEL_TEST_PROMPT="Please reply with a short sentence confirming this model is available for text generation."
STARTUP_NOTICE=1
PROJECT_LEAD_NAME="remfan1994"
PROJECT_LEAD_NOTICE="I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994"
USER_NAME="$(whoami 2>/dev/null || printf user)"
CONTEXT_BUFFER=()
ATTACH_FILES=()
ACTION="chat"
SESSION_NAME=""
RESUME=0
SAVE=0
TEST_MODEL=""
RUNTIME_SEND_OVERRIDE=""

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
# MAINTAINER COMMENTARY: function clear_screen
# clear_screen is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
clear_screen() { [ -t 1 ] && printf '\033[H\033[J'; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function sanitize_session_name
# sanitize_session_name is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
sanitize_session_name() { printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g; s/^[._-]*//; s/[._-]*$//' | awk '{print ($0==""?"session":$0)}'; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function effective_model
# effective_model is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
effective_model() { if [ -n "$MODEL" ]; then normalize_model_id "$MODEL"; else printf '%s\n' "$FALLBACK_MODEL"; fi; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function model_label
# model_label is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
model_label() { if [ -n "$MODEL" ]; then normalize_model_id "$MODEL"; else printf '%s (fallback; no MODEL configured)\n' "$FALLBACK_MODEL"; fi; }


# ==========================================================
# DIAGNOSTICS / DOCTOR
# ----------------------------------------------------------
# The bash-only edition intentionally avoids Python. Its doctor command uses
# only shell builtins and common Unix commands to check whether the dependency-
# light client has enough tools, storage access, and network access to run.
# ==========================================================

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function doctor_status
# doctor_status is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
doctor_status() { printf '[%-4s] %s: %s\n' "$1" "$2" "$3"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function check_cmd
# check_cmd is part of the dependency-light client behavior. Changes should
# preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
check_cmd() { if command -v "$1" >/dev/null 2>&1; then doctor_status OK "command $1" "$(command -v "$1")"; else doctor_status FAIL "command $1" "not found"; fi; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function check_writable_dir
# check_writable_dir is part of the dependency-light client behavior.
# Changes should preserve old-system usability and update docs/comments if
# behavior changes.
# ----------------------------------------------------------
check_writable_dir() { mkdir -p "$1" 2>/dev/null || { printf 'not creatable: %s' "$1"; return 1; }; f="$1/.ttychatter.bash-write-test-$$"; if printf ok > "$f" 2>/dev/null; then rm -f "$f"; printf 'writable: %s' "$1"; return 0; fi; printf 'not writable: %s' "$1"; return 1; }

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function doctor_command
# Basic diagnostics for constrained systems. It should never leak API keys
# or require network generation calls.
# ----------------------------------------------------------
doctor_command() {
  printf '%s\n\n' "${PROGRAM} bash-only diagnostics"
  doctor_status OK program "$PROGRAM version $VERSION"
  doctor_status OK shell "${BASH_VERSION:-unknown bash}"
  for cmd in bash curl sed awk grep tr mktemp printf; do check_cmd "$cmd"; done
  command -v base64 >/dev/null 2>&1 && doctor_status OK "command base64" "$(command -v base64)" || doctor_status WARN "command base64" "not found; binary attachments limited"
  command -v file >/dev/null 2>&1 && doctor_status OK "command file" "$(command -v file)" || doctor_status WARN "command file" "not found; MIME detection falls back to extension"
  [ -f "$CONFIG_FILE" ] && doctor_status OK "config file" "$CONFIG_FILE present" || doctor_status WARN "config file" "not found: $CONFIG_FILE"
  [ -n "$OPENROUTER_API_KEY" ] && doctor_status OK "API key" loaded || doctor_status WARN "API key" "not loaded"
  d="$(check_writable_dir "$SESSION_DIR")"; st=$?; doctor_status $([ "$st" -eq 0 ] && printf OK || printf FAIL) "session dir" "$d"
  d="$(check_writable_dir "$ATTACHMENT_DIR")"; st=$?; doctor_status $([ "$st" -eq 0 ] && printf OK || printf FAIL) "attachment dir" "$d"
  d="$(check_writable_dir "$(dirname "$MODEL_CACHE_FILE")")"; st=$?; doctor_status $([ "$st" -eq 0 ] && printf OK || printf FAIL) "model cache dir" "$d"
  [ -f "$MODEL_CACHE_FILE" ] && doctor_status OK "model cache" "$MODEL_CACHE_FILE present" || doctor_status WARN "model cache" "missing; run --update-models"
  curl -sS --max-time 10 https://openrouter.ai/api/v1/models -H "Authorization: Bearer ${OPENROUTER_API_KEY:-dummy}" >/dev/null 2>&1 && doctor_status OK network "OpenRouter reachable" || doctor_status WARN network "OpenRouter not reachable or API key missing"
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
      SESSION_DIR) SESSION_DIR="${value/#\~/$HOME}" ;;
      ATTACHMENT_DIR) ATTACHMENT_DIR="${value/#\~/$HOME}"; ATTACHMENT_DIR_CONFIGURED=1 ;;
      MODEL_TEST_PROMPT) [ -n "$value" ] && MODEL_TEST_PROMPT="$value" ;;
      STARTUP_NOTICE) STARTUP_NOTICE="$value" ;;
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

load_config
case "$CONTEXT_TURNS" in ''|*[!0-9]*) CONTEXT_TURNS=8 ;; esac
[ "$CONTEXT_TURNS" -lt 1 ] && CONTEXT_TURNS=8
[ "$ATTACHMENT_DIR_CONFIGURED" -eq 0 ] && ATTACHMENT_DIR="$SESSION_DIR/attachments"
mkdir -p "$CONFIG_DIR" "$SESSION_DIR" "$ATTACHMENT_DIR"

# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function require_api_key
# require_api_key is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
require_api_key() {
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
    $PROGRAM [session]
    $PROGRAM --resume <session>
    $PROGRAM --list
    $PROGRAM --models
    $PROGRAM --update-models
    $PROGRAM --select-model [--save]
    $PROGRAM --test-model <model> [--save]
    $PROGRAM --set-api-key
    $PROGRAM --attach FILE [session]
    $PROGRAM --credits
    $PROGRAM --doctor
    $PROGRAM --version
    $PROGRAM --help

INPUT
    Type or paste a message, then press Ctrl+D to send.

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
        :select-model
        :model openrouter/auto
        :memory
        :attach /path/to/file.txt
        :editor
        :credits

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
  I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994
EOF
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
fetch_models_json() { require_api_key || return 1; curl -sS --max-time 60 -H "Authorization: Bearer $OPENROUTER_API_KEY" "https://openrouter.ai/api/v1/models"; }
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
models_json_source() { if [ -f "$MODEL_CACHE_FILE" ]; then cat "$MODEL_CACHE_FILE"; else fetch_models_json; fi; }
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
          (($m.max_completion_tokens // $m.maxCompletionTokens // "")|tostring)
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
  tr ',' '\n' | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1\t\1\t\t/p'
}
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function list_models
# Shows model candidates using jq when possible and sed/grep fallback
# otherwise. This is intentionally best-effort in bash-only.
# ----------------------------------------------------------
list_models() { local json; json="$(models_json_source)" || return 1; printf '%s' "$json" | parse_models_basic; }

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
resolve_session_path() { printf '%s/%s.log\n' "$SESSION_DIR" "$1"; }
# ----------------------------------------------------------
# MAINTAINER COMMENTARY: function list_sessions
# list_sessions is part of the dependency-light client behavior. Changes
# should preserve old-system usability and update docs/comments if behavior
# changes.
# ----------------------------------------------------------
list_sessions() { for f in "$SESSION_DIR"/*.log; do [ -e "$f" ] && basename "$f" .log; done; }
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
extract_code_blocks() { awk -v dir="$ATTACHMENT_DIR" -v session="$SESSION_NAME" 'BEGIN{inblock=0;count=0} /^```/ {if(!inblock){inblock=1;lang=$0;sub(/^```/,"",lang);if(lang=="")lang="text";count++;file=dir "/" session "-" lang "-" sprintf("%02d",count) ".txt";next}else{close(file);inblock=0;print "[attachment saved]\nfile: " file;next}} {if(inblock)print > file;else print}'; }


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
  :list                   List saved sessions
  :rename NAME            Rename current session/log file

Models:
  :models                 Show model list
  :update-models          Refresh model cache from OpenRouter
  :select-model           Numbered model selector
  :test-model MODEL       Test one model
  :model MODEL            Use MODEL for this running chat only
  :model-save MODEL       Use MODEL and save MODEL=... to config

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
  local new_name="$1" clean_name new_path old_path
  [ -n "$new_name" ] || { printf '%s\n' "usage: :rename NEW_SESSION_NAME"; return 1; }
  clean_name="$(sanitize_session_name "$new_name")"
  new_path="$(resolve_session_path "$clean_name")"
  old_path="$SESSION"
  [ "$new_path" = "$old_path" ] && { printf 'session already named: %s\n' "$clean_name"; return 0; }
  [ -e "$new_path" ] && { printf 'cannot rename: target exists: %s\n' "$clean_name"; return 1; }
  [ -f "$old_path" ] && mv "$old_path" "$new_path"
  SESSION_NAME="$clean_name"
  SESSION="$new_path"
  printf 'current session renamed to: %s\n' "$SESSION_NAME"
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
    MODEL) MODEL="$(normalize_model_id "$value")"; ACTIVE_MODEL="$(effective_model)" ;;
    CONTEXT_TURNS|HISTORY_LIMIT) CONTEXT_TURNS="$value" ;;
    SESSION_DIR) SESSION_DIR="${value/#\~/$HOME}"; mkdir -p "$SESSION_DIR" ;;
    ATTACHMENT_DIR) ATTACHMENT_DIR="${value/#\~/$HOME}"; mkdir -p "$ATTACHMENT_DIR" ;;
    MODEL_TEST_PROMPT) MODEL_TEST_PROMPT="$value" ;;
    STARTUP_NOTICE) STARTUP_NOTICE="$value" ;;
  esac
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
    models) list_models ;;
    update-models|update_models) update_models_cache ;;
    select-model|select_model) select_model ;;
    test-model|test_model) [ -n "$rest" ] && test_model "$rest" || printf '%s\n' "usage: :test-model MODEL" ;;
    model) [ -n "$rest" ] && { MODEL="$(normalize_model_id "$rest")"; ACTIVE_MODEL="$(effective_model)"; printf 'active model: %s\n' "$ACTIVE_MODEL"; } || printf 'active model: %s\n' "$(model_label)" ;;
    model-save) [ -n "$rest" ] && { MODEL="$(normalize_model_id "$rest")"; ACTIVE_MODEL="$(effective_model)"; save_config_value MODEL "$ACTIVE_MODEL"; printf 'saved MODEL=%s\n' "$ACTIVE_MODEL"; } || printf '%s\n' "usage: :model-save MODEL" ;;
    config) print_config_summary ;;
    set)
      key="${rest%%[[:space:]]*}"; if [ "$key" = "$rest" ]; then value=""; else value="${rest#*[[:space:]]}"; fi
      [ -n "$key" ] || { printf '%s\n' "usage: :set KEY VALUE"; return 0; }
      save_config_value "$key" "$value"; apply_runtime_config_change "$key" "$value"; printf 'set %s=%s\n' "$key" "$value"
      ;;
    unset) [ -n "$rest" ] && { remove_config_key "$rest"; [ "$rest" = "MODEL" ] && { MODEL=""; ACTIVE_MODEL="$(effective_model)"; }; printf 'removed config key if present: %s\n' "$rest"; } || printf '%s\n' "usage: :unset KEY" ;;
    set-api-key|api-key) printf 'Paste OpenRouter API key: '; IFS= read -r -s key; printf '\n'; [ -n "$key" ] && { OPENROUTER_API_KEY="$key"; save_config_value OPENROUTER_API_KEY "$key"; } ;;
    forget-api-key) remove_config_key OPENROUTER_API_KEY; printf '%s\n' "removed OPENROUTER_API_KEY from config if present" ;;
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help|help) ACTION=help;; --version) ACTION=version;; --doctor) ACTION=doctor;; --credits) ACTION=credits;; --list) ACTION=list;; --models) ACTION=models;; --update-models) ACTION=update-models;; --select-model) ACTION=select;; --test-model) ACTION=test; shift; TEST_MODEL="${1:-}";; --set-api-key) ACTION=set-key;; --save) SAVE=1;; --attach) shift; ATTACH_FILES+=("${1:-}");; --resume) RESUME=1; shift; SESSION_NAME="${1:-}";; -*) printf 'unknown option: %s\n' "$1" >&2; exit 2;; *) [ -z "$SESSION_NAME" ] && SESSION_NAME="$1" || { printf 'unexpected argument: %s\n' "$1" >&2; exit 2; };;
  esac; shift
 done

# The action dispatcher below deliberately stays near the bottom of the file.
# By the time we reach it, config has been loaded and helper functions exist.
# This keeps command behavior easy to audit: parse flags, dispatch one action,
# or fall through into the interactive chat loop.
case "$ACTION" in
  help) help_menu; exit 0;; version) printf '%s version %s\n' "$PROGRAM" "$VERSION"; exit 0;; doctor) doctor_command; exit 0;; credits) credits; exit 0;; list) list_sessions; exit 0;; models) list_models; exit $?;; update-models) update_models_cache; exit $?;; select) select_model; exit $?;; test) [ -n "$TEST_MODEL" ] || { printf 'ERROR: --test-model requires a model\n' >&2; exit 2; }; test_model "$TEST_MODEL"; [ "$SAVE" -eq 1 ] && save_config_value MODEL "$TEST_MODEL"; exit $?;; set-key) printf 'Paste OpenRouter API key: '; IFS= read -r -s key; printf '\n'; [ -n "$key" ] && save_config_value OPENROUTER_API_KEY "$key"; exit $?;;
esac

# ==========================================================
# CHAT LOOP
# ==========================================================

require_api_key || exit 1
[ -z "$SESSION_NAME" ] && SESSION_NAME="session-$(date +%Y-%m-%d-%H-%M-%S)" || SESSION_NAME="$(sanitize_session_name "$SESSION_NAME")"
SESSION="$(resolve_session_path "$SESSION_NAME")"
[ "$RESUME" -eq 1 ] && [ ! -f "$SESSION" ] && { printf 'session not found: %s\n' "$SESSION_NAME" >&2; exit 1; }
load_context
ACTIVE_MODEL="$(effective_model)"
NOTICE_VISIBLE=0
truthy "$STARTUP_NOTICE" && [ ! -s "$SESSION" ] && NOTICE_VISIBLE=1
printf '%s (bash-only)\n' "$PROGRAM"; printf 'session: %s\n' "$SESSION"; printf 'model: %s\n' "$(model_label)"; printf 'Ctrl+D to send, :help for commands, Ctrl+C to exit\n\n'
if [ "$NOTICE_VISIBLE" -eq 1 ]; then printf '[%s] %s: %s\n' "$(timestamp)" "$PROJECT_LEAD_NAME" "$PROJECT_LEAD_NOTICE"; printf '[%s] system: This is the message list window. The notice above will disappear when you send your first message, and messages to and from AI will appear here.\n\n' "$(timestamp)"; fi
send_one_message() {
  local buffer="$1" ts context output cleaned
  ts="$(timestamp)"
  [ -z "$buffer" ] && { printf '%s
' "empty message ignored"; return 1; }
  printf '[%s] %s: %s
' "$ts" "$USER_NAME" "$buffer" >> "$SESSION"
  add_context "User: $buffer"
  context="$(build_context)"
  output="$(call_model "$ACTIVE_MODEL" "$context")"
  printf '[%s] AI: %s
' "$(timestamp)" "$output" >> "$SESSION"
  cleaned="$(printf '%s
' "$output" | extract_code_blocks)"
  add_context "Assistant: $cleaned"
  write_context_snapshot "$(timestamp)"
  printf '%s

' "$cleaned"
}

while true; do
  ts="$(timestamp)"; printf '[%s] %s> ' "$ts" "$USER_NAME"; buffer="$(cat)"; if [ -z "$buffer" ]; then [ -t 0 ] || exit 0; continue; fi
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
