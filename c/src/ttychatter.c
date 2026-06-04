/*
 * ttychatter.c - command-line OpenRouter client for the ttychatter project
 *
 * This C edition is deliberately NOT an ncurses program.  The Python ncurses
 * edition is the full terminal application.  This file is the traditional Unix
 * command-line client: the user may start a plain interactive session, send an
 * input file into an automatically saved timestamped session, or use the classic
 * input-file/output-log form when an explicit path is desired.
 *
 * The design target is the classic shell workflow plus complete terminal prompt
 * coverage:
 *
 *     ttychatter
 *     $EDITOR prompt.txt
 *     ttychatter prompt.txt
 *     ttychatter -a notes.txt prompt.txt session.log
 *     ttychatter --prompt "summarize this" --session notes
 *     less session.log
 *
 * The output file is more than a transcript.  It is also the session state. On
 * subsequent invocations, ttychatter reads the most recent turns from that file
 * and sends them back to OpenRouter as context.  This means the user can resume
 * a conversation by simply reusing the same output file:
 *
 *     ttychatter new-question.txt session.log
 *
 * The implementation is heavily commented on purpose.  The project owner wants
 * future maintainers, porters, packagers, and fork authors to understand not
 * only what the code does, but why the code takes a given shape.  C code that
 * touches networking, terminal files, config files, and user secrets becomes
 * expensive to maintain if the original design intent is not written down.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TC_PROGRAM "ttychatter"
#define TC_VERSION "0.6.1-c-cli"
#define TC_OPENROUTER_CHAT_URL "https://openrouter.ai/api/v1/chat/completions"
#define TC_OPENROUTER_MODELS_URL "https://openrouter.ai/api/v1/models"

/*
 * New 0.6.1 logs use ttychatter's shared line-oriented OpenRouter transcript
 * format.  The older C marker strings remain defined so the parser can resume
 * 0.6.0-era C logs without forcing users to migrate old session files.
 */
#define TC_TURN_BEGIN "===== TTYCHATTER TURN BEGIN ====="
#define TC_TURN_END   "===== TTYCHATTER TURN END ====="
#define TC_USER_BEGIN "----- USER BEGIN -----"
#define TC_USER_END   "----- USER END -----"
#define TC_AI_BEGIN   "----- AI BEGIN -----"
#define TC_AI_END     "----- AI END -----"
#define TC_NOTICE_BEGIN "===== TTYCHATTER NOTICE BEGIN ====="
#define TC_NOTICE_END   "===== TTYCHATTER NOTICE END ====="
#define TC_MEMORY_CLEAR "===== TTYCHATTER MEMORY CLEAR ====="
#define TC_CONTEXT_BEGIN "===== TTYCHATTER CONTEXT BEGIN ====="
#define TC_CONTEXT_END   "===== TTYCHATTER CONTEXT END ====="
#define TC_PRECHAT_LINE "Everyone is encouraged to get the cruelty-free vegetarian alternatives and remember the bloodguilt curse from the Bible... http://bloodguiltcurse.net"

/*
 * Configuration defaults follow XDG conventions rather than the early project
 * prototype's ~/.gpt directory.  ~/.gpt was convenient while the project was
 * personal; public Unix software should keep config, cache, and data separate.
 */
typedef struct TCConfig {
    char *config_file;
    char *api_key;
    char *api_key_gpg_file;
    char *model;
    char *session_title_model;
    char *model_cache_file;
    char *model_favorites_file;
    char *session_dir;
    char *attachment_dir;
    long context_turns;
    long session_title_max_words;
    long code_attachment_min_lines;
    long max_attachment_bytes;
    char *theme;
    char *send_input;
    char *editor_cmd;
    char *voice_input_cmd;
    char *voice_output_cmd;
    char *model_test_prompt;
    char *model_sort_order;
    char *model_type_filter;
    bool model_filter_generate_content;
    bool model_filter_require_tokens;
    bool model_filter_hide_preview;
    long model_min_input_tokens;
    long model_min_output_tokens;
    bool confirm_live_send;
    bool startup_notice;
    bool session_auto_title;
    bool stream;
    bool status_updates;
    bool voice_output;
    bool demo_mode_default;
    bool loopback_default;
} TCConfig;

typedef struct TCArgs {
    bool show_help;
    bool show_version;
    bool doctor;
    bool set_api_key;
    bool set_api_key_gpg;
    bool forget_api_key;
    bool update_models;
    bool list_models;
    bool test_model;
    bool loopback;
    bool demo;
    bool interactive;
    bool show_config;
    bool favorites;
    bool favorite_model;
    bool unfavorite_model;
    bool config_set_cmd;
    bool config_unset_cmd;
    bool list_sessions_cmd;
    bool rename_session_cmd;
    bool show_memory_cmd;
    bool clear_memory_cmd;
    bool edit_memory_cmd;
    bool editor_prompt_cmd;
    bool credits_cmd;
    bool search_sessions_cmd;
    bool select_model_cmd;
    bool new_session_cmd;
    bool turns_cmd;
    bool branch_cmd;
    bool edit_turn_cmd;
    bool export_cmd;
    bool show_all_models;
    char *loopback_file;
    char *interactive_session_path;
    char *session_arg;
    char *prompt_text;
    char *config_key;
    char *config_value;
    char *rename_old;
    char *rename_new;
    char *memory_path;
    char *resume_session;
    char *turns_session;
    char *branch_session;
    long branch_turn;
    char *edit_turn_session;
    long edit_turn;
    char *export_session;
    char *export_format;
    char *search;
    char *model_type;
    char *model_sort;
    long min_input_tokens;
    long min_output_tokens;
    bool require_tokens;
    bool allow_missing_tokens;
    bool hide_preview;
    bool show_preview;
    bool favorites_only;
    bool save;
    bool yes;
    bool stream_set;
    bool stream;
    bool status_set;
    bool status_updates;
    bool speak_set;
    bool speak;
    char *model_override;
    char *test_model_id;
    char *input_path;
    char *output_path;
    char *voice_input_path;
    char **attachments;
    size_t attachment_count;
} TCArgs;

typedef struct TCBuffer {
    char *data;
    size_t len;
    size_t cap;
} TCBuffer;

typedef struct TCHttpResponse {
    long status;
    char *body;
} TCHttpResponse;

typedef struct TCMessage {
    char *role;
    char *content;
} TCMessage;

typedef struct TCMessageList {
    TCMessage *items;
    size_t count;
    size_t cap;
} TCMessageList;

typedef struct TCAttachmentResult {
    char *prompt_text_addition;
    json_object *content_parts;
} TCAttachmentResult;

/* ------------------------------------------------------------------------- */
/* Small allocation and string helpers                                       */
/* ------------------------------------------------------------------------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *out = strdup(s);
    if (!out) die("out of memory");
    return out;
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) die("out of memory");
    return p;
}

static void buffer_init(TCBuffer *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = xcalloc(1, b->cap);
}

static void buffer_reserve(TCBuffer *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    char *p = realloc(b->data, b->cap);
    if (!p) die("out of memory");
    b->data = p;
}

static void buffer_append_n(TCBuffer *b, const char *s, size_t n) {
    buffer_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buffer_append(TCBuffer *b, const char *s) {
    if (s) buffer_append_n(b, s, strlen(s));
}

static void buffer_appendf(TCBuffer *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) die("vsnprintf failed");
    buffer_reserve(b, (size_t)need);
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
}

static char *buffer_take(TCBuffer *b) {
    char *out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

static char *trim_in_place(char *s) {
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool starts_with_casefold(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0;
}

static bool parse_bool_string(const char *value, bool fallback) {
    /*
     * Boolean config values are deliberately permissive because real users tend
     * to write config files by hand.  Accepting several common spellings avoids
     * making the C client feel brittle.  This helper is used only for user-facing
     * configuration keys; internal flags stay strongly typed as bool.
     */
    if (!value) return fallback;
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "y") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "on") == 0) return true;
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "n") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0) return false;
    return fallback;
}

static char *path_join2(const char *a, const char *b) {
    TCBuffer buf;
    buffer_init(&buf);
    buffer_append(&buf, a);
    if (buf.len && buf.data[buf.len - 1] != '/') buffer_append(&buf, "/");
    buffer_append(&buf, b);
    return buffer_take(&buf);
}

static char *home_path(const char *suffix) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    return path_join2(home, suffix);
}

static char *expand_tilde(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~' && path[1] == '/') {
        char *base = home_path(path + 2);
        return base;
    }
    return xstrdup(path);
}

static void mkdir_p(const char *path) {
    if (!path || !*path) return;
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    free(tmp);
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static long file_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("could not open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("could not seek %s", path);
    long n = ftell(f);
    if (n < 0) die("could not tell size for %s", path);
    rewind(f);
    char *data = xcalloc((size_t)n + 1, 1);
    size_t got = fread(data, 1, (size_t)n, f);
    if (got != (size_t)n && ferror(f)) die("could not read %s", path);
    fclose(f);
    data[got] = '\0';
    if (out_len) *out_len = got;
    return data;
}

static char *read_stream(FILE *f, size_t *out_len) {
    TCBuffer b;
    buffer_init(&b);
    char chunk[8192];
    while (!feof(f)) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n > 0) buffer_append_n(&b, chunk, n);
        if (ferror(f)) die("could not read input stream");
    }
    if (out_len) *out_len = b.len;
    return buffer_take(&b);
}

static char *read_input_source(const char *path, size_t *out_len) {
    if (!path || strcmp(path, "-") == 0) return read_stream(stdin, out_len);
    return read_file(path, out_len);
}

static void write_file_mode(const char *path, const char *data, mode_t mode) {
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(copy);
    }
    free(copy);
    FILE *f = fopen(path, "wb");
    if (!f) die("could not write %s: %s", path, strerror(errno));
    if (data) fwrite(data, 1, strlen(data), f);
    fclose(f);
    chmod(path, mode);
}

static void append_file_text(const char *path, const char *data) {
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(copy);
    }
    free(copy);
    FILE *f = fopen(path, "ab");
    if (!f) die("could not append %s: %s", path, strerror(errno));
    if (data) fwrite(data, 1, strlen(data), f);
    fclose(f);
}

static char *now_string(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z", &tmv);
    return xstrdup(buf);
}

static char *now_hms(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    return xstrdup(buf);
}

static char *basename_no_ext(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char *out = xstrdup(base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
    for (char *p = out; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') *p = '_';
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Base64 helper for data URLs                                               */
/* ------------------------------------------------------------------------- */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = xcalloc(out_len + 1, 1);
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned octet_a = i < len ? data[i++] : 0;
        unsigned octet_b = i < len ? data[i++] : 0;
        unsigned octet_c = i < len ? data[i++] : 0;
        unsigned triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    if (len % 3) {
        out[out_len - 1] = '=';
        if (len % 3 == 1) out[out_len - 2] = '=';
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Shell quoting and external GPG                                            */
/* ------------------------------------------------------------------------- */

static char *shell_quote(const char *s) {
    TCBuffer b;
    buffer_init(&b);
    buffer_append(&b, "'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buffer_append(&b, "'\\''");
        else buffer_append_n(&b, p, 1);
    }
    buffer_append(&b, "'");
    return buffer_take(&b);
}

static bool command_exists(const char *cmd) {
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = xstrdup(path);
    for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":")) {
        char *candidate = path_join2(tok, cmd);
        bool ok = access(candidate, X_OK) == 0;
        free(candidate);
        if (ok) {
            free(copy);
            return true;
        }
    }
    free(copy);
    return false;
}

static char *read_command_output(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    TCBuffer b;
    buffer_init(&b);
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), p)) buffer_append(&b, chunk);
    int rc = pclose(p);
    if (rc != 0) {
        free(b.data);
        return NULL;
    }
    char *out = buffer_take(&b);
    char *trimmed = trim_in_place(out);
    if (trimmed != out) memmove(out, trimmed, strlen(trimmed) + 1);
    return out;
}


static char *command_with_file_placeholder(const char *cmd, const char *path) {
    char *qpath = path && *path ? shell_quote(path) : xstrdup("");
    TCBuffer out;
    buffer_init(&out);
    const char *p = cmd ? cmd : "";
    bool replaced = false;
    while (*p) {
        if (p[0] == '%' && p[1] == 'f') {
            buffer_append(&out, qpath);
            p += 2;
            replaced = true;
        } else {
            buffer_append_n(&out, p, 1);
            p++;
        }
    }
    if (!replaced && path && *path) {
        if (out.len > 0) buffer_append(&out, " ");
        buffer_append(&out, qpath);
    }
    free(qpath);
    return buffer_take(&out);
}

static char *run_voice_input_command(const TCConfig *cfg, const char *path) {
    if (!cfg->voice_input_cmd || !*cfg->voice_input_cmd) {
        fprintf(stderr, "VOICE_INPUT_CMD is not configured\n");
        return NULL;
    }
    char *cmd = command_with_file_placeholder(cfg->voice_input_cmd, path);
    char *out = read_command_output(cmd);
    free(cmd);
    if (!out || !trim_in_place(out)[0]) {
        free(out);
        fprintf(stderr, "voice input command produced no text\n");
        return NULL;
    }
    return out;
}

static int run_voice_output_command(const TCConfig *cfg, const char *text) {
    if (!cfg->voice_output_cmd || !*cfg->voice_output_cmd) {
        fprintf(stderr, "VOICE_OUTPUT_CMD/TTS_CMD is not configured\n");
        return 1;
    }
    if (strstr(cfg->voice_output_cmd, "%f")) {
        char tmpl[PATH_MAX];
        snprintf(tmpl, sizeof(tmpl), "/tmp/ttychatter-tts-text.XXXXXX");
        int fd = mkstemp(tmpl);
        if (fd < 0) return 1;
        FILE *tf = fdopen(fd, "wb");
        if (!tf) { close(fd); unlink(tmpl); return 1; }
        if (text) fwrite(text, 1, strlen(text), tf);
        fclose(tf);
        char *cmd = command_with_file_placeholder(cfg->voice_output_cmd, tmpl);
        int rc = system(cmd);
        free(cmd);
        unlink(tmpl);
        return rc == 0 ? 0 : 1;
    }
    FILE *p = popen(cfg->voice_output_cmd, "w");
    if (!p) return 1;
    if (text) fwrite(text, 1, strlen(text), p);
    int rc = pclose(p);
    return rc == 0 ? 0 : 1;
}

static char *gpg_decrypt_file(const char *path) {
    if (!path || !file_exists(path) || !command_exists("gpg")) return NULL;
    char *q = shell_quote(path);
    TCBuffer cmd;
    buffer_init(&cmd);
    buffer_appendf(&cmd, "gpg --quiet --decrypt %s 2>/dev/null", q);
    free(q);
    char *cmds = buffer_take(&cmd);
    char *out = read_command_output(cmds);
    free(cmds);
    return out;
}

static bool gpg_encrypt_text_to_file(const char *text, const char *path) {
    if (!command_exists("gpg")) return false;
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "/tmp/ttychatter-key.XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmpl);
        return false;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    chmod(tmpl, 0600);

    char *qin = shell_quote(tmpl);
    char *qout = shell_quote(path);
    TCBuffer cmd;
    buffer_init(&cmd);
    buffer_appendf(&cmd, "gpg --symmetric --cipher-algo AES256 --output %s %s", qout, qin);
    char *cmds = buffer_take(&cmd);
    int rc = system(cmds);
    free(cmds);
    free(qin);
    free(qout);
    unlink(tmpl);
    if (rc == 0) {
        chmod(path, 0600);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Config file handling                                                      */
/* ------------------------------------------------------------------------- */

static char *xdg_config_provider_file(const char *leaf) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter/openrouter");
        char *file = path_join2(dir, leaf);
        free(dir);
        return file;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), ".config/ttychatter/openrouter/%s", leaf);
    return home_path(tmp);
}

static char *xdg_config_file(void) {
    return xdg_config_provider_file("config");
}

static char *legacy_xdg_config_file(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter");
        char *file = path_join2(dir, "config");
        free(dir);
        return file;
    }
    return home_path(".config/ttychatter/config");
}

static char *xdg_api_key_gpg_file(void) {
    return xdg_config_provider_file("api-key.gpg");
}

static char *legacy_xdg_api_key_gpg_file(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter");
        char *file = path_join2(dir, "api-key.gpg");
        free(dir);
        return file;
    }
    return home_path(".config/ttychatter/api-key.gpg");
}

static char *xdg_cache_file(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter/openrouter");
        char *file = path_join2(dir, "models.json");
        free(dir);
        return file;
    }
    return home_path(".cache/ttychatter/openrouter/models.json");
}

static char *xdg_data_dir(const char *leaf) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter/openrouter");
        char *out = path_join2(dir, leaf);
        free(dir);
        return out;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), ".local/share/ttychatter/openrouter/%s", leaf);
    return home_path(tmp);
}

static void config_init_defaults(TCConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_file = xdg_config_file();
    cfg->api_key_gpg_file = xdg_api_key_gpg_file();
    cfg->model = xstrdup("openrouter/auto");
    cfg->session_title_model = xstrdup("openrouter/auto");
    cfg->model_cache_file = xdg_cache_file();
    cfg->model_favorites_file = xdg_config_provider_file("model-favorites");
    cfg->session_dir = xdg_data_dir("sessions");
    cfg->attachment_dir = xdg_data_dir("attachments");
    cfg->context_turns = 12;
    cfg->session_title_max_words = 8;
    cfg->code_attachment_min_lines = 5;
    cfg->max_attachment_bytes = 1024 * 1024;
    cfg->theme = xstrdup("default");
    cfg->send_input = xstrdup("file");
    cfg->editor_cmd = xstrdup("");
    cfg->voice_input_cmd = xstrdup("");
    cfg->voice_output_cmd = xstrdup("");
    cfg->model_test_prompt = xstrdup("Please reply with a short sentence confirming this model is available for text generation.");
    cfg->model_sort_order = xstrdup("name");
    cfg->model_type_filter = xstrdup("all");
    cfg->model_filter_generate_content = true;
    cfg->model_filter_require_tokens = false;
    cfg->model_filter_hide_preview = false;
    cfg->model_min_input_tokens = 0;
    cfg->model_min_output_tokens = 0;
    cfg->confirm_live_send = false;
    cfg->startup_notice = true;
    cfg->session_auto_title = false;
    cfg->stream = false;
    cfg->status_updates = false;
    cfg->voice_output = false;
    cfg->demo_mode_default = false;
    cfg->loopback_default = false;
}

static void config_set(TCConfig *cfg, const char *key, const char *value) {
    if (strcmp(key, "OPENROUTER_API_KEY") == 0 || strcmp(key, "TTYCHATTER_API_KEY") == 0 || strcmp(key, "API_KEY") == 0) {
        free(cfg->api_key);
        cfg->api_key = xstrdup(value);
    } else if (strcmp(key, "API_KEY_GPG_FILE") == 0 || strcmp(key, "OPENROUTER_API_KEY_GPG_FILE") == 0) {
        free(cfg->api_key_gpg_file);
        cfg->api_key_gpg_file = expand_tilde(value);
    } else if (strcmp(key, "MODEL") == 0) {
        free(cfg->model);
        cfg->model = xstrdup(value);
    } else if (strcmp(key, "SESSION_TITLE_MODEL") == 0) {
        free(cfg->session_title_model);
        cfg->session_title_model = xstrdup(value);
    } else if (strcmp(key, "MODEL_CACHE_FILE") == 0) {
        free(cfg->model_cache_file);
        cfg->model_cache_file = expand_tilde(value);
    } else if (strcmp(key, "MODEL_FAVORITES_FILE") == 0) {
        free(cfg->model_favorites_file);
        cfg->model_favorites_file = expand_tilde(value);
    } else if (strcmp(key, "SESSION_DIR") == 0) {
        free(cfg->session_dir);
        cfg->session_dir = expand_tilde(value);
    } else if (strcmp(key, "ATTACHMENT_DIR") == 0) {
        free(cfg->attachment_dir);
        cfg->attachment_dir = expand_tilde(value);
    } else if (strcmp(key, "CONTEXT_TURNS") == 0 || strcmp(key, "HISTORY_LIMIT") == 0) {
        cfg->context_turns = atol(value) > 0 ? atol(value) : cfg->context_turns;
    } else if (strcmp(key, "SESSION_TITLE_MAX_WORDS") == 0) {
        cfg->session_title_max_words = atol(value) > 0 ? atol(value) : cfg->session_title_max_words;
    } else if (strcmp(key, "CODE_ATTACHMENT_MIN_LINES") == 0) {
        cfg->code_attachment_min_lines = atol(value) > 0 ? atol(value) : cfg->code_attachment_min_lines;
    } else if (strcmp(key, "MAX_ATTACHMENT_BYTES") == 0) {
        cfg->max_attachment_bytes = atol(value) > 0 ? atol(value) : cfg->max_attachment_bytes;
    } else if (strcmp(key, "THEME") == 0) {
        free(cfg->theme);
        cfg->theme = xstrdup(value);
    } else if (strcmp(key, "SEND_INPUT") == 0) {
        free(cfg->send_input);
        cfg->send_input = xstrdup(value);
    } else if (strcmp(key, "EDITOR") == 0) {
        free(cfg->editor_cmd);
        cfg->editor_cmd = xstrdup(value);
    } else if (strcmp(key, "VOICE_INPUT_CMD") == 0 || strcmp(key, "TRANSCRIBE_CMD") == 0) {
        free(cfg->voice_input_cmd);
        cfg->voice_input_cmd = xstrdup(value);
    } else if (strcmp(key, "VOICE_OUTPUT_CMD") == 0 || strcmp(key, "TTS_CMD") == 0) {
        free(cfg->voice_output_cmd);
        cfg->voice_output_cmd = xstrdup(value);
    } else if (strcmp(key, "MODEL_TEST_PROMPT") == 0) {
        free(cfg->model_test_prompt);
        cfg->model_test_prompt = xstrdup(value);
    } else if (strcmp(key, "MODEL_SORT_ORDER") == 0) {
        free(cfg->model_sort_order);
        cfg->model_sort_order = xstrdup(value);
    } else if (strcmp(key, "MODEL_TYPE_FILTER") == 0) {
        free(cfg->model_type_filter);
        cfg->model_type_filter = xstrdup(value);
    } else if (strcmp(key, "MODEL_FILTER_GENERATE_CONTENT") == 0 || strcmp(key, "MODEL_FILTER_CHAT_COMPLETIONS") == 0) {
        cfg->model_filter_generate_content = parse_bool_string(value, cfg->model_filter_generate_content);
    } else if (strcmp(key, "MODEL_FILTER_REQUIRE_TOKENS") == 0) {
        cfg->model_filter_require_tokens = parse_bool_string(value, cfg->model_filter_require_tokens);
    } else if (strcmp(key, "MODEL_FILTER_HIDE_PREVIEW") == 0) {
        cfg->model_filter_hide_preview = parse_bool_string(value, cfg->model_filter_hide_preview);
    } else if (strcmp(key, "MODEL_MIN_INPUT_TOKENS") == 0) {
        cfg->model_min_input_tokens = atol(value) >= 0 ? atol(value) : cfg->model_min_input_tokens;
    } else if (strcmp(key, "MODEL_MIN_OUTPUT_TOKENS") == 0) {
        cfg->model_min_output_tokens = atol(value) >= 0 ? atol(value) : cfg->model_min_output_tokens;
    } else if (strcmp(key, "CONFIRM_LIVE_SEND") == 0) {
        cfg->confirm_live_send = parse_bool_string(value, cfg->confirm_live_send);
    } else if (strcmp(key, "STARTUP_NOTICE") == 0) {
        cfg->startup_notice = parse_bool_string(value, cfg->startup_notice);
    } else if (strcmp(key, "SESSION_AUTO_TITLE") == 0) {
        cfg->session_auto_title = parse_bool_string(value, cfg->session_auto_title);
    } else if (strcmp(key, "STREAM") == 0) {
        cfg->stream = parse_bool_string(value, cfg->stream);
    } else if (strcmp(key, "STATUS_UPDATES") == 0 || strcmp(key, "PROGRESS_UPDATES") == 0) {
        cfg->status_updates = parse_bool_string(value, cfg->status_updates);
    } else if (strcmp(key, "VOICE_OUTPUT") == 0 || strcmp(key, "TTS") == 0) {
        cfg->voice_output = parse_bool_string(value, cfg->voice_output);
    } else if (strcmp(key, "DEMO_MODE") == 0) {
        cfg->demo_mode_default = parse_bool_string(value, cfg->demo_mode_default);
    } else if (strcmp(key, "LOOPBACK") == 0) {
        cfg->loopback_default = parse_bool_string(value, cfg->loopback_default);
    }
}

static bool config_load_file(TCConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_in_place(line);
        if (!*s || *s == '#') continue;
        if (starts_with(s, "export ")) s = trim_in_place(s + 7);
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_in_place(s);
        char *value = trim_in_place(eq + 1);
        config_set(cfg, key, value);
    }
    fclose(f);
    return true;
}

static void config_load(TCConfig *cfg) {
    bool loaded = config_load_file(cfg, cfg->config_file);

    /*
     * 0.6.1 moves the C client into the shared OpenRouter namespace used by
     * the ncurses, Bash, and Bash+Python clients.  Existing C-only users may
     * still have ~/.config/ttychatter/config, so read that file only as a
     * compatibility fallback when the new provider-specific config is absent.
     * Writes continue to go to cfg->config_file, which is the OpenRouter path.
     */
    char *legacy_config = legacy_xdg_config_file();
    if (!loaded && file_exists(legacy_config)) config_load_file(cfg, legacy_config);
    free(legacy_config);

    {
        const char *k = getenv("OPENROUTER_API_KEY");
        if (!k || !*k) k = getenv("TTYCHATTER_API_KEY");
        if (k && *k) {
            free(cfg->api_key);
            cfg->api_key = xstrdup(k);
        }
    }

    if (!cfg->api_key && cfg->api_key_gpg_file && file_exists(cfg->api_key_gpg_file)) {
        cfg->api_key = gpg_decrypt_file(cfg->api_key_gpg_file);
    }
    if (!cfg->api_key) {
        char *legacy_gpg = legacy_xdg_api_key_gpg_file();
        if (legacy_gpg && (!cfg->api_key_gpg_file || strcmp(legacy_gpg, cfg->api_key_gpg_file) != 0) && file_exists(legacy_gpg)) {
            cfg->api_key = gpg_decrypt_file(legacy_gpg);
        }
        free(legacy_gpg);
    }
}

static void config_write_key_value(const TCConfig *cfg, const char *key, const char *value) {
    char *dircopy = xstrdup(cfg->config_file);
    char *slash = strrchr(dircopy, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dircopy);
    }
    free(dircopy);

    TCBuffer out;
    buffer_init(&out);
    bool replaced = false;
    FILE *f = fopen(cfg->config_file, "r");
    if (f) {
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            char copy[8192];
            snprintf(copy, sizeof(copy), "%s", line);
            char *s = trim_in_place(copy);
            if (starts_with(s, key) && s[strlen(key)] == '=') {
                buffer_appendf(&out, "%s=%s\n", key, value);
                replaced = true;
            } else {
                buffer_append(&out, line);
            }
        }
        fclose(f);
    }
    if (!replaced) buffer_appendf(&out, "%s=%s\n", key, value);
    write_file_mode(cfg->config_file, out.data, 0600);
    free(out.data);
}

static void config_remove_key(const TCConfig *cfg, const char *key) {
    if (!file_exists(cfg->config_file)) return;
    TCBuffer out;
    buffer_init(&out);
    FILE *f = fopen(cfg->config_file, "r");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char copy[8192];
        snprintf(copy, sizeof(copy), "%s", line);
        char *s = trim_in_place(copy);
        if (starts_with(s, key) && s[strlen(key)] == '=') continue;
        buffer_append(&out, line);
    }
    fclose(f);
    write_file_mode(cfg->config_file, out.data, 0600);
    free(out.data);
}


static bool effective_stream(const TCConfig *cfg, const TCArgs *args) {
    if (args && args->stream_set) return args->stream;
    return cfg && cfg->stream;
}

static bool effective_status_updates(const TCConfig *cfg, const TCArgs *args) {
    if (args && args->status_set) return args->status_updates;
    return cfg && cfg->status_updates;
}

static bool effective_speak(const TCConfig *cfg, const TCArgs *args) {
    if (args && args->speak_set) return args->speak;
    return cfg && cfg->voice_output;
}

static void status_update(const TCConfig *cfg, const TCArgs *args, const char *fmt, ...) {
    if (!effective_status_updates(cfg, args)) return;
    fprintf(stderr, "[ttychatter] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------------- */
/* HTTP via libcurl                                                          */
/* ------------------------------------------------------------------------- */

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    TCBuffer *b = userdata;
    buffer_append_n(b, ptr, size * nmemb);
    return size * nmemb;
}

static TCHttpResponse http_request(const char *url, const char *method, const char *api_key, const char *body) {
    TCHttpResponse r = {0, NULL};
    CURL *curl = curl_easy_init();
    if (!curl) die("curl_easy_init failed");
    TCBuffer b;
    buffer_init(&b);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-Title: ttychatter");
    if (api_key && *api_key) {
        TCBuffer auth;
        buffer_init(&auth);
        buffer_appendf(&auth, "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth.data);
        free(auth.data);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ttychatter-c-cli/0.6.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "{}");
    }
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        free(b.data);
        r.status = 0;
        r.body = xstrdup(curl_easy_strerror(rc));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        r.body = buffer_take(&b);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}



typedef struct TCStreamState {
    TCBuffer raw;
    TCBuffer pending;
    TCBuffer text;
    bool display;
    bool done;
} TCStreamState;

static void streamstate_init(TCStreamState *st, bool display) {
    memset(st, 0, sizeof(*st));
    buffer_init(&st->raw);
    buffer_init(&st->pending);
    buffer_init(&st->text);
    st->display = display;
}

static void streamstate_free(TCStreamState *st) {
    free(st->raw.data);
    free(st->pending.data);
    free(st->text.data);
    memset(st, 0, sizeof(*st));
}

static const char *find_event_separator(const char *s) {
    return strstr(s, "\n\n");
}

static void sse_append_delta(TCStreamState *st, const char *delta) {
    if (!delta || !*delta) return;
    buffer_append(&st->text, delta);
    if (st->display) {
        fputs(delta, stdout);
        fflush(stdout);
    }
}

static void sse_handle_json(TCStreamState *st, const char *data) {
    json_object *obj = json_tokener_parse(data);
    if (!obj) return;
    json_object *choices = NULL;
    if (json_object_object_get_ex(obj, "choices", &choices) && json_object_is_type(choices, json_type_array) && json_object_array_length(choices) > 0) {
        json_object *choice = json_object_array_get_idx(choices, 0);
        json_object *delta = NULL;
        if (json_object_object_get_ex(choice, "delta", &delta)) {
            json_object *content = NULL;
            if (json_object_object_get_ex(delta, "content", &content) && !json_object_is_type(content, json_type_null)) {
                sse_append_delta(st, json_object_get_string(content));
            }
        }
        json_object *message = NULL;
        if (json_object_object_get_ex(choice, "message", &message)) {
            json_object *content = NULL;
            if (json_object_object_get_ex(message, "content", &content) && !json_object_is_type(content, json_type_null)) {
                sse_append_delta(st, json_object_get_string(content));
            }
        }
        json_object *text = NULL;
        if (json_object_object_get_ex(choice, "text", &text) && !json_object_is_type(text, json_type_null)) {
            sse_append_delta(st, json_object_get_string(text));
        }
    }
    json_object_put(obj);
}

static void sse_process_event_block(TCStreamState *st, const char *block) {
    TCBuffer data;
    buffer_init(&data);
    char *copy = xstrdup(block ? block : "");
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = trim_in_place(line);
        if (!*t || *t == ':') continue;
        if (starts_with(t, "data:")) {
            char *payload = trim_in_place(t + 5);
            if (data.len > 0) buffer_append(&data, "\n");
            buffer_append(&data, payload);
        }
    }
    free(copy);
    char *payload = trim_in_place(data.data);
    if (strcmp(payload, "[DONE]") == 0) {
        st->done = true;
    } else if (*payload) {
        sse_handle_json(st, payload);
    }
    free(data.data);
}

static void sse_process_pending(TCStreamState *st) {
    const char *sep;
    while ((sep = find_event_separator(st->pending.data)) != NULL) {
        size_t block_len = (size_t)(sep - st->pending.data);
        char *block = xcalloc(block_len + 1, 1);
        memcpy(block, st->pending.data, block_len);
        sse_process_event_block(st, block);
        free(block);
        size_t remove_len = block_len + 2;
        size_t remain = st->pending.len - remove_len;
        memmove(st->pending.data, st->pending.data + remove_len, remain + 1);
        st->pending.len = remain;
    }
}

static size_t curl_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    TCStreamState *st = userdata;
    size_t n = size * nmemb;
    buffer_append_n(&st->raw, ptr, n);
    const char *s = ptr;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\r') buffer_append_n(&st->pending, s + i, 1);
    }
    sse_process_pending(st);
    return n;
}

static TCHttpResponse http_request_stream(const char *url, const char *api_key, const char *body, bool display) {
    TCHttpResponse r = {0, NULL};
    CURL *curl = curl_easy_init();
    if (!curl) die("curl_easy_init failed");
    TCStreamState st;
    streamstate_init(&st, display);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "X-Title: ttychatter");
    if (api_key && *api_key) {
        TCBuffer auth;
        buffer_init(&auth);
        buffer_appendf(&auth, "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth.data);
        free(auth.data);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ttychatter-c-cli/0.6.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "{}");
    CURLcode rc = curl_easy_perform(curl);
    if (st.pending.len > 0) sse_process_event_block(&st, st.pending.data);
    if (rc != CURLE_OK) {
        r.status = 0;
        r.body = xstrdup(curl_easy_strerror(rc));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        if (r.status >= 200 && r.status < 300 && st.text.len > 0) r.body = xstrdup(st.text.data);
        else r.body = xstrdup(st.raw.data ? st.raw.data : "");
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    streamstate_free(&st);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Demo-mode model data                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Demo mode exists so users and maintainers can exercise the command-line
 * workflow without spending OpenRouter requests or needing network access.  It
 * is deliberately implemented at the same layer as the real provider boundary:
 * listing models returns a small local catalog, and chat completion returns a
 * deterministic response that includes both a short code block and a long code
 * block.  That means demo mode can test the code-block threshold and session log
 * plumbing without fake special cases in the caller.
 */
static const char *TC_DEMO_MODELS_JSON =
    "{\"data\":["
    "{\"id\":\"openrouter/auto\",\"name\":\"Demo Auto Router\",\"context_length\":128000,\"top_provider\":{\"max_completion_tokens\":8192}},"
    "{\"id\":\"openrouter/free\",\"name\":\"Demo Free Router\",\"context_length\":64000,\"top_provider\":{\"max_completion_tokens\":4096}},"
    "{\"id\":\"demo/fixed-small\",\"name\":\"Demo Fixed Small\",\"context_length\":16000,\"top_provider\":{\"max_completion_tokens\":2048}},"
    "{\"id\":\"demo/fixed-large\",\"name\":\"Demo Fixed Large\",\"context_length\":256000,\"top_provider\":{\"max_completion_tokens\":16384}}"
    "]}";

static char *demo_chat_response(const char *model, const char *user_text) {
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "Demo response from model %s.\n\n", model ? model : "openrouter/auto");
    buffer_append(&b, "This response was generated locally by ttychatter --demo. No network call was made.\n\n");
    buffer_append(&b, "Short inline code block:\n```sh\necho demo\n```\n\n");
    buffer_append(&b, "Long code block intended to exercise attachment extraction:\n```c\n");
    buffer_append(&b, "#include <stdio.h>\n");
    buffer_append(&b, "int main(void) {\n");
    buffer_append(&b, "    puts(\"ttychatter demo attachment block\");\n");
    buffer_append(&b, "    return 0;\n");
    buffer_append(&b, "}\n```\n\n");
    if (user_text && *user_text) {
        buffer_append(&b, "You sent this input file content:\n");
        buffer_append(&b, user_text);
        buffer_append(&b, "\n");
    }
    return buffer_take(&b);
}

/* ------------------------------------------------------------------------- */
/* Model catalog helpers                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The mature ttychatter clients treat the model catalog as more than a raw list
 * of opaque strings.  OpenRouter entries may be fixed models, routers such as
 * openrouter/auto, free variants, or other provider-specific aliases.  The C
 * client therefore carries enough metadata to sort and filter the list instead
 * of forcing users to scan a raw JSON dump.  This remains line-oriented and
 * scriptable, but it mirrors the same "model browser" concept used by the
 * ncurses version.
 */
typedef struct TCModelRow {
    char *id;
    char *name;
    long context;
    long output;
    bool favorite;
} TCModelRow;

typedef struct TCModelRows {
    TCModelRow *items;
    size_t count;
    size_t cap;
} TCModelRows;

static bool favorite_contains(const TCConfig *cfg, const char *model);


static const char *model_type_of(const char *id) {
    if (!id) return "fixed";
    if (strcmp(id, "openrouter/auto") == 0) return "auto-router";
    if (strcmp(id, "openrouter/free") == 0) return "free-router";
    if (strstr(id, ":free")) return "free-fixed";
    return "fixed";
}

static bool model_type_matches(const char *id, const char *filter) {
    if (!filter || !*filter || strcmp(filter, "all") == 0) return true;
    const char *type = model_type_of(id);
    if (strcmp(filter, "routers") == 0) return strstr(type, "router") != NULL;
    if (strcmp(filter, "fixed") == 0) return strcmp(type, "fixed") == 0 || strcmp(type, "free-fixed") == 0;
    if (strcmp(filter, "free") == 0) return strstr(type, "free") != NULL;
    if (strcmp(filter, "auto") == 0) return strstr(type, "auto") != NULL;
    return true;
}

static bool contains_casefold(const char *hay, const char *needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

static long json_object_get_long_key(json_object *obj, const char *key) {
    json_object *v = NULL;
    if (!obj || !json_object_object_get_ex(obj, key, &v)) return 0;
    if (json_object_is_type(v, json_type_int)) return json_object_get_int64(v);
    if (json_object_is_type(v, json_type_double)) return (long)json_object_get_double(v);
    if (json_object_is_type(v, json_type_string)) return atol(json_object_get_string(v));
    return 0;
}

static long model_output_token_limit(json_object *model) {
    long out = json_object_get_long_key(model, "max_completion_tokens");
    if (out <= 0) out = json_object_get_long_key(model, "max_output_tokens");
    json_object *top_provider = NULL;
    if (out <= 0 && json_object_object_get_ex(model, "top_provider", &top_provider)) {
        out = json_object_get_long_key(top_provider, "max_completion_tokens");
        if (out <= 0) out = json_object_get_long_key(top_provider, "max_output_tokens");
    }
    return out > 0 ? out : 0;
}

static int update_models(const TCConfig *cfg, bool demo) {
    if (demo) {
        write_file_mode(cfg->model_cache_file, TC_DEMO_MODELS_JSON, 0600);
        fprintf(stderr, "wrote demo model cache: %s\n", cfg->model_cache_file);
        return 0;
    }
    TCHttpResponse r = http_request(TC_OPENROUTER_MODELS_URL, "GET", cfg->api_key, NULL);
    if (r.status < 200 || r.status >= 300) {
        fprintf(stderr, "model update failed: HTTP %ld\n%s\n", r.status, r.body ? r.body : "");
        free(r.body);
        return 1;
    }
    write_file_mode(cfg->model_cache_file, r.body, 0600);
    fprintf(stderr, "wrote model cache: %s\n", cfg->model_cache_file);
    free(r.body);
    return 0;
}

static void modelrows_add(TCModelRows *rows, const char *id, const char *name, long context, long output, bool favorite) {
    if (rows->count == rows->cap) {
        rows->cap = rows->cap ? rows->cap * 2 : 32;
        rows->items = realloc(rows->items, rows->cap * sizeof(rows->items[0]));
        if (!rows->items) die("out of memory");
    }
    rows->items[rows->count].id = xstrdup(id ? id : "");
    rows->items[rows->count].name = xstrdup(name ? name : "");
    rows->items[rows->count].context = context;
    rows->items[rows->count].output = output;
    rows->items[rows->count].favorite = favorite;
    rows->count++;
}

static void modelrows_free(TCModelRows *rows) {
    for (size_t i = 0; i < rows->count; i++) {
        free(rows->items[i].id);
        free(rows->items[i].name);
    }
    free(rows->items);
}

static const char *g_model_sort_key = "name";

static int modelrow_cmp(const void *a, const void *b) {
    const TCModelRow *ra = a;
    const TCModelRow *rb = b;
    if (strcmp(g_model_sort_key, "context") == 0 || strcmp(g_model_sort_key, "tokens") == 0) {
        if (ra->context < rb->context) return 1;
        if (ra->context > rb->context) return -1;
        if (ra->output < rb->output) return 1;
        if (ra->output > rb->output) return -1;
        return strcasecmp(ra->id, rb->id);
    }
    if (strcmp(g_model_sort_key, "type") == 0) {
        int t = strcasecmp(model_type_of(ra->id), model_type_of(rb->id));
        if (t) return t;
        return strcasecmp(ra->id, rb->id);
    }
    if (strcmp(g_model_sort_key, "favorites") == 0) {
        if (ra->favorite != rb->favorite) return ra->favorite ? -1 : 1;
        return strcasecmp(ra->id, rb->id);
    }
    return strcasecmp(ra->id, rb->id);
}


static int select_model_command(const TCConfig *cfg, bool demo) {
    /*
     * The Bash/Python clients have a numbered model selector.  The C CLI is not
     * a full-screen program, but a small numbered prompt is still worth having:
     * it lets users choose a model without copying a long model ID by hand.  The
     * selector deliberately reuses the same cache and demo-data path as
     * --models, so it never performs surprise network access.  Users who want a
     * fresh catalog run --update-models first.
     */
    char *json = NULL;
    if (demo) {
        json = xstrdup(TC_DEMO_MODELS_JSON);
    } else {
        if (!file_exists(cfg->model_cache_file)) {
            fprintf(stderr, "No model cache found: %s\n", cfg->model_cache_file);
            fprintf(stderr, "Run: %s --update-models\n", TC_PROGRAM);
            return 1;
        }
        size_t len = 0;
        json = read_file(cfg->model_cache_file, &len);
    }
    json_object *root = json_tokener_parse(json);
    free(json);
    if (!root) die("could not parse model cache JSON");
    json_object *data = NULL;
    if (!json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_array)) {
        json_object_put(root);
        die("model cache does not contain data[]");
    }
    TCModelRows rows = {0};
    size_t n = json_object_array_length(data);
    for (size_t i = 0; i < n; i++) {
        json_object *m = json_object_array_get_idx(data, i);
        json_object *jid = NULL, *jname = NULL, *jctx = NULL;
        json_object_object_get_ex(m, "id", &jid);
        json_object_object_get_ex(m, "name", &jname);
        json_object_object_get_ex(m, "context_length", &jctx);
        const char *id = jid ? json_object_get_string(jid) : "";
        const char *name = jname ? json_object_get_string(jname) : "";
        long ctx = jctx ? json_object_get_int64(jctx) : 0;
        long out = model_output_token_limit(m);
        if (!model_type_matches(id, cfg->model_type_filter)) continue;
        if (cfg->model_filter_require_tokens && (ctx <= 0 || out <= 0)) continue;
        if (cfg->model_min_input_tokens > 0 && ctx < cfg->model_min_input_tokens) continue;
        if (cfg->model_min_output_tokens > 0 && out < cfg->model_min_output_tokens) continue;
        if (cfg->model_filter_hide_preview && (contains_casefold(id, "preview") || contains_casefold(name, "preview") || contains_casefold(id, "experimental") || contains_casefold(name, "experimental"))) continue;
        modelrows_add(&rows, id, name, ctx, out, favorite_contains(cfg, id));
    }
    json_object_put(root);
    g_model_sort_key = cfg->model_sort_order;
    qsort(rows.items, rows.count, sizeof(rows.items[0]), modelrow_cmp);
    if (rows.count == 0) {
        fprintf(stderr, "No models matched current filters.\n");
        modelrows_free(&rows);
        return 1;
    }
    for (size_t i = 0; i < rows.count; i++) printf("%3zu) %-42.42s %-14s ctx=%ld out=%ld\n", i + 1, rows.items[i].id, model_type_of(rows.items[i].id), rows.items[i].context, rows.items[i].output);
    fprintf(stderr, "Select model number: ");
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) { modelrows_free(&rows); return 1; }
    long choice = atol(line);
    if (choice < 1 || (size_t)choice > rows.count) {
        fprintf(stderr, "selection out of range\n");
        modelrows_free(&rows);
        return 1;
    }
    const char *selected = rows.items[(size_t)choice - 1].id;
    fprintf(stderr, "Selected model: %s\n", selected);
    fprintf(stderr, "Save MODEL=%s to config? [y/N] ", selected);
    if (fgets(line, sizeof(line), stdin) && (line[0] == 'y' || line[0] == 'Y')) {
        config_write_key_value(cfg, "MODEL", selected);
        fprintf(stderr, "saved MODEL=%s\n", selected);
    }
    modelrows_free(&rows);
    return 0;
}

static int list_models(const TCConfig *cfg, const char *search, const char *type_filter, const char *sort_key, bool favorites_only, bool demo) {
    char *json = NULL;
    if (demo) {
        json = xstrdup(TC_DEMO_MODELS_JSON);
    } else {
        if (!file_exists(cfg->model_cache_file)) {
            fprintf(stderr, "No model cache found: %s\n", cfg->model_cache_file);
            fprintf(stderr, "Run: %s --update-models\n", TC_PROGRAM);
            return 1;
        }
        size_t len = 0;
        json = read_file(cfg->model_cache_file, &len);
    }

    json_object *root = json_tokener_parse(json);
    free(json);
    if (!root) die("could not parse model cache JSON");
    json_object *data = NULL;
    if (!json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_array)) {
        json_object_put(root);
        die("model cache does not contain data[]");
    }

    TCModelRows rows = {0};
    size_t n = json_object_array_length(data);
    for (size_t i = 0; i < n; i++) {
        json_object *m = json_object_array_get_idx(data, i);
        json_object *jid = NULL, *jname = NULL, *jctx = NULL;
        json_object_object_get_ex(m, "id", &jid);
        json_object_object_get_ex(m, "name", &jname);
        json_object_object_get_ex(m, "context_length", &jctx);
        const char *id = jid ? json_object_get_string(jid) : "";
        const char *name = jname ? json_object_get_string(jname) : "";
        long ctx = jctx ? json_object_get_int64(jctx) : 0;
        long out = model_output_token_limit(m);
        bool fav = favorite_contains(cfg, id);
        const char *effective_type = (type_filter && *type_filter) ? type_filter : cfg->model_type_filter;
        if (!model_type_matches(id, effective_type)) continue;
        if (favorites_only && !fav) continue;
        if (cfg->model_filter_require_tokens && (ctx <= 0 || out <= 0)) continue;
        if (cfg->model_min_input_tokens > 0 && ctx < cfg->model_min_input_tokens) continue;
        if (cfg->model_min_output_tokens > 0 && out < cfg->model_min_output_tokens) continue;
        if (cfg->model_filter_hide_preview && (contains_casefold(id, "preview") || contains_casefold(name, "preview") || contains_casefold(id, "experimental") || contains_casefold(name, "experimental"))) continue;
        if (search && *search && !contains_casefold(id, search) && !contains_casefold(name, search)) continue;
        modelrows_add(&rows, id, name, ctx, out, fav);
    }
    json_object_put(root);

    g_model_sort_key = sort_key && *sort_key ? sort_key : cfg->model_sort_order;
    qsort(rows.items, rows.count, sizeof(rows.items[0]), modelrow_cmp);

    printf("%-2s %-42s %-14s %-10s %-10s %s\n", "F", "MODEL", "TYPE", "CONTEXT", "OUTPUT", "NAME");
    printf("%-2s %-42s %-14s %-10s %-10s %s\n", "-", "-----", "----", "-------", "------", "----");
    for (size_t i = 0; i < rows.count; i++) {
        printf("%-2s %-42.42s %-14s %-10ld %-10ld %s\n",
               rows.items[i].favorite ? "*" : "",
               rows.items[i].id,
               model_type_of(rows.items[i].id),
               rows.items[i].context,
               rows.items[i].output,
               rows.items[i].name);
    }
    modelrows_free(&rows);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Session parsing and context reconstruction                                */
/* ------------------------------------------------------------------------- */

static void msglist_add(TCMessageList *list, const char *role, const char *content) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = realloc(list->items, list->cap * sizeof(list->items[0]));
        if (!list->items) die("out of memory");
    }
    list->items[list->count].role = xstrdup(role ? role : "user");
    list->items[list->count].content = xstrdup(content ? content : "");
    list->count++;
}

static void msglist_free(TCMessageList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].role);
        free(list->items[i].content);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static TCMessageList msglist_clip_take(TCMessageList *all, long context_turns) {
    long max_messages = context_turns * 2;
    if (max_messages < 2) max_messages = 2;
    if ((long)all->count <= max_messages) {
        TCMessageList keep = *all;
        all->items = NULL;
        all->count = 0;
        all->cap = 0;
        return keep;
    }

    TCMessageList clipped = {0};
    size_t start = all->count - (size_t)max_messages;
    for (size_t i = start; i < all->count; i++) {
        msglist_add(&clipped, all->items[i].role, all->items[i].content);
    }
    msglist_free(all);
    return clipped;
}

static char *snapshot_safe_line(const char *s) {
    TCBuffer b;
    buffer_init(&b);
    for (const char *p = s ? s : ""; *p; p++) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        buffer_append_n(&b, &c, 1);
    }
    return buffer_take(&b);
}

typedef struct TCLogEntry {
    char *speaker;
    TCBuffer text;
} TCLogEntry;

typedef struct TCLogEntryList {
    TCLogEntry *items;
    size_t count;
    size_t cap;
} TCLogEntryList;

static void logentries_add(TCLogEntryList *list, const char *speaker, const char *text) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->items = realloc(list->items, list->cap * sizeof(list->items[0]));
        if (!list->items) die("out of memory");
    }
    list->items[list->count].speaker = xstrdup(speaker ? speaker : "");
    buffer_init(&list->items[list->count].text);
    buffer_append(&list->items[list->count].text, text ? text : "");
    list->count++;
}

static void logentries_free(TCLogEntryList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].speaker);
        free(list->items[i].text.data);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static bool parse_shared_log_header(const char *line, char *speaker, size_t speaker_size, const char **content_out) {
    size_t n = strlen(line);
    if (n < 12) return false;
    if (line[0] != '[' || line[3] != ':' || line[6] != ':' || line[9] != ']' || line[10] != ' ') return false;
    if (!isdigit((unsigned char)line[1]) || !isdigit((unsigned char)line[2]) ||
        !isdigit((unsigned char)line[4]) || !isdigit((unsigned char)line[5]) ||
        !isdigit((unsigned char)line[7]) || !isdigit((unsigned char)line[8])) return false;
    const char *sp = line + 11;
    const char *colon = strchr(sp, ':');
    if (!colon || colon == sp) return false;
    size_t slen = (size_t)(colon - sp);
    if (slen >= speaker_size) slen = speaker_size - 1;
    memcpy(speaker, sp, slen);
    speaker[slen] = '\0';
    const char *content = colon + 1;
    if (*content == ' ') content++;
    *content_out = content;
    return true;
}

static TCLogEntryList parse_shared_log_entries(const char *text) {
    TCLogEntryList entries = {0};
    const char *p = text ? text : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = xcalloc(n + 1, 1);
        memcpy(line, p, n);
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';

        char speaker[256];
        const char *content = NULL;
        if (parse_shared_log_header(line, speaker, sizeof(speaker), &content)) {
            logentries_add(&entries, speaker, content);
        } else if (entries.count > 0) {
            buffer_append(&entries.items[entries.count - 1].text, "\n");
            buffer_append(&entries.items[entries.count - 1].text, line);
        }
        free(line);
        if (!e) break;
        p = e + 1;
    }
    return entries;
}

static const char *role_from_shared_speaker(const char *speaker) {
    if (!speaker || !*speaker) return NULL;
    if (strcasecmp(speaker, "system") == 0) return NULL;
    if (strcasecmp(speaker, "AI") == 0 || strcasecmp(speaker, "OpenRouter") == 0 || strcasecmp(speaker, "Assistant") == 0) return "assistant";
    return "user";
}

static bool parse_context_snapshot_text(const char *text, TCMessageList *out) {
    bool saw_begin = false;
    const char *p = text ? text : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = xcalloc(n + 1, 1);
        memcpy(line, p, n);
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';
        char *t = trim_in_place(line);

        if (!saw_begin) {
            if (strcmp(t, "CONTEXT_BEGIN") == 0 || strcmp(t, TC_CONTEXT_BEGIN) == 0) {
                saw_begin = true;
                free(line);
                if (!e) break;
                p = e + 1;
                continue;
            }
            free(line);
            return false;
        }

        if (strcmp(t, "CONTEXT_END") == 0 || strcmp(t, TC_CONTEXT_END) == 0) {
            free(line);
            break;
        }
        if (starts_with(t, "User: ")) msglist_add(out, "user", t + 6);
        else if (starts_with(t, "user: ")) msglist_add(out, "user", t + 6);
        else if (starts_with(t, "Assistant: ")) msglist_add(out, "assistant", t + 11);
        else if (starts_with(t, "assistant: ")) msglist_add(out, "assistant", t + 11);

        free(line);
        if (!e) break;
        p = e + 1;
    }
    return saw_begin;
}

static TCMessageList load_context_from_shared_entries(TCLogEntryList *entries, long context_turns, bool *parsed_shared) {
    *parsed_shared = entries->count > 0;

    for (size_t rev = entries->count; rev > 0; rev--) {
        TCLogEntry *entry = &entries->items[rev - 1];
        if (strcasecmp(entry->speaker, "system") != 0) continue;
        TCMessageList snap = {0};
        if (parse_context_snapshot_text(entry->text.data, &snap)) {
            return msglist_clip_take(&snap, context_turns);
        }
    }

    TCMessageList all = {0};
    for (size_t i = 0; i < entries->count; i++) {
        const char *role = role_from_shared_speaker(entries->items[i].speaker);
        if (!role) continue;
        msglist_add(&all, role, entries->items[i].text.data);
    }
    return msglist_clip_take(&all, context_turns);
}

static char *extract_between(const char *start, const char *begin_marker, const char *end_marker, const char **after) {
    const char *b = strstr(start, begin_marker);
    if (!b) return NULL;
    b += strlen(begin_marker);
    if (*b == '\r') b++;
    if (*b == '\n') b++;
    const char *e = strstr(b, end_marker);
    if (!e) return NULL;
    size_t n = (size_t)(e - b);
    char *out = xcalloc(n + 1, 1);
    memcpy(out, b, n);
    out[n] = '\0';
    if (after) *after = e + strlen(end_marker);
    return out;
}

static TCMessageList load_context_from_legacy_c_log(const char *text, long context_turns) {
    TCMessageList all = {0};

    const char *last_snapshot = NULL;
    const char *scan_snap = text;
    while ((scan_snap = strstr(scan_snap, TC_CONTEXT_BEGIN))) {
        last_snapshot = scan_snap;
        scan_snap += strlen(TC_CONTEXT_BEGIN);
    }
    if (last_snapshot) {
        const char *body = last_snapshot + strlen(TC_CONTEXT_BEGIN);
        const char *end = strstr(body, TC_CONTEXT_END);
        if (end) {
            TCMessageList snap = {0};
            char *chunk = xcalloc((size_t)(end - body) + 1, 1);
            memcpy(chunk, body, (size_t)(end - body));
            char *save = NULL;
            for (char *line = strtok_r(chunk, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char *t = trim_in_place(line);
                if (starts_with(t, "user: ")) msglist_add(&snap, "user", t + 6);
                else if (starts_with(t, "assistant: ")) msglist_add(&snap, "assistant", t + 11);
                else if (starts_with(t, "User: ")) msglist_add(&snap, "user", t + 6);
                else if (starts_with(t, "Assistant: ")) msglist_add(&snap, "assistant", t + 11);
            }
            free(chunk);
            return msglist_clip_take(&snap, context_turns);
        }
    }

    const char *start_scan = text;
    const char *last_clear = NULL;
    const char *scan_clear = text;
    while ((scan_clear = strstr(scan_clear, TC_MEMORY_CLEAR))) {
        last_clear = scan_clear;
        scan_clear += strlen(TC_MEMORY_CLEAR);
    }
    if (last_clear) start_scan = last_clear + strlen(TC_MEMORY_CLEAR);

    const char *p = start_scan;
    while ((p = strstr(p, TC_TURN_BEGIN))) {
        const char *turn_end = strstr(p, TC_TURN_END);
        if (!turn_end) break;
        char *user = extract_between(p, TC_USER_BEGIN, TC_USER_END, NULL);
        char *ai = extract_between(p, TC_AI_BEGIN, TC_AI_END, NULL);
        if (user) msglist_add(&all, "user", trim_in_place(user));
        if (ai) msglist_add(&all, "assistant", trim_in_place(ai));
        free(user);
        free(ai);
        p = turn_end + strlen(TC_TURN_END);
    }
    return msglist_clip_take(&all, context_turns);
}

static TCMessageList load_context_from_session(const char *output_path, long context_turns) {
    TCMessageList empty = {0};
    if (!file_exists(output_path)) return empty;
    size_t len = 0;
    char *text = read_file(output_path, &len);
    (void)len;

    /*
     * ttychatter's OpenRouter clients use a shared, line-oriented transcript:
     *     [HH:MM:SS] User: ...
     *     [HH:MM:SS] AI: ...
     *     [HH:MM:SS] system: CONTEXT_BEGIN
     *     User: ...
     *     Assistant: ...
     *     [HH:MM:SS] system: CONTEXT_END
     * 0.6.1 makes the C client read that format first, then falls back to the
     * earlier C marker blocks so existing C-only session logs remain resumable.
     */
    TCLogEntryList entries = parse_shared_log_entries(text);
    bool parsed_shared = false;
    TCMessageList shared = load_context_from_shared_entries(&entries, context_turns, &parsed_shared);
    logentries_free(&entries);
    if (parsed_shared) {
        free(text);
        return shared;
    }
    msglist_free(&shared);

    TCMessageList legacy = load_context_from_legacy_c_log(text, context_turns);
    free(text);
    return legacy;
}

/* ------------------------------------------------------------------------- */
/* Attachment handling                                                       */
/* ------------------------------------------------------------------------- */

static const char *mime_from_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".log") == 0) return "text/plain";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".csv") == 0) return "text/csv";
    if (strcasecmp(dot, ".c") == 0 || strcasecmp(dot, ".h") == 0 || strcasecmp(dot, ".sh") == 0 || strcasecmp(dot, ".py") == 0) return "text/plain";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

static bool is_text_mime(const char *mime) {
    return starts_with(mime, "text/") || strcmp(mime, "application/json") == 0 || strcmp(mime, "text/csv") == 0;
}

static bool is_image_mime(const char *mime) {
    return starts_with(mime, "image/");
}

static void add_attachments_to_user_content(TCBuffer *text_part, json_object *parts, const TCConfig *cfg, const TCArgs *args) {
    for (size_t i = 0; i < args->attachment_count; i++) {
        const char *path = args->attachments[i];
        long sz = file_size(path);
        const char *mime = mime_from_ext(path);
        if (sz < 0) {
            buffer_appendf(text_part, "\n\n[Attachment not found: %s]\n", path);
            continue;
        }
        if (sz > cfg->max_attachment_bytes) {
            buffer_appendf(text_part, "\n\n[Attachment too large for inline send: %s (%ld bytes)]\n", path, sz);
            continue;
        }
        size_t len = 0;
        char *data = read_file(path, &len);
        if (is_text_mime(mime)) {
            buffer_appendf(text_part, "\n\n----- ATTACHED FILE: %s (%s) -----\n", path, mime);
            buffer_append_n(text_part, data, len);
            buffer_appendf(text_part, "\n----- END ATTACHED FILE: %s -----\n", path);
        } else if (is_image_mime(mime)) {
            char *b64 = base64_encode((const unsigned char *)data, len);
            TCBuffer url;
            buffer_init(&url);
            buffer_appendf(&url, "data:%s;base64,%s", mime, b64);
            json_object *part = json_object_new_object();
            json_object_object_add(part, "type", json_object_new_string("image_url"));
            json_object *iu = json_object_new_object();
            json_object_object_add(iu, "url", json_object_new_string(url.data));
            json_object_object_add(part, "image_url", iu);
            json_object_array_add(parts, part);
            buffer_appendf(text_part, "\n\n[Image attachment sent: %s (%s, %ld bytes)]\n", path, mime, sz);
            free(b64);
            free(url.data);
        } else {
            buffer_appendf(text_part, "\n\n[Attachment listed but not embedded: %s (%s, %ld bytes)]\n", path, mime, sz);
        }
        free(data);
    }
}

/* ------------------------------------------------------------------------- */
/* Code fence extraction                                                     */
/* ------------------------------------------------------------------------- */

static const char *ext_for_lang(const char *lang) {
    if (!lang || !*lang) return ".txt";
    if (strcasecmp(lang, "bash") == 0 || strcasecmp(lang, "sh") == 0 || strcasecmp(lang, "shell") == 0) return ".sh";
    if (strcasecmp(lang, "python") == 0 || strcasecmp(lang, "py") == 0) return ".py";
    if (strcasecmp(lang, "json") == 0) return ".json";
    if (strcasecmp(lang, "yaml") == 0 || strcasecmp(lang, "yml") == 0) return ".yml";
    if (strcasecmp(lang, "c") == 0) return ".c";
    if (strcasecmp(lang, "cpp") == 0 || strcasecmp(lang, "c++") == 0) return ".cpp";
    return ".txt";
}

static long count_lines(const char *s, size_t n) {
    long lines = 1;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') lines++;
    return lines;
}

static char *clean_lang(const char *s, size_t n) {
    TCBuffer b;
    buffer_init(&b);
    for (size_t i = 0; i < n && i < 32; i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '+') buffer_append_n(&b, &c, 1);
    }
    if (b.len == 0) buffer_append(&b, "text");
    return buffer_take(&b);
}

static char *extract_code_blocks(const TCConfig *cfg, const char *session_base, const char *raw) {
    TCBuffer out;
    buffer_init(&out);
    const char *p = raw;
    int counter = 1;
    while (*p) {
        const char *open = strstr(p, "```");
        if (!open) {
            buffer_append(&out, p);
            break;
        }
        buffer_append_n(&out, p, (size_t)(open - p));
        const char *lang_start = open + 3;
        const char *line_end = strchr(lang_start, '\n');
        if (!line_end) {
            buffer_append(&out, open);
            break;
        }
        const char *close = strstr(line_end + 1, "```");
        if (!close) {
            buffer_append(&out, open);
            break;
        }
        char *lang = clean_lang(lang_start, (size_t)(line_end - lang_start));
        const char *code = line_end + 1;
        size_t code_len = (size_t)(close - code);
        if (count_lines(code, code_len) >= cfg->code_attachment_min_lines) {
            mkdir_p(cfg->attachment_dir);
            char filename[PATH_MAX];
            snprintf(filename, sizeof(filename), "%s/%s-code-%02d%s", cfg->attachment_dir, session_base, counter++, ext_for_lang(lang));
            FILE *f = fopen(filename, "wb");
            if (f) {
                fwrite(code, 1, code_len, f);
                fclose(f);
                buffer_appendf(&out, "\n[code attachment saved: %s]\n", filename);
            } else {
                buffer_append_n(&out, open, (size_t)(close + 3 - open));
            }
        } else {
            buffer_append_n(&out, open, (size_t)(close + 3 - open));
        }
        free(lang);
        p = close + 3;
    }
    return buffer_take(&out);
}

/* ------------------------------------------------------------------------- */
/* OpenRouter chat request                                                   */
/* ------------------------------------------------------------------------- */

static char *openrouter_chat(const TCConfig *cfg, const char *model, const char *user_text, const TCMessageList *context, const TCArgs *args) {
    bool stream = effective_stream(cfg, args);
    status_update(cfg, args, "preparing request for %s", model ? model : "(default model)");
    if (args && args->demo) {
        char *demo = demo_chat_response(model, user_text);
        if (stream) {
            fputs(demo, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
        return demo;
    }
    json_object *root = json_object_new_object();
    json_object_object_add(root, "model", json_object_new_string(model));
    json_object *messages = json_object_new_array();

    for (size_t i = 0; i < context->count; i++) {
        json_object *msg = json_object_new_object();
        json_object_object_add(msg, "role", json_object_new_string(context->items[i].role));
        json_object_object_add(msg, "content", json_object_new_string(context->items[i].content));
        json_object_array_add(messages, msg);
    }

    json_object *user = json_object_new_object();
    json_object_object_add(user, "role", json_object_new_string("user"));

    if (args->attachment_count == 0) {
        json_object_object_add(user, "content", json_object_new_string(user_text));
    } else {
        json_object *parts = json_object_new_array();
        TCBuffer text;
        buffer_init(&text);
        buffer_append(&text, user_text);
        add_attachments_to_user_content(&text, parts, cfg, args);
        json_object *textpart = json_object_new_object();
        json_object_object_add(textpart, "type", json_object_new_string("text"));
        json_object_object_add(textpart, "text", json_object_new_string(text.data));
        json_object_array_add(parts, textpart);
        json_object_object_add(user, "content", parts);
        free(text.data);
    }

    json_object_array_add(messages, user);
    json_object_object_add(root, "messages", messages);
    if (stream) json_object_object_add(root, "stream", json_object_new_boolean(1));
    const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    char *payload_copy = xstrdup(payload);
    json_object_put(root);

    status_update(cfg, args, stream ? "waiting for streaming response" : "waiting for response");
    TCHttpResponse r = stream ? http_request_stream(TC_OPENROUTER_CHAT_URL, cfg->api_key, payload_copy, true)
                              : http_request(TC_OPENROUTER_CHAT_URL, "POST", cfg->api_key, payload_copy);
    free(payload_copy);
    if (stream && r.status >= 200 && r.status < 300) {
        fputc('\n', stdout);
        fflush(stdout);
    }
    if (r.status < 200 || r.status >= 300) {
        TCBuffer err;
        buffer_init(&err);
        buffer_appendf(&err, "OpenRouter HTTP %ld\n%s", r.status, r.body ? r.body : "");
        free(r.body);
        return buffer_take(&err);
    }
    if (stream) {
        status_update(cfg, args, "stream complete");
        return r.body;
    }
    json_object *resp = json_tokener_parse(r.body);
    if (!resp) {
        char *bad = r.body;
        return bad;
    }
    free(r.body);
    json_object *choices = NULL;
    if (!json_object_object_get_ex(resp, "choices", &choices) || !json_object_is_type(choices, json_type_array) || json_object_array_length(choices) == 0) {
        json_object_put(resp);
        return xstrdup("[No choices returned by OpenRouter]");
    }
    json_object *choice = json_object_array_get_idx(choices, 0);
    json_object *msg = NULL, *content = NULL;
    if (!json_object_object_get_ex(choice, "message", &msg) || !json_object_object_get_ex(msg, "content", &content)) {
        json_object_put(resp);
        return xstrdup("[No message.content returned by OpenRouter]");
    }
    char *out = xstrdup(json_object_get_string(content));
    json_object_put(resp);
    status_update(cfg, args, "response complete");
    return out;
}

/* ------------------------------------------------------------------------- */
/* Session log append                                                        */
/* ------------------------------------------------------------------------- */

static void append_snapshot_message(TCBuffer *b, const char *role, const char *content) {
    const char *label = (role && strcasecmp(role, "assistant") == 0) ? "Assistant" : "User";
    char *safe = snapshot_safe_line(content);
    buffer_appendf(b, "%s: %s\n", label, safe);
    free(safe);
}

static void append_shared_context_snapshot(TCBuffer *b, const char *ts, const TCMessageList *previous_context,
                                           long context_turns, const char *user_text, const char *ai_text) {
    long max_messages = context_turns * 2;
    if (max_messages < 2) max_messages = 2;
    size_t prev_count = previous_context ? previous_context->count : 0;
    size_t drop = 0;
    if ((long)(prev_count + 2) > max_messages) drop = (prev_count + 2) - (size_t)max_messages;
    if (drop > prev_count) drop = prev_count;

    buffer_appendf(b, "[%s] system: CONTEXT_BEGIN\n", ts);
    for (size_t i = drop; i < prev_count; i++) {
        append_snapshot_message(b, previous_context->items[i].role, previous_context->items[i].content);
    }
    append_snapshot_message(b, "user", user_text);
    append_snapshot_message(b, "assistant", ai_text);
    buffer_appendf(b, "[%s] system: CONTEXT_END\n", ts);
}

static void append_turn(const char *output_path, const char *model, const char *input_path, TCArgs *args,
                        const char *user_text, const char *ai_text, const TCMessageList *previous_context,
                        long context_turns) {
    TCBuffer b;
    buffer_init(&b);
    char *ts = now_hms();

    if (model && *model) buffer_appendf(&b, "[%s] system: model: %s\n", ts, model);
    if (input_path && *input_path) buffer_appendf(&b, "[%s] system: input-file: %s\n", ts, input_path);
    if (args) {
        for (size_t i = 0; i < args->attachment_count; i++) {
            buffer_appendf(&b, "[%s] system: attachment: %s\n", ts, args->attachments[i]);
        }
    }

    buffer_appendf(&b, "[%s] User: %s\n", ts, user_text ? user_text : "");
    buffer_appendf(&b, "[%s] AI: %s\n", ts, ai_text ? ai_text : "");
    append_shared_context_snapshot(&b, ts, previous_context, context_turns, user_text ? user_text : "", ai_text ? ai_text : "");
    buffer_append(&b, "\n");

    append_file_text(output_path, b.data);
    free(ts);
    free(b.data);
}


static char *session_title_clean(const char *raw, long max_words) {
    if (!raw) return NULL;

    TCBuffer b;
    buffer_init(&b);
    bool prev_space = false;
    for (const char *p = raw; *p; p++) {
        unsigned char uc = (unsigned char)*p;
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (iscntrl(uc)) continue;
        if (isspace((unsigned char)c)) {
            if (!prev_space && b.len > 0) buffer_append(&b, " ");
            prev_space = true;
        } else {
            buffer_append_n(&b, &c, 1);
            prev_space = false;
        }
    }

    char *out = buffer_take(&b);
    char *t = trim_in_place(out);
    if (starts_with_casefold(t, "session title:")) t = trim_in_place(t + strlen("session title:"));
    else if (starts_with_casefold(t, "title:")) t = trim_in_place(t + strlen("title:"));

    while (*t == '"' || *t == '\'' || *t == '`' || *t == '*' || *t == '-') t++;
    char *end = t + strlen(t);
    while (end > t && (isspace((unsigned char)end[-1]) || end[-1] == '"' || end[-1] == '\'' || end[-1] == '`' || end[-1] == '*' || end[-1] == '.' || end[-1] == '-')) end--;
    *end = '\0';

    if (max_words <= 0) max_words = 8;
    long words = 0;
    bool in_word = false;
    for (char *p = t; *p; p++) {
        if (isspace((unsigned char)*p)) {
            in_word = false;
            continue;
        }
        if (!in_word) {
            words++;
            if (words > max_words) {
                char *cut = p;
                while (cut > t && !isspace((unsigned char)cut[-1])) cut--;
                *cut = '\0';
                break;
            }
            in_word = true;
        }
    }

    size_t max_chars = 96;
    if (strlen(t) > max_chars) {
        char *cut = t + max_chars;
        while (cut > t && !isspace((unsigned char)*cut)) cut--;
        if (cut == t) cut = t + max_chars;
        *cut = '\0';
    }
    t = trim_in_place(t);
    if (!*t) {
        free(out);
        return NULL;
    }
    if (t != out) memmove(out, t, strlen(t) + 1);
    return out;
}

static char *session_title_from_file(const char *path) {
    if (!path || !file_exists(path)) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[8192];
    char *title = NULL;
    while (fgets(line, sizeof(line), f)) {
        char copy[8192];
        snprintf(copy, sizeof(copy), "%s", line);
        char speaker[256];
        const char *content = NULL;
        if (parse_shared_log_header(copy, speaker, sizeof(speaker), &content) && strcasecmp(speaker, "system") == 0) {
            if (starts_with_casefold(content, "session-title:")) {
                char *clean = session_title_clean(content + strlen("session-title:"), 64);
                if (clean) {
                    free(title);
                    title = clean;
                }
            }
            continue;
        }
        char *t = trim_in_place(copy);
        if (starts_with_casefold(t, "session-title:")) {
            char *clean = session_title_clean(t + strlen("session-title:"), 64);
            if (clean) {
                free(title);
                title = clean;
            }
        }
    }
    fclose(f);
    return title;
}

static bool session_has_title(const char *path) {
    char *title = session_title_from_file(path);
    bool ok = title && *title;
    free(title);
    return ok;
}

static void write_session_title_metadata(const char *path, const char *title) {
    char *clean = session_title_clean(title, 64);
    if (!path || !clean || !*clean) {
        free(clean);
        return;
    }
    TCBuffer b;
    buffer_init(&b);
    char *ts = now_hms();
    buffer_appendf(&b, "[%s] system: session-title: %s\n", ts, clean);
    append_file_text(path, b.data);
    free(ts);
    free(clean);
    free(b.data);
}

static void append_title_excerpt(TCBuffer *b, const char *label, const char *text, size_t max_chars) {
    char *safe = snapshot_safe_line(text ? text : "");
    if (strlen(safe) > max_chars) safe[max_chars] = '\0';
    buffer_appendf(b, "%s: %s\n", label, safe);
    free(safe);
}

static char *demo_session_title(const char *user_text, long max_words) {
    char *clean = session_title_clean(user_text, max_words);
    if (clean) return clean;
    return xstrdup("Demo Session");
}

static char *generate_session_title(const TCConfig *cfg, const char *user_text, const char *ai_text, bool demo) {
    if (!cfg->session_auto_title) return NULL;
    if (demo) return demo_session_title(user_text, cfg->session_title_max_words);
    if (!cfg->api_key || !cfg->session_title_model || !*cfg->session_title_model) return NULL;

    TCBuffer prompt;
    buffer_init(&prompt);
    buffer_appendf(&prompt, "Generate a concise topical title for this ttychatter session. Reply with only the title, no quotes, no punctuation-only decoration, no explanation, maximum %ld words.\n\n", cfg->session_title_max_words > 0 ? cfg->session_title_max_words : 8);
    append_title_excerpt(&prompt, "User", user_text, 2000);
    append_title_excerpt(&prompt, "Assistant", ai_text, 2000);

    TCMessageList ctx = {0};
    TCArgs title_args;
    memset(&title_args, 0, sizeof(title_args));
    title_args.model_type = xstrdup("all");
    title_args.stream_set = true;
    title_args.stream = false;
    title_args.status_set = true;
    title_args.status_updates = false;
    title_args.speak_set = true;
    title_args.speak = false;
    char *reply = openrouter_chat(cfg, cfg->session_title_model, prompt.data, &ctx, &title_args);
    free(prompt.data);
    free(title_args.model_type);
    msglist_free(&ctx);

    if (!reply) return NULL;
    if (starts_with(reply, "OpenRouter HTTP") || starts_with(reply, "[No ")) {
        free(reply);
        return NULL;
    }
    char *title = session_title_clean(reply, cfg->session_title_max_words);
    free(reply);
    return title;
}

static void maybe_generate_session_title(const TCConfig *cfg, const TCArgs *args, const char *output_path,
                                         const char *user_text, const char *ai_text) {
    if (!cfg->session_auto_title || !output_path || !*output_path) return;
    if (session_has_title(output_path)) return;
    char *title = generate_session_title(cfg, user_text, ai_text, args && args->demo);
    if (title && *title) write_session_title_metadata(output_path, title);
    free(title);
}


/* ------------------------------------------------------------------------- */
/* Config inspection and model favorites                                     */
/* ------------------------------------------------------------------------- */

/*
 * These helper commands keep the C edition useful as a real Unix tool rather
 * than merely a single-shot network wrapper.  They also preserve feature parity
 * with the ncurses and shell clients where that parity makes sense on a plain
 * command line: users can inspect config, set/unset config keys, and maintain a
 * simple model favorites file.  The favorites file is intentionally just one
 * model ID per line so it remains editable with vi, ed, awk, sed, or any other
 * traditional Unix text tool.
 */
static void print_config_summary(const TCConfig *cfg) {
    printf("config_file=%s\n", cfg->config_file);
    printf("model=%s\n", cfg->model);
    printf("session_auto_title=%s\n", cfg->session_auto_title ? "1" : "0");
    printf("session_title_model=%s\n", cfg->session_title_model);
    printf("session_title_max_words=%ld\n", cfg->session_title_max_words);
    printf("model_cache_file=%s\n", cfg->model_cache_file);
    printf("model_favorites_file=%s\n", cfg->model_favorites_file);
    printf("session_dir=%s\n", cfg->session_dir);
    printf("attachment_dir=%s\n", cfg->attachment_dir);
    printf("api_key=%s\n", cfg->api_key ? "loaded" : "missing");
    printf("api_key_gpg_file=%s\n", cfg->api_key_gpg_file);
    printf("context_turns=%ld\n", cfg->context_turns);
    printf("code_attachment_min_lines=%ld\n", cfg->code_attachment_min_lines);
    printf("max_attachment_bytes=%ld\n", cfg->max_attachment_bytes);
    printf("theme=%s\n", cfg->theme);
    printf("send_input=%s\n", cfg->send_input);
    printf("editor=%s\n", cfg->editor_cmd);
    printf("voice_input_cmd=%s\n", cfg->voice_input_cmd);
    printf("voice_output_cmd=%s\n", cfg->voice_output_cmd);
    printf("model_test_prompt=%s\n", cfg->model_test_prompt);
    printf("model_sort_order=%s\n", cfg->model_sort_order);
    printf("model_type_filter=%s\n", cfg->model_type_filter);
    printf("model_filter_generate_content=%s\n", cfg->model_filter_generate_content ? "1" : "0");
    printf("model_filter_require_tokens=%s\n", cfg->model_filter_require_tokens ? "1" : "0");
    printf("model_filter_hide_preview=%s\n", cfg->model_filter_hide_preview ? "1" : "0");
    printf("model_min_input_tokens=%ld\n", cfg->model_min_input_tokens);
    printf("model_min_output_tokens=%ld\n", cfg->model_min_output_tokens);
    printf("confirm_live_send=%s\n", cfg->confirm_live_send ? "1" : "0");
    printf("startup_notice=%s\n", cfg->startup_notice ? "1" : "0");
    printf("stream=%s\n", cfg->stream ? "1" : "0");
    printf("status_updates=%s\n", cfg->status_updates ? "1" : "0");
    printf("voice_output=%s\n", cfg->voice_output ? "1" : "0");
    printf("demo_mode=%s\n", cfg->demo_mode_default ? "1" : "0");
    printf("loopback=%s\n", cfg->loopback_default ? "1" : "0");
}

static bool favorite_contains(const TCConfig *cfg, const char *model) {
    if (!cfg->model_favorites_file || !file_exists(cfg->model_favorites_file)) return false;
    FILE *f = fopen(cfg->model_favorites_file, "r");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_in_place(line);
        if (strcmp(s, model) == 0) { found = true; break; }
    }
    fclose(f);
    return found;
}

static int list_favorites(const TCConfig *cfg) {
    if (!cfg->model_favorites_file || !file_exists(cfg->model_favorites_file)) {
        fprintf(stderr, "No favorites file yet: %s\n", cfg->model_favorites_file ? cfg->model_favorites_file : "(unset)");
        return 0;
    }
    size_t len = 0;
    char *text = read_file(cfg->model_favorites_file, &len);
    printf("%s", text);
    free(text);
    return 0;
}

static int add_favorite(const TCConfig *cfg, const char *model) {
    if (!model || !*model) return 1;
    if (favorite_contains(cfg, model)) {
        fprintf(stderr, "favorite already present: %s\n", model);
        return 0;
    }
    char *dircopy = xstrdup(cfg->model_favorites_file);
    char *slash = strrchr(dircopy, '/');
    if (slash) { *slash = '\0'; mkdir_p(dircopy); }
    free(dircopy);
    TCBuffer b; buffer_init(&b);
    buffer_appendf(&b, "%s\n", model);
    append_file_text(cfg->model_favorites_file, b.data);
    free(b.data);
    fprintf(stderr, "added favorite: %s\n", model);
    return 0;
}

static int remove_favorite(const TCConfig *cfg, const char *model) {
    if (!model || !*model || !file_exists(cfg->model_favorites_file)) return 0;
    FILE *f = fopen(cfg->model_favorites_file, "r");
    if (!f) return 1;
    TCBuffer out; buffer_init(&out);
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", line);
        char *s = trim_in_place(copy);
        if (strcmp(s, model) != 0) buffer_append(&out, line);
    }
    fclose(f);
    write_file_mode(cfg->model_favorites_file, out.data, 0600);
    free(out.data);
    fprintf(stderr, "removed favorite: %s\n", model);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* API key commands                                                          */
/* ------------------------------------------------------------------------- */

static char *prompt_secret(const char *prompt) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    struct termios oldt, newt;
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldt) == 0;
    if (tty) {
        newt = oldt;
        newt.c_lflag &= ~(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
    char buf[8192];
    if (!fgets(buf, sizeof(buf), stdin)) buf[0] = '\0';
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fprintf(stderr, "\n");
    }
    return xstrdup(trim_in_place(buf));
}

static int cmd_set_api_key(TCConfig *cfg, bool use_gpg) {
    char *key = prompt_secret("Paste OpenRouter API key: ");
    if (!key || !*key) {
        free(key);
        fprintf(stderr, "No key entered.\n");
        return 1;
    }
    if (use_gpg) {
        if (!command_exists("gpg")) {
            fprintf(stderr, "gpg not found. Install gpg or save plaintext.\n");
            free(key);
            return 1;
        }
        if (!gpg_encrypt_text_to_file(key, cfg->api_key_gpg_file)) {
            fprintf(stderr, "GPG encryption failed.\n");
            free(key);
            return 1;
        }
        config_write_key_value(cfg, "API_KEY_GPG_FILE", cfg->api_key_gpg_file);
        config_remove_key(cfg, "OPENROUTER_API_KEY");
        config_remove_key(cfg, "TTYCHATTER_API_KEY");
        config_remove_key(cfg, "API_KEY");
        fprintf(stderr, "Encrypted API key saved to %s\n", cfg->api_key_gpg_file);
    } else {
        fprintf(stderr, "WARNING: saving API key in plaintext config.\n");
        config_write_key_value(cfg, "OPENROUTER_API_KEY", key);
        fprintf(stderr, "Plaintext API key saved to %s\n", cfg->config_file);
    }
    free(key);
    return 0;
}

static int cmd_forget_api_key(TCConfig *cfg) {
    config_remove_key(cfg, "OPENROUTER_API_KEY");
    config_remove_key(cfg, "TTYCHATTER_API_KEY");
    config_remove_key(cfg, "API_KEY");
    if (cfg->api_key_gpg_file && file_exists(cfg->api_key_gpg_file)) unlink(cfg->api_key_gpg_file);
    fprintf(stderr, "Removed stored API key entries.\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CLI display helpers                                                       */
/* ------------------------------------------------------------------------- */

static void print_help(void) {
    printf("%s %s\n", TC_PROGRAM, TC_VERSION);
    printf("\n");
    printf("Usage:\n");
    printf("  ttychatter\n");
    printf("  ttychatter [options] input.txt [output.log]\n");
    printf("  ttychatter --prompt TEXT [--session SESSION|--output output.log]\n");
    printf("  ttychatter --input input.txt [--session SESSION|--output output.log]\n");
    printf("  ttychatter -i|--interactive|--chat [SESSION|./session.log]\n");
    printf("  ttychatter --resume SESSION\n");
    printf("  ttychatter --set-api-key [--gpg]\n");
    printf("  ttychatter --forget-api-key\n");
    printf("  ttychatter --models [filters]\n");
    printf("  ttychatter --update-models\n");
    printf("  ttychatter --routers\n");
    printf("  ttychatter --select-model\n");
    printf("  ttychatter --test-model MODEL [--save]\n");
    printf("  ttychatter --favorites | --favorite-model MODEL | --unfavorite-model MODEL\n");
    printf("  ttychatter --list | --rename-session SESSION TITLE\n");
    printf("  ttychatter --turns SESSION | --branch SESSION TURN | --edit-turn SESSION TURN\n");
    printf("  ttychatter --export SESSION [--format markdown|text|json] [--output FILE]\n");
    printf("  ttychatter --show-memory SESSION | --clear-memory SESSION | --edit-memory SESSION\n");
    printf("  ttychatter --editor-prompt [output.log]\n");
    printf("  ttychatter --search TEXT [--all-sessions]\n");
    printf("  ttychatter --config | --set KEY=VALUE | --unset KEY\n");
    printf("  ttychatter --doctor | --credits | --version | --help\n");
    printf("\nGeneral:\n");
    printf("  -h, --help                   show this help\n");
    printf("  -v, --version                show version\n");
    printf("      --demo                   use local demo models/responses; no network/API key\n");
    printf("      --doctor                 run local diagnostics\n");
    printf("      --credits                show project notice\n");
    printf("      --config                 print config summary\n");
    printf("\nChat and sessions:\n");
    printf("  -m, --model MODEL            model/router to use (default from config)\n");
    printf("  -a, --attach FILE            attach file; repeatable\n");
    printf("      --stream / --no-stream   print assistant output as it arrives\n");
    printf("      --status / --no-status   print local progress/status updates to stderr\n");
    printf("      --progress / --no-progress alias for --status / --no-status\n");
    printf("      --voice-input FILE       transcribe FILE with VOICE_INPUT_CMD and send\n");
    printf("      --transcribe FILE        alias for --voice-input FILE\n");
    printf("      --speak / --no-speak     run VOICE_OUTPUT_CMD/TTS_CMD for replies\n");
    printf("  -l, --loopback               also print AI output to stdout\n");
    printf("      --loopback-file FILE     mirror clean AI output to FILE\n");
    printf("  -o, --output FILE            explicit output/session log for batch mode\n");
    printf("  -s, --session SESSION        named session under SESSION_DIR\n");
    printf("  -p, --prompt TEXT            send TEXT instead of reading an input file\n");
    printf("      --input FILE             input file; use - for stdin\n");
    printf("  -n, --new                    start a new interactive timestamped session\n");
    printf("  -i, --interactive            line-oriented chat loop with colon commands\n");
    printf("      --chat                   alias for --interactive\n");
    printf("      --resume SESSION         resume named or explicit interactive session\n");
    printf("      --list                   list session logs with friendly titles\n");
    printf("      --rename-session SESSION TITLE set session title metadata\n");
    printf("      --turns SESSION          list message numbers in a session\n");
    printf("      --branch SESSION TURN    create a new branch after message TURN\n");
    printf("      --edit-turn SESSION TURN edit a user turn, branch, and resend\n");
    printf("      --export SESSION         export a session as markdown/text/json\n");
    printf("      --format FORMAT          export format: markdown, text, or json\n");
    printf("      --show-memory SESSION    print reconstructed context/memory\n");
    printf("      --clear-memory SESSION   append a memory-clear marker\n");
    printf("      --edit-memory SESSION    edit reconstructed context in editor\n");
    printf("      --editor-prompt [LOG]    compose one message in editor and send it\n");
    printf("      --search TEXT            filter --models, otherwise search session logs\n");
    printf("      --search-sessions TEXT   search all session logs\n");
    printf("      --all-sessions           with --search, search all sessions\n");
    printf("      --yes                    confirm live send when CONFIRM_LIVE_SEND=1\n");
    printf("\nModels:\n");
    printf("      --update-models          refresh model cache from OpenRouter\n");
    printf("      --models                 list cached models\n");
    printf("      --routers                shorthand for --models --model-type routers\n");
    printf("      --select-model           numbered cached model selector\n");
    printf("      --autoscan-model         removed compatibility flag; use --models/--select-model\n");
    printf("      --test-model MODEL       send a test prompt to MODEL\n");
    printf("      --save                   save successful --test-model result as MODEL\n");
    printf("      --model-type TYPE        all, routers, fixed, free, auto\n");
    printf("      --sort KEY               name, context, tokens, type, favorites\n");
    printf("      --min-input-tokens N     filter by input/context limit\n");
    printf("      --min-output-tokens N    filter by output-token limit\n");
    printf("      --hide-preview           hide preview/experimental-looking model IDs\n");
    printf("      --show-preview           allow preview/experimental-looking model IDs\n");
    printf("      --require-tokens         require known context/output token limits\n");
    printf("      --allow-missing-tokens   allow rows with unknown token limits\n");
    printf("      --all                    disable model-list filters\n");
    printf("      --favorites-only         show only favorite models in --models\n");
    printf("      --favorites              list favorite models\n");
    printf("      --favorite-model MODEL   add favorite model\n");
    printf("      --unfavorite-model MODEL remove favorite model\n");
    printf("      --unbookmark-model MODEL alias for --unfavorite-model\n");
    printf("\nAPI key and config writes:\n");
    printf("      --set-api-key            save API key\n");
    printf("      --gpg                    encrypt API key with gpg when used with --set-api-key\n");
    printf("      --forget-api-key         remove stored plaintext/encrypted API key\n");
    printf("      --set KEY=VALUE          write a config key\n");
    printf("      --unset KEY              remove a config key\n");
    printf("      --send-input VALUE       write SEND_INPUT config compatibility key\n");
    printf("      --theme VALUE            write THEME config compatibility key\n");
    printf("      --code-attachment-min-lines N write CODE_ATTACHMENT_MIN_LINES\n");
    printf("\nSession addressing:\n");
    printf("  With no arguments on a terminal, ttychatter starts interactive mode.\n");
    printf("  input.txt output.log uses output.log exactly.\n");
    printf("  input.txt alone auto-saves to a timestamped log in SESSION_DIR.\n");
    printf("  --prompt TEXT and stdin input also auto-save unless --output/--session is used.\n");
    printf("  Bare SESSION names resolve to SESSION_DIR/SESSION.log.\n");
    printf("  SESSION values containing '/' are explicit paths. Use ./name.log for cwd.\n");
    printf("  SESSION_AUTO_TITLE=1 enables optional AI-generated titles for --list.\n");
    printf("  STREAM=1 enables streaming by default; STATUS_UPDATES=1 enables local progress notes.\n");
    printf("  VOICE_INPUT_CMD and VOICE_OUTPUT_CMD/TTS_CMD provide external audio hooks.\n");
}

static void print_version(void) {
    printf("%s %s\n", TC_PROGRAM, TC_VERSION);
}

static int doctor(const TCConfig *cfg) {
    printf("ttychatter doctor\n");
    printf("config:      %s\n", cfg->config_file);
    printf("api key:     %s\n", cfg->api_key ? "loaded" : "missing");
    printf("gpg:         %s\n", command_exists("gpg") ? "found" : "not found");
    printf("gpg file:    %s%s\n", cfg->api_key_gpg_file, file_exists(cfg->api_key_gpg_file) ? " (present)" : "");
    printf("model:       %s\n", cfg->model);
    printf("cache:       %s%s\n", cfg->model_cache_file, file_exists(cfg->model_cache_file) ? " (present)" : "");
    printf("sessions:    %s\n", cfg->session_dir);
    printf("title-gen:   %s (%s)\n", cfg->session_auto_title ? "enabled" : "disabled", cfg->session_title_model);
    printf("attachments: %s\n", cfg->attachment_dir);
    printf("streaming:   %s\n", cfg->stream ? "enabled" : "disabled");
    printf("status:      %s\n", cfg->status_updates ? "enabled" : "disabled");
    printf("voice in:    %s\n", (cfg->voice_input_cmd && *cfg->voice_input_cmd) ? cfg->voice_input_cmd : "not configured");
    printf("voice out:   %s%s\n", cfg->voice_output ? "enabled " : "disabled ", (cfg->voice_output_cmd && *cfg->voice_output_cmd) ? cfg->voice_output_cmd : "(no command)");
    printf("libcurl:     %s\n", curl_version());
    printf("theme:       %s\n", cfg->theme);
    printf("confirm:     %s\n", cfg->confirm_live_send ? "enabled" : "disabled");
    printf("startup:     %s\n", cfg->startup_notice ? "enabled" : "disabled");
    printf("loopback:    %s\n", cfg->loopback_default ? "enabled" : "disabled");
    printf("sort:        %s\n", cfg->model_sort_order);
    return cfg->api_key ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Main argument parser and command dispatch                                 */
/* ------------------------------------------------------------------------- */

static void args_init(TCArgs *a) {
    memset(a, 0, sizeof(*a));
    a->model_type = xstrdup("all");
    a->model_sort = NULL;
}

static void args_add_attachment(TCArgs *a, const char *path) {
    a->attachments = realloc(a->attachments, sizeof(char *) * (a->attachment_count + 1));
    if (!a->attachments) die("out of memory");
    a->attachments[a->attachment_count++] = xstrdup(path);
}


static bool parse_session_turn_value(const char *arg, char **session_out, long *turn_out) {
    if (!arg || !*arg) return false;
    const char *colon = strrchr(arg, ':');
    if (!colon || !colon[1]) return false;
    for (const char *p = colon + 1; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    size_t slen = (size_t)(colon - arg);
    if (slen == 0) return false;
    char *session = xcalloc(slen + 1, 1);
    memcpy(session, arg, slen);
    *session_out = session;
    *turn_out = atol(colon + 1);
    return true;
}

static long parse_turn_operand(const char *s) {
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    return atol(s);
}

static int parse_args(int argc, char **argv, TCArgs *args) {
    static struct option longopts[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"model", required_argument, 0, 'm'},
        {"attach", required_argument, 0, 'a'},
        {"loopback", no_argument, 0, 'l'},
        {"output", required_argument, 0, 'o'},
        {"session", required_argument, 0, 's'},
        {"prompt", required_argument, 0, 'p'},
        {"new", no_argument, 0, 'n'},
        {"input", required_argument, 0, 1044},
        {"set-api-key", no_argument, 0, 1000},
        {"gpg", no_argument, 0, 1001},
        {"forget-api-key", no_argument, 0, 1002},
        {"models", no_argument, 0, 1003},
        {"routers", no_argument, 0, 1039},
        {"update-models", no_argument, 0, 1004},
        {"search", required_argument, 0, 1005},
        {"model-type", required_argument, 0, 1006},
        {"test-model", required_argument, 0, 1007},
        {"doctor", no_argument, 0, 1008},
        {"demo", no_argument, 0, 1009},
        {"interactive", no_argument, 0, 'i'},
        {"chat", no_argument, 0, 'i'},
        {"config", no_argument, 0, 1010},
        {"set", required_argument, 0, 1011},
        {"unset", required_argument, 0, 1012},
        {"favorites", no_argument, 0, 1013},
        {"favorite-model", required_argument, 0, 1014},
        {"unfavorite-model", required_argument, 0, 1015},
        {"unbookmark-model", required_argument, 0, 1015},
        {"loopback-file", required_argument, 0, 1016},
        {"sort", required_argument, 0, 1017},
        {"favorites-only", no_argument, 0, 1018},
        {"save", no_argument, 0, 1040},
        {"yes", no_argument, 0, 1019},
        {"list", no_argument, 0, 1020},
        {"rename-session", required_argument, 0, 1021},
        {"show-memory", required_argument, 0, 1022},
        {"clear-memory", required_argument, 0, 1023},
        {"edit-memory", required_argument, 0, 1024},
        {"editor-prompt", optional_argument, 0, 1025},
        {"credits", no_argument, 0, 1026},
        {"all", no_argument, 0, 1027},
        {"search-sessions", required_argument, 0, 1038},
        {"all-sessions", no_argument, 0, 1028},
        {"select-model", no_argument, 0, 1029},
        {"min-input-tokens", required_argument, 0, 1030},
        {"min-output-tokens", required_argument, 0, 1031},
        {"hide-preview", no_argument, 0, 1032},
        {"show-preview", no_argument, 0, 1033},
        {"allow-missing-tokens", no_argument, 0, 1041},
        {"require-tokens", no_argument, 0, 1034},
        {"send-input", required_argument, 0, 1035},
        {"resume", required_argument, 0, 1042},
        {"autoscan-model", no_argument, 0, 1043},
        {"theme", required_argument, 0, 1036},
        {"code-attachment-min-lines", required_argument, 0, 1037},
        {"stream", no_argument, 0, 1045},
        {"no-stream", no_argument, 0, 1046},
        {"status", no_argument, 0, 1047},
        {"progress", no_argument, 0, 1047},
        {"no-status", no_argument, 0, 1048},
        {"no-progress", no_argument, 0, 1048},
        {"voice-input", required_argument, 0, 1049},
        {"transcribe", required_argument, 0, 1049},
        {"speak", no_argument, 0, 1050},
        {"no-speak", no_argument, 0, 1051},
        {"turns", required_argument, 0, 1052},
        {"branch", required_argument, 0, 1053},
        {"edit-turn", required_argument, 0, 1054},
        {"export", required_argument, 0, 1055},
        {"format", required_argument, 0, 1056},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "hvim:a:lo:s:p:n", longopts, NULL)) != -1) {
        switch (c) {
            case 'h': args->show_help = true; break;
            case 'v': args->show_version = true; break;
            case 'i': args->interactive = true; break;
            case 'm': args->model_override = xstrdup(optarg); break;
            case 'a': args_add_attachment(args, optarg); break;
            case 'l': args->loopback = true; break;
            case 'o': args->output_path = xstrdup(optarg); break;
            case 's': args->session_arg = xstrdup(optarg); break;
            case 'p': args->prompt_text = xstrdup(optarg); break;
            case 'n': args->new_session_cmd = true; args->interactive = true; break;
            case 1044: args->input_path = xstrdup(optarg); break;
            case 1000: args->set_api_key = true; break;
            case 1001: args->set_api_key_gpg = true; break;
            case 1002: args->forget_api_key = true; break;
            case 1003: args->list_models = true; break;
            case 1039: args->list_models = true; free(args->model_type); args->model_type = xstrdup("routers"); break;
            case 1004: args->update_models = true; break;
            case 1005: args->search = xstrdup(optarg); break;
            case 1006: free(args->model_type); args->model_type = xstrdup(optarg); break;
            case 1007: args->test_model = true; args->test_model_id = xstrdup(optarg); break;
            case 1008: args->doctor = true; break;
            case 1009: args->demo = true; break;
            case 1010: args->show_config = true; break;
            case 1011: {
                args->config_set_cmd = true;
                char *eq = strchr(optarg, '=');
                if (eq) {
                    size_t klen = (size_t)(eq - optarg);
                    args->config_key = xcalloc(klen + 1, 1);
                    memcpy(args->config_key, optarg, klen);
                    args->config_value = xstrdup(eq + 1);
                } else {
                    args->config_key = xstrdup(optarg);
                }
                break;
            }
            case 1012: args->config_unset_cmd = true; args->config_key = xstrdup(optarg); break;
            case 1013: args->favorites = true; break;
            case 1014: args->favorite_model = true; args->test_model_id = xstrdup(optarg); break;
            case 1015: args->unfavorite_model = true; args->test_model_id = xstrdup(optarg); break;
            case 1016: args->loopback_file = xstrdup(optarg); break;
            case 1017: args->model_sort = xstrdup(optarg); break;
            case 1018: args->favorites_only = true; break;
            case 1040: args->save = true; break;
            case 1019: args->yes = true; break;
            case 1020: args->list_sessions_cmd = true; break;
            case 1021:
                args->rename_session_cmd = true;
                args->rename_old = xstrdup(optarg);
                if (optind < argc && argv[optind][0] != '-') {
                    TCBuffer title;
                    buffer_init(&title);
                    while (optind < argc && argv[optind][0] != '-') {
                        if (title.len > 0) buffer_append(&title, " ");
                        buffer_append(&title, argv[optind++]);
                    }
                    args->rename_new = buffer_take(&title);
                }
                break;
            case 1022: args->show_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1023: args->clear_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1024: args->edit_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1025:
                args->editor_prompt_cmd = true;
                if (optarg) args->output_path = xstrdup(optarg);
                else if (optind < argc && argv[optind][0] != '-') args->output_path = xstrdup(argv[optind++]);
                break;
            case 1026: args->credits_cmd = true; break;
            case 1027: args->show_all_models = true; break;
            case 1028: args->search_sessions_cmd = true; break;
            case 1029: args->select_model_cmd = true; break;
            case 1030: args->min_input_tokens = atol(optarg); break;
            case 1031: args->min_output_tokens = atol(optarg); break;
            case 1032: args->hide_preview = true; break;
            case 1033: args->show_preview = true; break;
            case 1041: args->allow_missing_tokens = true; break;
            case 1034: args->require_tokens = true; break;
            case 1035: args->config_set_cmd = true; args->config_key = xstrdup("SEND_INPUT"); args->config_value = xstrdup(optarg); break;
            case 1042: args->interactive = true; args->resume_session = xstrdup(optarg); break;
            case 1043: fprintf(stderr, "--autoscan-model has been removed. Use --models, --test-model MODEL, or --select-model.\n"); return 1;
            case 1036: args->config_set_cmd = true; args->config_key = xstrdup("THEME"); args->config_value = xstrdup(optarg); break;
            case 1037: args->config_set_cmd = true; args->config_key = xstrdup("CODE_ATTACHMENT_MIN_LINES"); args->config_value = xstrdup(optarg); break;
            case 1038: args->search = xstrdup(optarg); args->search_sessions_cmd = true; break;
            case 1045: args->stream_set = true; args->stream = true; break;
            case 1046: args->stream_set = true; args->stream = false; break;
            case 1047: args->status_set = true; args->status_updates = true; break;
            case 1048: args->status_set = true; args->status_updates = false; break;
            case 1049: args->voice_input_path = xstrdup(optarg); break;
            case 1050: args->speak_set = true; args->speak = true; break;
            case 1051: args->speak_set = true; args->speak = false; break;
            case 1052: args->turns_cmd = true; args->turns_session = xstrdup(optarg); break;
            case 1053:
                args->branch_cmd = true;
                if (!parse_session_turn_value(optarg, &args->branch_session, &args->branch_turn)) {
                    args->branch_session = xstrdup(optarg);
                    if (optind < argc && argv[optind][0] != '-') args->branch_turn = parse_turn_operand(argv[optind++]);
                    else args->branch_turn = -1;
                }
                break;
            case 1054:
                args->edit_turn_cmd = true;
                if (!parse_session_turn_value(optarg, &args->edit_turn_session, &args->edit_turn)) {
                    args->edit_turn_session = xstrdup(optarg);
                    if (optind < argc && argv[optind][0] != '-') args->edit_turn = parse_turn_operand(argv[optind++]);
                    else args->edit_turn = -1;
                }
                break;
            case 1055: args->export_cmd = true; args->export_session = xstrdup(optarg); break;
            case 1056: args->export_format = xstrdup(optarg); break;
            default: return 1;
        }
    }
    if (optind < argc) {
        if (!args->input_path && !args->prompt_text) args->input_path = xstrdup(argv[optind++]);
        else if (!args->output_path) args->output_path = xstrdup(argv[optind++]);
    }
    if (optind < argc) {
        if (!args->output_path) args->output_path = xstrdup(argv[optind++]);
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected extra argument: %s\n", argv[optind]);
        return 1;
    }
    return 0;
}

static bool confirm_live_send_if_needed(const TCConfig *cfg, const TCArgs *args) {
    /*
     * CONFIRM_LIVE_SEND is the command-line counterpart to the graphical
     * client's "first real send" protection.  A file-in/file-out Unix command
     * is already fairly explicit, so the default is off.  Still, some users are
     * cautious with paid providers and want a last human checkpoint before a
     * real OpenRouter request leaves the machine.  Demo mode, model listing,
     * and other local commands never reach this function; it guards only the
     * final chat-completion request.
     */
    if (args->demo || args->yes || !cfg->confirm_live_send) return true;
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "CONFIRM_LIVE_SEND is enabled but stdin is not a terminal. Use --yes to confirm.\n");
        return false;
    }
    fprintf(stderr, "About to send a live OpenRouter request using model %s. Continue? [y/N] ", args->model_override ? args->model_override : cfg->model);
    char line[32];
    if (!fgets(line, sizeof(line), stdin)) return false;
    char *s = trim_in_place(line);
    return strcasecmp(s, "y") == 0 || strcasecmp(s, "yes") == 0;
}

static void maybe_write_prechat_and_startup_notice(const TCConfig *cfg, const TCArgs *args, const char *output_path) {
    /*
     * Put the requested pre-chat line at byte zero of each new session log.
     * It stays outside USER/AI/system timestamp entries, so context loading
     * ignores it and does not replay the line to the provider as conversation
     * memory.  STARTUP_NOTICE controls only the extra metadata marker below.
     */
    (void)args;
    if (file_exists(output_path) && file_size(output_path) > 0) return;
    TCBuffer b;
    buffer_init(&b);
    buffer_append(&b, TC_PRECHAT_LINE);
    buffer_append(&b, "\n");
    if (cfg->startup_notice) {
        char *now = now_string();
        buffer_appendf(&b, "%s\n", TC_NOTICE_BEGIN);
        buffer_appendf(&b, "time: %s\n", now);
        buffer_append(&b, "kind: startup-prechat\n");
        buffer_append(&b, "This is a ttychatter session file.  Future user inputs and AI responses will be appended below.\n");
        buffer_appendf(&b, "%s\n\n", TC_NOTICE_END);
        free(now);
    }
    append_file_text(output_path, b.data);
    free(b.data);
}

static int test_model_command(const TCConfig *cfg, const char *model, bool demo) {
    TCMessageList ctx = {0};
    TCArgs empty;
    args_init(&empty);
    empty.demo = demo;
    empty.stream_set = true;
    empty.stream = false;
    empty.status_set = true;
    empty.status_updates = false;
    const char *prompt = cfg->model_test_prompt;
    char *reply = openrouter_chat(cfg, model, prompt, &ctx, &empty);
    printf("Model: %s\n", model);
    printf("Response:\n%s\n", reply);
    free(reply);
    free(empty.model_type);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Interactive colon-command chat loop                                       */
/* ------------------------------------------------------------------------- */

/*
 * The earliest C command-line design was intentionally file-in/file-out:
 *     ttychatter input.txt output.log
 * That remains the most traditional Unix interface and is still the default.
 *
 * The Bash clients, however, also have a running chat loop with colon commands
 * such as :help, :models, :attach, and :quit.  Those commands are not sent to
 * the model; they are local client-control commands.  To keep the C edition on
 * feature parity with the Bash editions without turning it into ncurses, the C
 * version provides an explicit --interactive mode.
 *
 * In --interactive mode, ttychatter stays line-oriented.  This is not a curses
 * screen, not a readline clone, and not an editor.  It is a simple REPL:
 *
 *     ttychatter --interactive session.log
 *     ttychatter> hello
 *     ttychatter> :models
 *     ttychatter> :attach notes.txt
 *     ttychatter> :quit
 *
 * This preserves Unix composability while making the C client usable as a
 * genuine chat program.  The same output log remains the session file, and the
 * same context reconstruction code is used.  Future maintainers should resist
 * the temptation to make this function an entire terminal UI.  That job belongs
 * to the ncurses implementation.  This REPL exists only to give the plain C CLI
 * feature-equivalent local commands.
 */

static const char *pick_external_editor(void);
static char *compose_with_external_editor(void);
static char *session_path_from_arg(const TCConfig *cfg, const char *arg);

static char *default_session_path(const TCConfig *cfg) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stem[128];
    strftime(stem, sizeof(stem), "session-%Y-%m-%d-%H-%M-%S", &tmv);
    for (int i = 0; i < 1000; i++) {
        char name[160];
        if (i == 0) snprintf(name, sizeof(name), "%s.log", stem);
        else snprintf(name, sizeof(name), "%s-%03d.log", stem, i);
        char *path = path_join2(cfg->session_dir, name);
        if (!file_exists(path)) return path;
        free(path);
    }
    char name[192];
    snprintf(name, sizeof(name), "%s-%ld.log", stem, (long)getpid());
    return path_join2(cfg->session_dir, name);
}


static char *message_preview(const char *text, size_t max_chars) {
    TCBuffer b;
    buffer_init(&b);
    bool prev_space = false;
    for (const char *p = text ? text : ""; *p && b.len < max_chars; p++) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t' || isspace((unsigned char)c)) {
            if (!prev_space && b.len > 0) buffer_append(&b, " ");
            prev_space = true;
        } else {
            buffer_append_n(&b, &c, 1);
            prev_space = false;
        }
    }
    char *out = buffer_take(&b);
    char *t = trim_in_place(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
    return out;
}

static TCMessageList load_messages_from_session_file(const char *path) {
    TCMessageList out = {0};
    if (!path || !file_exists(path)) return out;
    size_t len = 0;
    char *text = read_file(path, &len);
    (void)len;
    TCLogEntryList entries = parse_shared_log_entries(text);
    if (entries.count > 0) {
        for (size_t i = 0; i < entries.count; i++) {
            const char *role = role_from_shared_speaker(entries.items[i].speaker);
            if (role) msglist_add(&out, role, entries.items[i].text.data);
        }
        logentries_free(&entries);
        free(text);
        return out;
    }
    logentries_free(&entries);
    out = load_context_from_legacy_c_log(text, 999999);
    free(text);
    return out;
}

static TCMessageList msglist_prefix_copy(const TCMessageList *src, size_t count) {
    TCMessageList out = {0};
    if (!src) return out;
    if (count > src->count) count = src->count;
    for (size_t i = 0; i < count; i++) msglist_add(&out, src->items[i].role, src->items[i].content);
    return out;
}

static int print_turns_for_file(const char *path) {
    TCMessageList msgs = load_messages_from_session_file(path);
    if (msgs.count == 0) {
        printf("[no user/assistant messages]\n");
        msglist_free(&msgs);
        return 0;
    }
    printf("%-5s %-5s %-10s %s\n", "MSG", "CYCLE", "ROLE", "PREVIEW");
    printf("%-5s %-5s %-10s %s\n", "---", "-----", "----", "-------");
    long cycle = 0;
    for (size_t i = 0; i < msgs.count; i++) {
        if (strcasecmp(msgs.items[i].role, "user") == 0) cycle++;
        char *prev = message_preview(msgs.items[i].content, 88);
        printf("%-5zu %-5ld %-10s %s\n", i + 1, cycle > 0 ? cycle : 1, msgs.items[i].role, prev);
        free(prev);
    }
    msglist_free(&msgs);
    return 0;
}

static int turns_command(const TCConfig *cfg, const char *session_name) {
    char *path = session_path_from_arg(cfg, session_name);
    if (!file_exists(path)) {
        fprintf(stderr, "session not found: %s\n", path);
        free(path);
        return 1;
    }
    int rc = print_turns_for_file(path);
    free(path);
    return rc;
}

static void append_branch_metadata(const char *branch_path, const char *source_path, long turn, const char *reason) {
    TCBuffer b;
    buffer_init(&b);
    char *ts = now_hms();
    buffer_appendf(&b, "[%s] system: branch-from: %s\n", ts, source_path ? source_path : "");
    buffer_appendf(&b, "[%s] system: branch-turn: %ld\n", ts, turn);
    buffer_appendf(&b, "[%s] system: branch-reason: %s\n", ts, reason ? reason : "manual");
    append_file_text(branch_path, b.data);
    free(ts);
    free(b.data);
}

static void append_message_as_log(const char *path, const TCMessage *msg) {
    char *ts = now_hms();
    const char *speaker = (msg && strcasecmp(msg->role, "assistant") == 0) ? "AI" : "User";
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "[%s] %s: %s\n", ts, speaker, msg && msg->content ? msg->content : "");
    append_file_text(path, b.data);
    free(ts);
    free(b.data);
}

static char *create_branch_file_from_prefix(const TCConfig *cfg, const TCArgs *args, const char *source_path,
                                            const TCMessageList *msgs, size_t keep_count, long turn, const char *reason) {
    char *branch_path = default_session_path(cfg);
    maybe_write_prechat_and_startup_notice(cfg, args, branch_path);
    append_branch_metadata(branch_path, source_path, turn, reason);
    if (msgs && keep_count > msgs->count) keep_count = msgs->count;
    for (size_t i = 0; msgs && i < keep_count; i++) append_message_as_log(branch_path, &msgs->items[i]);
    append_file_text(branch_path, "\n");
    return branch_path;
}

static int branch_session_command(const TCConfig *cfg, const TCArgs *args, const char *session_name, long turn) {
    if (!session_name || !*session_name || turn < 0) {
        fprintf(stderr, "usage: --branch SESSION TURN\n");
        return 2;
    }
    char *source_path = session_path_from_arg(cfg, session_name);
    if (!file_exists(source_path)) {
        fprintf(stderr, "session not found: %s\n", source_path);
        free(source_path);
        return 1;
    }
    TCMessageList msgs = load_messages_from_session_file(source_path);
    if ((size_t)turn > msgs.count) {
        fprintf(stderr, "turn out of range: %ld (session has %zu messages)\n", turn, msgs.count);
        msglist_free(&msgs);
        free(source_path);
        return 2;
    }
    char *branch_path = create_branch_file_from_prefix(cfg, args, source_path, &msgs, (size_t)turn, turn, "manual-branch");
    printf("%s\n", branch_path);
    msglist_free(&msgs);
    free(branch_path);
    free(source_path);
    return 0;
}

static char *edit_text_with_external_editor(const char *initial_text) {
    const char *editor = pick_external_editor();
    if (!editor) {
        fprintf(stderr, "no external editor found; set VISUAL or EDITOR\n");
        return NULL;
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "/tmp/ttychatter-edit-turn.XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmpl); return NULL; }
    if (initial_text) fwrite(initial_text, 1, strlen(initial_text), f);
    fclose(f);
    char *qpath = shell_quote(tmpl);
    TCBuffer cmd;
    buffer_init(&cmd);
    buffer_appendf(&cmd, "%s %s", editor, qpath);
    free(qpath);
    int rc = system(cmd.data);
    free(cmd.data);
    if (rc != 0) { unlink(tmpl); return NULL; }
    size_t len = 0;
    char *edited = read_file(tmpl, &len);
    (void)len;
    unlink(tmpl);
    char *t = trim_in_place(edited);
    if (!*t) {
        free(edited);
        fprintf(stderr, "edited turn is empty\n");
        return NULL;
    }
    if (t != edited) memmove(edited, t, strlen(t) + 1);
    return edited;
}

static int edit_turn_command(const TCConfig *cfg, TCArgs *args, const char *session_name, long turn) {
    if (!session_name || !*session_name || turn <= 0) {
        fprintf(stderr, "usage: --edit-turn SESSION TURN\n");
        return 2;
    }
    if (!args->demo && !cfg->api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");
    char *source_path = session_path_from_arg(cfg, session_name);
    if (!file_exists(source_path)) {
        fprintf(stderr, "session not found: %s\n", source_path);
        free(source_path);
        return 1;
    }
    TCMessageList msgs = load_messages_from_session_file(source_path);
    if ((size_t)turn > msgs.count) {
        fprintf(stderr, "turn out of range: %ld (session has %zu messages)\n", turn, msgs.count);
        msglist_free(&msgs);
        free(source_path);
        return 2;
    }
    TCMessage *target = &msgs.items[(size_t)turn - 1];
    if (strcasecmp(target->role, "user") != 0) {
        fprintf(stderr, "--edit-turn expects a user message number; run --turns SESSION first\n");
        msglist_free(&msgs);
        free(source_path);
        return 2;
    }
    char *edited = edit_text_with_external_editor(target->content);
    if (!edited) { msglist_free(&msgs); free(source_path); return 1; }
    TCMessageList prefix = msglist_prefix_copy(&msgs, (size_t)turn - 1);
    char *branch_path = create_branch_file_from_prefix(cfg, args, source_path, &msgs, (size_t)turn - 1, turn, "edited-user-turn");
    status_update(cfg, args, "created branch %s", branch_path);
    const char *model = args->model_override ? args->model_override : cfg->model;
    char *raw = openrouter_chat(cfg, model, edited, &prefix, args);
    char *base = basename_no_ext(branch_path);
    char *cleaned = extract_code_blocks(cfg, base, raw);
    append_turn(branch_path, model, "edited-turn", args, edited, cleaned, &prefix, cfg->context_turns);
    maybe_generate_session_title(cfg, args, branch_path, edited, cleaned);
    if (!effective_stream(cfg, args)) printf("%s\n", cleaned);
    if (effective_speak(cfg, args)) run_voice_output_command(cfg, cleaned);
    fprintf(stderr, "branched edited turn to %s\n", branch_path);
    free(edited); free(raw); free(base); free(cleaned); free(branch_path);
    msglist_free(&prefix); msglist_free(&msgs); free(source_path);
    return 0;
}

static int export_session_command(const TCConfig *cfg, const char *session_name, const char *format, const char *output_path) {
    if (!session_name || !*session_name) {
        fprintf(stderr, "usage: --export SESSION [--format text|markdown|json] [--output FILE]\n");
        return 2;
    }
    char *path = session_path_from_arg(cfg, session_name);
    if (!file_exists(path)) {
        fprintf(stderr, "session not found: %s\n", path);
        free(path);
        return 1;
    }
    const char *fmt = format && *format ? format : "markdown";
    TCMessageList msgs = load_messages_from_session_file(path);
    char *title = session_title_from_file(path);
    if (!title) title = basename_no_ext(path);
    TCBuffer out;
    buffer_init(&out);
    if (strcasecmp(fmt, "json") == 0) {
        json_object *root = json_object_new_object();
        json_object_object_add(root, "session", json_object_new_string(path));
        json_object_object_add(root, "title", json_object_new_string(title));
        json_object *arr = json_object_new_array();
        for (size_t i = 0; i < msgs.count; i++) {
            json_object *m = json_object_new_object();
            json_object_object_add(m, "role", json_object_new_string(msgs.items[i].role));
            json_object_object_add(m, "content", json_object_new_string(msgs.items[i].content));
            json_object_array_add(arr, m);
        }
        json_object_object_add(root, "messages", arr);
        buffer_append(&out, json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        buffer_append(&out, "\n");
        json_object_put(root);
    } else if (strcasecmp(fmt, "text") == 0) {
        buffer_appendf(&out, "Title: %s\nSession: %s\n\n", title, path);
        for (size_t i = 0; i < msgs.count; i++) {
            buffer_appendf(&out, "%s:\n%s\n\n", msgs.items[i].role, msgs.items[i].content);
        }
    } else {
        buffer_appendf(&out, "# %s\n\n", title);
        buffer_appendf(&out, "Source: `%s`\n\n", path);
        for (size_t i = 0; i < msgs.count; i++) {
            const char *label = strcasecmp(msgs.items[i].role, "assistant") == 0 ? "Assistant" : "User";
            buffer_appendf(&out, "## %s\n\n%s\n\n", label, msgs.items[i].content);
        }
    }
    if (output_path && *output_path) write_file_mode(output_path, out.data, 0600);
    else fputs(out.data, stdout);
    free(out.data); free(title); msglist_free(&msgs); free(path);
    return 0;
}

static void runtime_command_help(void) {
    printf("Runtime colon commands:\n");
    printf("  :help                         show this command list\n");
    printf("  :list                         list saved sessions\n");
    printf("  :rename TITLE                 set current session title metadata\n");
    printf("  :turns                        list message numbers in current session\n");
    printf("  :branch N                     branch current session after message N\n");
    printf("  :edit N                       edit user message N, branch, and resend\n");
    printf("  :stream [on|off]              toggle streaming assistant output\n");
    printf("  :status [on|off]              toggle local progress/status updates\n");
    printf("  :voice [FILE]                 transcribe with VOICE_INPUT_CMD and send\n");
    printf("  :speak [on|off|last]          toggle TTS or speak last reply\n");
    printf("  :models                       list cached models\n");
    printf("  :routers                      list router models\n");
    printf("  :update-models                refresh model cache from OpenRouter\n");
    printf("  :select-model                 open numbered cached model selector\n");
    printf("  :test-model MODEL             test one model\n");
    printf("  :model MODEL                  use model for this run\n");
    printf("  :model-save MODEL             use model and save MODEL config\n");
    printf("  :favorites                    list favorite models\n");
    printf("  :favorite MODEL               add favorite model\n");
    printf("  :unfavorite MODEL             remove favorite model\n");
    printf("  :unbookmark MODEL             alias for :unfavorite\n");
    printf("  :config                       print config summary\n");
    printf("  :set KEY VALUE                set config key\n");
    printf("  :unset KEY                    remove config key\n");
    printf("  :set-api-key [gpg]            store API key, optionally encrypted\n");
    printf("  :forget-api-key               remove stored API-key material\n");
    printf("  :memory                       show current context buffer\n");
    printf("  :edit-memory                  edit current memory/context in external editor\n");
    printf("  :clear-memory                 clear current context for future sends\n");
    printf("  :attach FILE                  queue attachment for next message\n");
    printf("  :attachments                  list pending attachments\n");
    printf("  :pending                      alias for :attachments\n");
    printf("  :clear-attachments            clear pending attachments\n");
    printf("  :editor                       compose one message in $VISUAL/$EDITOR/vi\n");
    printf("  :search TEXT                  search current session file\n");
    printf("  :search-all TEXT              search all logs in SESSION_DIR\n");
    printf("  :credits                      show project notice\n");
    printf("  :doctor                       run diagnostics\n");
    printf("  :quit                         exit interactive mode\n");
    printf("\nTo send a literal message beginning with ':' prefix it with '\\:'.\n");
}

static void free_pending_attachments(TCArgs *a) {
    for (size_t i = 0; i < a->attachment_count; i++) free(a->attachments[i]);
    free(a->attachments);
    a->attachments = NULL;
    a->attachment_count = 0;
}

static void interactive_context_add(TCMessageList *ctx, const char *role, const char *content, long context_turns) {
    msglist_add(ctx, role, content);
    long max_messages = context_turns * 2;
    if (max_messages < 2) max_messages = 2;
    if ((long)ctx->count <= max_messages) return;

    /*
     * TCMessageList is a tiny grow-only vector used throughout the C CLI.
     * In a long-running interactive loop it would otherwise grow forever.  The
     * graphical clients use CONTEXT_TURNS as a sliding window, so the C loop
     * does the same thing by freeing entries from the front and shifting the
     * remaining pointers down.  This is not the most algorithmically elegant
     * queue, but the context window is intentionally small and this keeps the
     * representation easy for future C maintainers to inspect.
     */
    size_t drop = ctx->count - (size_t)max_messages;
    for (size_t i = 0; i < drop; i++) {
        free(ctx->items[i].role);
        free(ctx->items[i].content);
    }
    memmove(ctx->items, ctx->items + drop, (ctx->count - drop) * sizeof(ctx->items[0]));
    ctx->count -= drop;
}

static void print_context_buffer(const TCMessageList *ctx) {
    if (ctx->count == 0) {
        printf("[memory empty]\n");
        return;
    }
    for (size_t i = 0; i < ctx->count; i++) {
        printf("%s: %s\n", ctx->items[i].role, ctx->items[i].content);
    }
}

static void search_file_lines(const char *path, const char *needle) {
    if (!path || !needle || !*needle) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "could not open %s: %s\n", path, strerror(errno));
        return;
    }
    char line[8192];
    long n = 0;
    while (fgets(line, sizeof(line), f)) {
        n++;
        if (contains_casefold(line, needle)) printf("%s:%ld:%s", path, n, line);
    }
    fclose(f);
}

static void search_all_sessions(const TCConfig *cfg, const char *needle) {
    DIR *d = opendir(cfg->session_dir);
    if (!d) {
        fprintf(stderr, "could not open session dir %s: %s\n", cfg->session_dir, strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *path = path_join2(cfg->session_dir, de->d_name);
        search_file_lines(path, needle);
        free(path);
    }
    closedir(d);
}


static const char *pick_external_editor(void);
static char *compose_with_external_editor(void);

/* ------------------------------------------------------------------------- */
/* Session utility commands                                                  */
/* ------------------------------------------------------------------------- */

static char *session_path_from_arg(const TCConfig *cfg, const char *arg) {
    if (!arg || !*arg) return NULL;
    if (strchr(arg, '/')) return expand_tilde(arg);
    const char *name = arg;
    char *with_ext = NULL;
    if (strlen(name) >= 4 && strcmp(name + strlen(name) - 4, ".log") == 0) {
        with_ext = xstrdup(name);
    } else {
        TCBuffer b;
        buffer_init(&b);
        buffer_appendf(&b, "%s.log", name);
        with_ext = b.data;
    }
    char *out = path_join2(cfg->session_dir, with_ext);
    free(with_ext);
    return out;
}

typedef struct TCSessionRow {
    char *name;
    char *path;
    char *title;
    time_t mtime;
    long size;
} TCSessionRow;

typedef struct TCSessionRows {
    TCSessionRow *items;
    size_t count;
    size_t cap;
} TCSessionRows;

static void sessionrows_add(TCSessionRows *rows, const char *name, const char *path, const char *title, time_t mtime, long size) {
    if (rows->count == rows->cap) {
        rows->cap = rows->cap ? rows->cap * 2 : 32;
        rows->items = realloc(rows->items, rows->cap * sizeof(rows->items[0]));
        if (!rows->items) die("out of memory");
    }
    rows->items[rows->count].name = xstrdup(name);
    rows->items[rows->count].path = xstrdup(path);
    rows->items[rows->count].title = xstrdup(title && *title ? title : "(untitled)");
    rows->items[rows->count].mtime = mtime;
    rows->items[rows->count].size = size;
    rows->count++;
}

static void sessionrows_free(TCSessionRows *rows) {
    for (size_t i = 0; i < rows->count; i++) {
        free(rows->items[i].name);
        free(rows->items[i].path);
        free(rows->items[i].title);
    }
    free(rows->items);
    rows->items = NULL;
    rows->count = rows->cap = 0;
}

static int sessionrow_cmp(const void *a, const void *b) {
    const TCSessionRow *ra = a;
    const TCSessionRow *rb = b;
    if (ra->mtime < rb->mtime) return 1;
    if (ra->mtime > rb->mtime) return -1;
    return strcasecmp(ra->name, rb->name);
}

static char *session_time_display(time_t t) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return xstrdup(buf);
}

static int list_sessions_command(const TCConfig *cfg) {
    DIR *d = opendir(cfg->session_dir);
    if (!d) {
        fprintf(stderr, "could not open session dir %s: %s\n", cfg->session_dir, strerror(errno));
        return 1;
    }
    TCSessionRows rows = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *path = path_join2(cfg->session_dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            char *title = session_title_from_file(path);
            if (!title) title = basename_no_ext(de->d_name);
            sessionrows_add(&rows, de->d_name, path, title, st.st_mtime, (long)st.st_size);
            free(title);
        }
        free(path);
    }
    closedir(d);

    qsort(rows.items, rows.count, sizeof(rows.items[0]), sessionrow_cmp);
    printf("%-42s %-16s %-10s %s\n", "TITLE", "UPDATED", "BYTES", "FILE");
    printf("%-42s %-16s %-10s %s\n", "-----", "-------", "-----", "----");
    for (size_t i = 0; i < rows.count; i++) {
        char *when = session_time_display(rows.items[i].mtime);
        printf("%-42.42s %-16s %-10ld %s\n", rows.items[i].title, when, rows.items[i].size, rows.items[i].name);
        free(when);
    }
    sessionrows_free(&rows);
    return 0;
}

static int rename_session_command(const TCConfig *cfg, const char *session_name, const char *new_title) {
    if (!session_name || !new_title || !*session_name || !*new_title) {
        fprintf(stderr, "usage: --rename-session SESSION TITLE\n");
        return 2;
    }
    char *path = session_path_from_arg(cfg, session_name);
    if (!file_exists(path)) {
        fprintf(stderr, "session not found: %s\n", path);
        free(path);
        return 1;
    }
    write_session_title_metadata(path, new_title);
    fprintf(stderr, "session title set for %s\n", path);
    free(path);
    return 0;
}

static void print_memory_for_file(const char *path, long context_turns) {
    TCMessageList ctx = load_context_from_session(path, context_turns);
    print_context_buffer(&ctx);
    msglist_free(&ctx);
}

static void write_context_snapshot_file(const char *path, const char *context_text) {
    TCBuffer b;
    buffer_init(&b);
    char *ts = now_hms();
    buffer_appendf(&b, "[%s] system: CONTEXT_BEGIN\n", ts);
    if (context_text && *context_text) {
        buffer_append(&b, context_text);
        if (b.len && b.data[b.len - 1] != '\n') buffer_append(&b, "\n");
    }
    buffer_appendf(&b, "[%s] system: CONTEXT_END\n", ts);
    append_file_text(path, b.data);
    free(ts);
    free(b.data);
}

static int clear_memory_command(const char *path) {
    if (!path || !*path) return 2;
    write_context_snapshot_file(path, "");
    fprintf(stderr, "empty memory snapshot appended to %s\n", path);
    return 0;
}

static int edit_memory_command(const TCConfig *cfg, const char *path) {
    (void)cfg;
    if (!path || !*path) return 2;
    TCMessageList ctx = load_context_from_session(path, 9999);
    TCBuffer initial;
    buffer_init(&initial);
    for (size_t i = 0; i < ctx.count; i++) {
        const char *label = strcasecmp(ctx.items[i].role, "assistant") == 0 ? "Assistant" : "User";
        char *safe = snapshot_safe_line(ctx.items[i].content);
        buffer_appendf(&initial, "%s: %s\n", label, safe);
        free(safe);
    }
    msglist_free(&ctx);

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "/tmp/ttychatter-memory-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(initial.data); return 1; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmpl); free(initial.data); return 1; }
    fputs(initial.data, f);
    fclose(f);
    free(initial.data);

    const char *ed = pick_external_editor();
    TCBuffer cmd;
    buffer_init(&cmd);
    char *q = shell_quote(tmpl);
    buffer_appendf(&cmd, "%s %s", ed, q);
    free(q);
    int rc = system(cmd.data);
    free(cmd.data);
    if (rc != 0) { unlink(tmpl); return 1; }
    size_t len = 0;
    char *edited = read_file(tmpl, &len);
    unlink(tmpl);
    write_context_snapshot_file(path, edited);
    free(edited);
    fprintf(stderr, "memory snapshot appended to %s\n", path);
    return 0;
}

static int editor_prompt_command(const TCConfig *cfg, const char *output_path, TCArgs *base_args) {
    char *msg = compose_with_external_editor();
    if (!msg || !*msg) { free(msg); fprintf(stderr, "editor returned empty prompt\n"); return 1; }
    char *owned_session = NULL;
    const char *session = output_path && *output_path ? output_path : (owned_session = default_session_path(cfg));
    maybe_write_prechat_and_startup_notice(cfg, base_args, session);
    TCMessageList ctx = load_context_from_session(session, cfg->context_turns);
    char *raw = openrouter_chat(cfg, base_args->model_override ? base_args->model_override : cfg->model, msg, &ctx, base_args);
    char *base = basename_no_ext(session);
    char *cleaned = extract_code_blocks(cfg, base, raw);
    append_turn(session, base_args->model_override ? base_args->model_override : cfg->model, "editor", base_args, msg, cleaned, &ctx, cfg->context_turns);
    maybe_generate_session_title(cfg, base_args, session, msg, cleaned);
    if (!effective_stream(cfg, base_args)) printf("%s\n", cleaned);
    if (effective_speak(cfg, base_args)) run_voice_output_command(cfg, cleaned);
    fprintf(stderr, "appended response to %s\n", session);
    free(msg); free(raw); free(base); free(cleaned); msglist_free(&ctx); free(owned_session);
    return 0;
}

static void credits_command(void) {
    printf("ttychatter - terminal chat clients for AI conversation.\n");
    printf("Project Lead notice: %s\n", TC_PRECHAT_LINE);
}

static const char *pick_external_editor(void) {
    const char *ed = getenv("VISUAL");
    if (ed && *ed) return ed;
    ed = getenv("EDITOR");
    if (ed && *ed) return ed;
    if (command_exists("vi")) return "vi";
    return NULL;
}

static char *compose_with_external_editor(void) {
    const char *editor = pick_external_editor();
    if (!editor) {
        fprintf(stderr, "no external editor found; set VISUAL or EDITOR\n");
        return NULL;
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "/tmp/ttychatter-compose.XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fprintf(stderr, "could not create temp file for editor\n");
        return NULL;
    }
    close(fd);
    char *qpath = shell_quote(tmpl);
    TCBuffer cmd;
    buffer_init(&cmd);
    buffer_appendf(&cmd, "%s %s", editor, qpath);
    char *cmds = buffer_take(&cmd);
    int rc = system(cmds);
    free(cmds);
    free(qpath);
    if (rc != 0) {
        unlink(tmpl);
        fprintf(stderr, "editor exited with nonzero status\n");
        return NULL;
    }
    size_t len = 0;
    char *text = read_file(tmpl, &len);
    unlink(tmpl);
    if (!text || !trim_in_place(text)[0]) {
        free(text);
        fprintf(stderr, "editor produced no message\n");
        return NULL;
    }
    return text;
}

static int interactive_send_message(TCConfig *cfg, const char *output_path, const char *model, const char *text,
                                    TCMessageList *ctx, TCArgs *pending_args, char **last_replyp) {
    if (!pending_args->demo && !cfg->api_key) {
        fprintf(stderr, "API key missing; use :set-api-key, set OPENROUTER_API_KEY, or run --set-api-key\n");
        return 1;
    }
    char *raw = openrouter_chat(cfg, model, text, ctx, pending_args);
    char *session_base = basename_no_ext(output_path);
    char *cleaned = extract_code_blocks(cfg, session_base, raw);
    append_turn(output_path, model, "interactive", pending_args, text, cleaned, ctx, cfg->context_turns);
    maybe_generate_session_title(cfg, pending_args, output_path, text, cleaned);
    interactive_context_add(ctx, "user", text, cfg->context_turns);
    interactive_context_add(ctx, "assistant", cleaned, cfg->context_turns);
    if (last_replyp) {
        free(*last_replyp);
        *last_replyp = xstrdup(cleaned);
    }
    if (!effective_stream(cfg, pending_args)) printf("%s\n", cleaned);
    if (effective_speak(cfg, pending_args)) run_voice_output_command(cfg, cleaned);
    free(raw);
    free(cleaned);
    free(session_base);
    free_pending_attachments(pending_args);
    return 0;
}

static int interactive_handle_command(TCConfig *cfg, TCArgs *pending_args, TCMessageList *ctx,
                                      char **output_pathp, char **active_model, char *line, char **last_replyp) {
    const char *output_path = *output_pathp;
    char *cmdline = trim_in_place(line + 1);
    char *cmd = strtok(cmdline, " \t");
    char *rest = strtok(NULL, "");
    rest = rest ? trim_in_place(rest) : NULL;
    if (!cmd || strcmp(cmd, "help") == 0) { runtime_command_help(); return 0; }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) return 1;
    if (strcmp(cmd, "list") == 0) { list_sessions_command(cfg); return 0; }
    if (strcmp(cmd, "rename") == 0) {
        if (!rest || !*rest) { fprintf(stderr, "usage: :rename TITLE\n"); return 0; }
        write_session_title_metadata(*output_pathp, rest);
        fprintf(stderr, "current session title set: %s\n", rest);
        return 0;
    }
    if (strcmp(cmd, "turns") == 0) { print_turns_for_file(*output_pathp); return 0; }
    if (strcmp(cmd, "branch") == 0 || strcmp(cmd, "fork") == 0) {
        long turn = parse_turn_operand(rest);
        if (turn < 0) { fprintf(stderr, "usage: :branch MESSAGE_NUMBER\n"); return 0; }
        TCMessageList msgs = load_messages_from_session_file(*output_pathp);
        if ((size_t)turn > msgs.count) {
            fprintf(stderr, "turn out of range: %ld (session has %zu messages)\n", turn, msgs.count);
            msglist_free(&msgs);
            return 0;
        }
        char *old_path = xstrdup(*output_pathp);
        char *branch_path = create_branch_file_from_prefix(cfg, pending_args, old_path, &msgs, (size_t)turn, turn, "interactive-branch");
        free(*output_pathp);
        *output_pathp = branch_path;
        msglist_free(ctx);
        *ctx = load_context_from_session(*output_pathp, cfg->context_turns);
        fprintf(stderr, "branched from %s at message %ld; current session is %s\n", old_path, turn, *output_pathp);
        free(old_path);
        msglist_free(&msgs);
        return 0;
    }
    if (strcmp(cmd, "edit") == 0 || strcmp(cmd, "edit-turn") == 0) {
        long turn = parse_turn_operand(rest);
        if (turn <= 0) { fprintf(stderr, "usage: :edit MESSAGE_NUMBER\n"); return 0; }
        TCMessageList msgs = load_messages_from_session_file(*output_pathp);
        if ((size_t)turn > msgs.count) {
            fprintf(stderr, "turn out of range: %ld (session has %zu messages)\n", turn, msgs.count);
            msglist_free(&msgs);
            return 0;
        }
        TCMessage *target = &msgs.items[(size_t)turn - 1];
        if (strcasecmp(target->role, "user") != 0) {
            fprintf(stderr, ":edit expects a user message number; run :turns first\n");
            msglist_free(&msgs);
            return 0;
        }
        char *edited = edit_text_with_external_editor(target->content);
        if (!edited) { msglist_free(&msgs); return 0; }
        TCMessageList prefix = msglist_prefix_copy(&msgs, (size_t)turn - 1);
        char *old_path = xstrdup(*output_pathp);
        char *branch_path = create_branch_file_from_prefix(cfg, pending_args, old_path, &msgs, (size_t)turn - 1, turn, "interactive-edited-user-turn");
        free(*output_pathp);
        *output_pathp = branch_path;
        msglist_free(ctx);
        *ctx = prefix;
        fprintf(stderr, "editing message %ld; branched to %s\n", turn, *output_pathp);
        interactive_send_message(cfg, *output_pathp, *active_model, edited, ctx, pending_args, last_replyp);
        free(edited);
        free(old_path);
        msglist_free(&msgs);
        return 0;
    }
    if (strcmp(cmd, "voice") == 0 || strcmp(cmd, "transcribe") == 0) {
        char *msg = run_voice_input_command(cfg, rest);
        if (msg) { interactive_send_message(cfg, *output_pathp, *active_model, msg, ctx, pending_args, last_replyp); free(msg); }
        return 0;
    }
    if (strcmp(cmd, "speak") == 0) {
        if (!rest || strcmp(rest, "last") == 0) {
            if (last_replyp && *last_replyp) run_voice_output_command(cfg, *last_replyp);
            else fprintf(stderr, "no last assistant reply to speak\n");
            return 0;
        }
        if (strcasecmp(rest, "on") == 0) { pending_args->speak_set = true; pending_args->speak = true; fprintf(stderr, "voice output enabled for this interactive session\n"); return 0; }
        if (strcasecmp(rest, "off") == 0) { pending_args->speak_set = true; pending_args->speak = false; fprintf(stderr, "voice output disabled for this interactive session\n"); return 0; }
        fprintf(stderr, "usage: :speak [on|off|last]\n");
        return 0;
    }
    if (strcmp(cmd, "stream") == 0) {
        if (!rest || strcasecmp(rest, "on") == 0) { pending_args->stream_set = true; pending_args->stream = true; fprintf(stderr, "streaming enabled for this interactive session\n"); return 0; }
        if (strcasecmp(rest, "off") == 0) { pending_args->stream_set = true; pending_args->stream = false; fprintf(stderr, "streaming disabled for this interactive session\n"); return 0; }
        fprintf(stderr, "usage: :stream [on|off]\n");
        return 0;
    }
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "progress") == 0) {
        if (!rest || strcasecmp(rest, "on") == 0) { pending_args->status_set = true; pending_args->status_updates = true; fprintf(stderr, "status updates enabled for this interactive session\n"); return 0; }
        if (strcasecmp(rest, "off") == 0) { pending_args->status_set = true; pending_args->status_updates = false; fprintf(stderr, "status updates disabled for this interactive session\n"); return 0; }
        fprintf(stderr, "usage: :status [on|off]\n");
        return 0;
    }
    if (strcmp(cmd, "models") == 0) { list_models(cfg, NULL, pending_args->model_type, pending_args->model_sort, false, pending_args->demo); return 0; }
    if (strcmp(cmd, "routers") == 0) { list_models(cfg, NULL, "routers", pending_args->model_sort, false, pending_args->demo); return 0; }
    if (strcmp(cmd, "update-models") == 0) { update_models(cfg, pending_args->demo); return 0; }
    if (strcmp(cmd, "select-model") == 0) { select_model_command(cfg, pending_args->demo); return 0; }
    if (strcmp(cmd, "favorites") == 0) { list_favorites(cfg); return 0; }
    if (strcmp(cmd, "favorite") == 0) { if (rest) add_favorite(cfg, rest); else fprintf(stderr, "usage: :favorite MODEL\n"); return 0; }
    if (strcmp(cmd, "unfavorite") == 0 || strcmp(cmd, "unbookmark") == 0) { if (rest) remove_favorite(cfg, rest); else fprintf(stderr, "usage: :unfavorite MODEL\n"); return 0; }
    if (strcmp(cmd, "test-model") == 0) { if (rest) test_model_command(cfg, rest, pending_args->demo); else fprintf(stderr, "usage: :test-model MODEL\n"); return 0; }
    if (strcmp(cmd, "model") == 0) { if (rest) { free(*active_model); *active_model = xstrdup(rest); fprintf(stderr, "runtime model: %s\n", *active_model); } else fprintf(stderr, "usage: :model MODEL\n"); return 0; }
    if (strcmp(cmd, "model-save") == 0) { if (rest) { free(*active_model); *active_model = xstrdup(rest); config_write_key_value(cfg, "MODEL", rest); fprintf(stderr, "saved MODEL=%s\n", rest); } else fprintf(stderr, "usage: :model-save MODEL\n"); return 0; }
    if (strcmp(cmd, "config") == 0) { print_config_summary(cfg); return 0; }
    if (strcmp(cmd, "set") == 0) {
        if (!rest) { fprintf(stderr, "usage: :set KEY VALUE\n"); return 0; }
        char *key = strtok(rest, " \t");
        char *val = strtok(NULL, "");
        if (!key || !val) { fprintf(stderr, "usage: :set KEY VALUE\n"); return 0; }
        val = trim_in_place(val);
        config_write_key_value(cfg, key, val);
        config_set(cfg, key, val);
        fprintf(stderr, "set %s\n", key);
        return 0;
    }
    if (strcmp(cmd, "unset") == 0) { if (rest) { config_remove_key(cfg, rest); fprintf(stderr, "removed %s\n", rest); } else fprintf(stderr, "usage: :unset KEY\n"); return 0; }
    if (strcmp(cmd, "set-api-key") == 0) { cmd_set_api_key(cfg, rest && strcmp(rest, "gpg") == 0); config_load(cfg); return 0; }
    if (strcmp(cmd, "forget-api-key") == 0) { cmd_forget_api_key(cfg); free(cfg->api_key); cfg->api_key = NULL; return 0; }
    if (strcmp(cmd, "memory") == 0) { print_context_buffer(ctx); return 0; }
    if (strcmp(cmd, "edit-memory") == 0) {
        edit_memory_command(cfg, output_path);
        msglist_free(ctx);
        *ctx = load_context_from_session(output_path, cfg->context_turns);
        fprintf(stderr, "reloaded memory from edited snapshot\n");
        return 0;
    }
    if (strcmp(cmd, "clear-memory") == 0) { msglist_free(ctx); ctx->items = NULL; ctx->count = 0; ctx->cap = 0; clear_memory_command(output_path); return 0; }
    if (strcmp(cmd, "attach") == 0) { if (rest) { args_add_attachment(pending_args, rest); fprintf(stderr, "queued attachment: %s\n", rest); } else fprintf(stderr, "usage: :attach FILE\n"); return 0; }
    if (strcmp(cmd, "attachments") == 0 || strcmp(cmd, "pending") == 0) { if (pending_args->attachment_count == 0) printf("[no pending attachments]\n"); for (size_t i = 0; i < pending_args->attachment_count; i++) printf("%zu: %s\n", i + 1, pending_args->attachments[i]); return 0; }
    if (strcmp(cmd, "clear-attachments") == 0) { free_pending_attachments(pending_args); fprintf(stderr, "cleared pending attachments\n"); return 0; }
    if (strcmp(cmd, "editor") == 0) { char *msg = compose_with_external_editor(); if (msg) { interactive_send_message(cfg, output_path, *active_model, msg, ctx, pending_args, last_replyp); free(msg); } return 0; }
    if (strcmp(cmd, "search") == 0) { if (rest) search_file_lines(output_path, rest); else fprintf(stderr, "usage: :search TEXT\n"); return 0; }
    if (strcmp(cmd, "search-all") == 0) { if (rest) search_all_sessions(cfg, rest); else fprintf(stderr, "usage: :search-all TEXT\n"); return 0; }
    if (strcmp(cmd, "credits") == 0) { printf("ttychatter - terminal chat clients. Project Lead notice: %s\n", TC_PRECHAT_LINE); return 0; }
    if (strcmp(cmd, "doctor") == 0) { doctor(cfg); return 0; }
    fprintf(stderr, "unknown command: :%s (try :help)\n", cmd);
    return 0;
}

static int interactive_loop(TCConfig *cfg, TCArgs *base_args, const char *maybe_output_path) {
    char *output_path = maybe_output_path ? xstrdup(maybe_output_path) : default_session_path(cfg);
    mkdir_p(cfg->session_dir);
    mkdir_p(cfg->attachment_dir);
    maybe_write_prechat_and_startup_notice(cfg, base_args, output_path);
    TCMessageList ctx = load_context_from_session(output_path, cfg->context_turns);
    TCArgs pending;
    args_init(&pending);
    pending.demo = base_args->demo;
    pending.loopback = base_args->loopback;
    pending.stream_set = base_args->stream_set;
    pending.stream = base_args->stream;
    pending.status_set = base_args->status_set;
    pending.status_updates = base_args->status_updates;
    pending.speak_set = base_args->speak_set;
    pending.speak = base_args->speak;
    free(pending.model_type);
    pending.model_type = xstrdup(base_args->model_type ? base_args->model_type : "all");
    pending.model_sort = base_args->model_sort ? xstrdup(base_args->model_sort) : NULL;
    char *active_model = xstrdup(base_args->model_override ? base_args->model_override : cfg->model);
    char *last_reply = NULL;

    printf("ttychatter interactive session: %s\n", output_path);
    printf("Type :help for local commands.  Lines beginning with ':' are not sent to the AI.\n");
    printf("%s\n", TC_PRECHAT_LINE);
    char line[65536];
    while (true) {
        printf("ttychatter> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *s = trim_in_place(line);
        if (!*s) continue;
        if (s[0] == ':' && s[1] != '\0') {
            int done = interactive_handle_command(cfg, &pending, &ctx, &output_path, &active_model, s, &last_reply);
            if (done) break;
            continue;
        }
        if (s[0] == '\\' && s[1] == ':') s++;
        interactive_send_message(cfg, output_path, active_model, s, &ctx, &pending, &last_reply);
    }
    free_pending_attachments(&pending);
    free(pending.model_type);
    free(pending.model_sort);
    free(active_model);
    free(last_reply);
    msglist_free(&ctx);
    free(output_path);
    return 0;
}


int main(int argc, char **argv) {
    TCConfig cfg;
    config_init_defaults(&cfg);
    config_load(&cfg);
    mkdir_p(cfg.session_dir);
    mkdir_p(cfg.attachment_dir);

    TCArgs args;
    args_init(&args);
    if (parse_args(argc, argv, &args) != 0) return 2;
    if (cfg.demo_mode_default) args.demo = true;
    if (cfg.loopback_default) args.loopback = true;

    if (args.show_help) { print_help(); return 0; }
    if (args.show_version) { print_version(); return 0; }
    if (args.set_api_key) return cmd_set_api_key(&cfg, args.set_api_key_gpg);
    if (args.forget_api_key) return cmd_forget_api_key(&cfg);
    if (args.doctor) return doctor(&cfg);
    if (args.credits_cmd) { credits_command(); return 0; }
    if (args.list_sessions_cmd) return list_sessions_command(&cfg);
    if (args.rename_session_cmd) return rename_session_command(&cfg, args.rename_old, args.rename_new);
    if (args.turns_cmd) return turns_command(&cfg, args.turns_session);
    if (args.branch_cmd) return branch_session_command(&cfg, &args, args.branch_session, args.branch_turn);
    if (args.edit_turn_cmd) { if (!confirm_live_send_if_needed(&cfg, &args)) return 1; return edit_turn_command(&cfg, &args, args.edit_turn_session, args.edit_turn); }
    if (args.export_cmd) return export_session_command(&cfg, args.export_session, args.export_format, args.output_path);
    if (args.show_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); print_memory_for_file(p, cfg.context_turns); free(p); return 0; }
    if (args.clear_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); int rc = clear_memory_command(p); free(p); return rc; }
    if (args.edit_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); int rc = edit_memory_command(&cfg, p); free(p); return rc; }
    if (args.search_sessions_cmd || (args.search && !args.list_models)) { search_all_sessions(&cfg, args.search ? args.search : ""); return 0; }
    if (args.editor_prompt_cmd) {
        if (args.session_arg && args.output_path) die("use either --session SESSION or an explicit output log, not both");
        if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");
        char *editor_session_path = args.session_arg ? session_path_from_arg(&cfg, args.session_arg) : NULL;
        int rc = editor_prompt_command(&cfg, editor_session_path ? editor_session_path : args.output_path, &args);
        free(editor_session_path);
        return rc;
    }
    if (args.show_config) { print_config_summary(&cfg); return 0; }
    if (args.config_set_cmd) {
        if (!args.config_key || !args.config_value) die("--set requires KEY=VALUE");
        config_write_key_value(&cfg, args.config_key, args.config_value);
        fprintf(stderr, "set %s in %s\n", args.config_key, cfg.config_file);
        return 0;
    }
    if (args.config_unset_cmd) {
        if (!args.config_key) die("--unset requires KEY");
        config_remove_key(&cfg, args.config_key);
        fprintf(stderr, "removed %s from %s\n", args.config_key, cfg.config_file);
        return 0;
    }
    if (args.favorites) return list_favorites(&cfg);
    if (args.favorite_model) return add_favorite(&cfg, args.test_model_id);
    if (args.unfavorite_model) return remove_favorite(&cfg, args.test_model_id);
    if (args.update_models) return update_models(&cfg, args.demo);
    if (args.select_model_cmd) return select_model_command(&cfg, args.demo);
    if (args.list_models) {
        if (args.show_all_models) {
            free(args.model_type); args.model_type = xstrdup("all");
            cfg.model_filter_require_tokens = false;
            cfg.model_filter_hide_preview = false;
            cfg.model_min_input_tokens = 0;
            cfg.model_min_output_tokens = 0;
        }
        if (args.require_tokens) cfg.model_filter_require_tokens = true;
        if (args.allow_missing_tokens) cfg.model_filter_require_tokens = false;
        if (args.hide_preview) cfg.model_filter_hide_preview = true;
        if (args.show_preview) cfg.model_filter_hide_preview = false;
        if (args.min_input_tokens > 0) cfg.model_min_input_tokens = args.min_input_tokens;
        if (args.min_output_tokens > 0) cfg.model_min_output_tokens = args.min_output_tokens;
        return list_models(&cfg, args.search, args.model_type, args.model_sort, args.favorites_only, args.demo);
    }
    if (args.test_model) {
        if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");
        int rc = test_model_command(&cfg, args.test_model_id, args.demo);
        if (rc == 0 && args.save && args.test_model_id) {
            config_write_key_value(&cfg, "MODEL", args.test_model_id);
            fprintf(stderr, "saved MODEL=%s\n", args.test_model_id);
        }
        return rc;
    }

    if (args.voice_input_path) {
        if (args.prompt_text || args.input_path) {
            fprintf(stderr, "use --voice-input/--transcribe by itself, not with --prompt or an input file\n");
            return 2;
        }
        args.prompt_text = run_voice_input_command(&cfg, args.voice_input_path);
        if (!args.prompt_text) return 1;
    }
    if (args.prompt_text && args.input_path) {
        fprintf(stderr, "use either --prompt TEXT or an input file, not both\n");
        return 2;
    }
    if (args.session_arg && args.output_path) {
        fprintf(stderr, "use either --session SESSION or an explicit output log, not both\n");
        return 2;
    }
    if (!args.interactive && !args.prompt_text && !args.input_path && isatty(STDIN_FILENO)) {
        args.interactive = true;
    }

    if (args.interactive) {
        if (args.prompt_text) {
            fprintf(stderr, "interactive mode does not accept --prompt; omit -i for one-shot sends\n");
            return 2;
        }
        if (args.output_path) {
            fprintf(stderr, "interactive mode accepts --session SESSION, --resume SESSION, or one positional session name/path, not an output log\n");
            return 2;
        }
        int session_sources = 0;
        if (args.resume_session) session_sources++;
        if (args.session_arg) session_sources++;
        if (args.input_path) session_sources++;
        if (args.new_session_cmd && session_sources > 0) {
            fprintf(stderr, "--new starts a fresh timestamped interactive session and does not accept a session name/path\n");
            return 2;
        }
        if (session_sources > 1) {
            fprintf(stderr, "interactive mode accepts only one session name/path\n");
            return 2;
        }
        char *session_path = NULL;
        if (!args.new_session_cmd && args.resume_session) session_path = session_path_from_arg(&cfg, args.resume_session);
        else if (!args.new_session_cmd && args.session_arg) session_path = session_path_from_arg(&cfg, args.session_arg);
        else if (!args.new_session_cmd && args.input_path) session_path = session_path_from_arg(&cfg, args.input_path);
        int rc = interactive_loop(&cfg, &args, session_path);
        free(session_path);
        return rc;
    }

    if (!args.prompt_text && !args.input_path) args.input_path = xstrdup("-");
    if (!args.output_path) {
        if (args.session_arg) args.output_path = session_path_from_arg(&cfg, args.session_arg);
        else args.output_path = default_session_path(&cfg);
    }
    if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");

    if (!confirm_live_send_if_needed(&cfg, &args)) return 1;
    maybe_write_prechat_and_startup_notice(&cfg, &args, args.output_path);

    size_t input_len = 0;
    char *input_text = args.prompt_text ? xstrdup(args.prompt_text) : read_input_source(args.input_path, &input_len);
    const char *input_label = args.prompt_text ? "prompt" : (args.input_path ? args.input_path : "stdin");
    TCMessageList context = load_context_from_session(args.output_path, cfg.context_turns);
    const char *model = args.model_override ? args.model_override : cfg.model;

    status_update(&cfg, &args, "loading context from %s", args.output_path);
    char *raw = openrouter_chat(&cfg, model, input_text, &context, &args);
    char *session_base = basename_no_ext(args.output_path);
    char *cleaned = extract_code_blocks(&cfg, session_base, raw);

    append_turn(args.output_path, model, input_label, &args, input_text, cleaned, &context, cfg.context_turns);
    maybe_generate_session_title(&cfg, &args, args.output_path, input_text, cleaned);
    status_update(&cfg, &args, "saving response to %s", args.output_path);
    fprintf(stderr, "appended response to %s\n", args.output_path);
    if (args.loopback && !effective_stream(&cfg, &args)) printf("%s\n", cleaned);
    if (effective_speak(&cfg, &args)) run_voice_output_command(&cfg, cleaned);
    if (args.loopback_file) {
        append_file_text(args.loopback_file, cleaned);
        append_file_text(args.loopback_file, "\n");
    }

    free(input_text);
    free(raw);
    free(cleaned);
    free(session_base);
    msglist_free(&context);
    curl_global_cleanup();
    return 0;
}
