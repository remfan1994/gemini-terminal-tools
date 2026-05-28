/*
 * ttychatter.c - command-line OpenRouter client for the ttychatter project
 *
 * This C edition is deliberately NOT an ncurses program.  The Python ncurses
 * edition is the full terminal application.  This file is the traditional Unix
 * command-line client: the user prepares an input file, runs ttychatter with an
 * input path and an output/session path, and ttychatter appends the AI response
 * to the output/session file.
 *
 * The design target is the classic shell workflow:
 *
 *     $EDITOR prompt.txt
 *     ttychatter -a notes.txt prompt.txt session.log
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
#define TC_VERSION "0.5.0-c-cli"
#define TC_OPENROUTER_CHAT_URL "https://openrouter.ai/api/v1/chat/completions"
#define TC_OPENROUTER_MODELS_URL "https://openrouter.ai/api/v1/models"

/*
 * The log markers are intentionally loud and simple.  They are not pretty prose;
 * they are parse anchors.  A future awk/sed/perl/C/Rust implementation should be
 * able to recover user and AI turns without understanding a binary database or a
 * private serialization format.  That is why the output file is both readable
 * and deliberately structured.
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
    char *model_cache_file;
    char *model_favorites_file;
    char *session_dir;
    char *attachment_dir;
    long context_turns;
    long code_attachment_min_lines;
    long max_attachment_bytes;
    char *theme;
    char *send_input;
    char *editor_cmd;
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
    bool show_all_models;
    char *loopback_file;
    char *interactive_session_path;
    char *config_key;
    char *config_value;
    char *rename_old;
    char *rename_new;
    char *memory_path;
    char *search;
    char *model_type;
    char *model_sort;
    long min_input_tokens;
    long min_output_tokens;
    bool require_tokens;
    bool hide_preview;
    bool favorites_only;
    bool yes;
    char *model_override;
    char *test_model_id;
    char *input_path;
    char *output_path;
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

static char *xdg_config_file(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter");
        char *file = path_join2(dir, "config");
        free(dir);
        return file;
    }
    return home_path(".config/ttychatter/config");
}

static char *xdg_cache_file(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter");
        char *file = path_join2(dir, "models.json");
        free(dir);
        return file;
    }
    return home_path(".cache/ttychatter/models.json");
}

static char *xdg_data_dir(const char *leaf) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        char *dir = path_join2(xdg, "ttychatter");
        char *out = path_join2(dir, leaf);
        free(dir);
        return out;
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), ".local/share/ttychatter/%s", leaf);
    return home_path(tmp);
}

static void config_init_defaults(TCConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_file = xdg_config_file();
    cfg->api_key_gpg_file = home_path(".config/ttychatter/api-key.gpg");
    cfg->model = xstrdup("openrouter/auto");
    cfg->model_cache_file = xdg_cache_file();
    cfg->model_favorites_file = home_path(".config/ttychatter/model-favorites");
    cfg->session_dir = xdg_data_dir("sessions");
    cfg->attachment_dir = xdg_data_dir("attachments");
    cfg->context_turns = 12;
    cfg->code_attachment_min_lines = 5;
    cfg->max_attachment_bytes = 1024 * 1024;
    cfg->theme = xstrdup("default");
    cfg->send_input = xstrdup("file");
    cfg->editor_cmd = xstrdup("");
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
    cfg->demo_mode_default = false;
    cfg->loopback_default = false;
}

static void config_set(TCConfig *cfg, const char *key, const char *value) {
    if (strcmp(key, "OPENROUTER_API_KEY") == 0 || strcmp(key, "TTYCHATTER_API_KEY") == 0) {
        free(cfg->api_key);
        cfg->api_key = xstrdup(value);
    } else if (strcmp(key, "API_KEY_GPG_FILE") == 0 || strcmp(key, "OPENROUTER_API_KEY_GPG_FILE") == 0) {
        free(cfg->api_key_gpg_file);
        cfg->api_key_gpg_file = expand_tilde(value);
    } else if (strcmp(key, "MODEL") == 0) {
        free(cfg->model);
        cfg->model = xstrdup(value);
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
    } else if (strcmp(key, "CONTEXT_TURNS") == 0) {
        cfg->context_turns = atol(value) > 0 ? atol(value) : cfg->context_turns;
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
    } else if (strcmp(key, "MODEL_TEST_PROMPT") == 0) {
        free(cfg->model_test_prompt);
        cfg->model_test_prompt = xstrdup(value);
    } else if (strcmp(key, "MODEL_SORT_ORDER") == 0) {
        free(cfg->model_sort_order);
        cfg->model_sort_order = xstrdup(value);
    } else if (strcmp(key, "MODEL_TYPE_FILTER") == 0) {
        free(cfg->model_type_filter);
        cfg->model_type_filter = xstrdup(value);
    } else if (strcmp(key, "MODEL_FILTER_GENERATE_CONTENT") == 0) {
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
    } else if (strcmp(key, "DEMO_MODE") == 0) {
        cfg->demo_mode_default = parse_bool_string(value, cfg->demo_mode_default);
    } else if (strcmp(key, "LOOPBACK") == 0) {
        cfg->loopback_default = parse_bool_string(value, cfg->loopback_default);
    }
}

static void config_load(TCConfig *cfg) {
    FILE *f = fopen(cfg->config_file, "r");
    if (!f) goto env_keys;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_in_place(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_in_place(s);
        char *value = trim_in_place(eq + 1);
        config_set(cfg, key, value);
    }
    fclose(f);

env_keys:
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ttychatter-c-cli/0.1");
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
    "{\"id\":\"openrouter/auto\",\"name\":\"Demo Auto Router\",\"context_length\":128000},"
    "{\"id\":\"openrouter/free\",\"name\":\"Demo Free Router\",\"context_length\":64000},"
    "{\"id\":\"demo/fixed-small\",\"name\":\"Demo Fixed Small\",\"context_length\":16000},"
    "{\"id\":\"demo/fixed-large\",\"name\":\"Demo Fixed Large\",\"context_length\":256000}"
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

static void modelrows_add(TCModelRows *rows, const char *id, const char *name, long context, bool favorite) {
    if (rows->count == rows->cap) {
        rows->cap = rows->cap ? rows->cap * 2 : 32;
        rows->items = realloc(rows->items, rows->cap * sizeof(rows->items[0]));
        if (!rows->items) die("out of memory");
    }
    rows->items[rows->count].id = xstrdup(id ? id : "");
    rows->items[rows->count].name = xstrdup(name ? name : "");
    rows->items[rows->count].context = context;
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
        if (!model_type_matches(id, cfg->model_type_filter)) continue;
        if (cfg->model_filter_require_tokens && ctx <= 0) continue;
        if (cfg->model_min_input_tokens > 0 && ctx < cfg->model_min_input_tokens) continue;
        if (cfg->model_filter_hide_preview && (contains_casefold(id, "preview") || contains_casefold(name, "preview") || contains_casefold(id, "experimental") || contains_casefold(name, "experimental"))) continue;
        modelrows_add(&rows, id, name, ctx, favorite_contains(cfg, id));
    }
    json_object_put(root);
    g_model_sort_key = cfg->model_sort_order;
    qsort(rows.items, rows.count, sizeof(rows.items[0]), modelrow_cmp);
    if (rows.count == 0) {
        fprintf(stderr, "No models matched current filters.\n");
        modelrows_free(&rows);
        return 1;
    }
    for (size_t i = 0; i < rows.count; i++) printf("%3zu) %-42.42s %-14s %ld\n", i + 1, rows.items[i].id, model_type_of(rows.items[i].id), rows.items[i].context);
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
        bool fav = favorite_contains(cfg, id);
        const char *effective_type = (type_filter && *type_filter) ? type_filter : cfg->model_type_filter;
        if (!model_type_matches(id, effective_type)) continue;
        if (favorites_only && !fav) continue;
        if (cfg->model_filter_require_tokens && ctx <= 0) continue;
        if (cfg->model_min_input_tokens > 0 && ctx < cfg->model_min_input_tokens) continue;
        if (cfg->model_filter_hide_preview && (contains_casefold(id, "preview") || contains_casefold(name, "preview") || contains_casefold(id, "experimental") || contains_casefold(name, "experimental"))) continue;
        if (search && *search && !contains_casefold(id, search) && !contains_casefold(name, search)) continue;
        modelrows_add(&rows, id, name, ctx, fav);
    }
    json_object_put(root);

    g_model_sort_key = sort_key && *sort_key ? sort_key : cfg->model_sort_order;
    qsort(rows.items, rows.count, sizeof(rows.items[0]), modelrow_cmp);

    printf("%-2s %-42s %-14s %-10s %s\n", "F", "MODEL", "TYPE", "CONTEXT", "NAME");
    printf("%-2s %-42s %-14s %-10s %s\n", "-", "-----", "----", "-------", "----");
    for (size_t i = 0; i < rows.count; i++) {
        printf("%-2s %-42.42s %-14s %-10ld %s\n",
               rows.items[i].favorite ? "*" : "",
               rows.items[i].id,
               model_type_of(rows.items[i].id),
               rows.items[i].context,
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
    list->items[list->count].role = xstrdup(role);
    list->items[list->count].content = xstrdup(content);
    list->count++;
}

static void msglist_free(TCMessageList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].role);
        free(list->items[i].content);
    }
    free(list->items);
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

static TCMessageList load_context_from_session(const char *output_path, long context_turns) {
    TCMessageList all = {0};
    if (!file_exists(output_path)) return all;
    size_t len = 0;
    char *text = read_file(output_path, &len);

    /*
     * Memory management in the C CLI is deliberately file-shaped.  The output
     * log is the session, so commands such as --clear-memory and --edit-memory
     * leave explicit markers in that same human-readable file rather than
     * creating a hidden state database.  The parser first looks for the most
     * recent explicit context snapshot; if one exists, it becomes the canonical
     * resume context.  If no snapshot exists, the parser falls back to ordinary
     * user/AI turns after the most recent memory-clear marker.
     */
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
            free(text);
            return snap;
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
    free(text);

    long max_messages = context_turns * 2;
    if (max_messages < 2) max_messages = 2;
    if ((long)all.count <= max_messages) return all;

    TCMessageList clipped = {0};
    size_t start = all.count - (size_t)max_messages;
    for (size_t i = start; i < all.count; i++) msglist_add(&clipped, all.items[i].role, all.items[i].content);
    msglist_free(&all);
    return clipped;
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
    (void)cfg;
    (void)context;
    if (args && args->demo) return demo_chat_response(model, user_text);
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
    const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    char *payload_copy = xstrdup(payload);
    json_object_put(root);

    TCHttpResponse r = http_request(TC_OPENROUTER_CHAT_URL, "POST", cfg->api_key, payload_copy);
    free(payload_copy);
    if (r.status < 200 || r.status >= 300) {
        TCBuffer err;
        buffer_init(&err);
        buffer_appendf(&err, "OpenRouter HTTP %ld\n%s", r.status, r.body ? r.body : "");
        free(r.body);
        return buffer_take(&err);
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
    return out;
}

/* ------------------------------------------------------------------------- */
/* Session log append                                                        */
/* ------------------------------------------------------------------------- */

static void append_turn(const char *output_path, const char *model, const char *input_path, TCArgs *args, const char *user_text, const char *ai_text) {
    TCBuffer b;
    buffer_init(&b);
    char *now = now_string();
    buffer_appendf(&b, "%s\n", TC_TURN_BEGIN);
    buffer_appendf(&b, "time: %s\n", now);
    buffer_appendf(&b, "model: %s\n", model);
    buffer_appendf(&b, "input-file: %s\n", input_path);
    for (size_t i = 0; i < args->attachment_count; i++) buffer_appendf(&b, "attachment: %s\n", args->attachments[i]);
    buffer_appendf(&b, "%s\n%s\n%s\n", TC_USER_BEGIN, user_text, TC_USER_END);
    buffer_appendf(&b, "%s\n%s\n%s\n", TC_AI_BEGIN, ai_text, TC_AI_END);
    buffer_appendf(&b, "%s\n\n", TC_TURN_END);
    append_file_text(output_path, b.data);
    free(now);
    free(b.data);
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
    printf("  ttychatter [options] input.txt output.log\n");
    printf("  ttychatter --interactive [session.log]\n");
    printf("  ttychatter --set-api-key [--gpg]\n");
    printf("  ttychatter --models [--search TEXT] [--model-type TYPE] [--sort KEY]\n");
    printf("  ttychatter --select-model\n");
    printf("  ttychatter --list | --rename-session OLD NEW\n");
    printf("  ttychatter --show-memory SESSION | --clear-memory SESSION | --edit-memory SESSION\n");
    printf("  ttychatter --editor-prompt [session.log]\n");
    printf("  ttychatter --search TEXT [--all-sessions]\n");
    printf("\nOptions:\n");
    printf("  -m, --model MODEL        model/router to use (default from config)\n");
    printf("  -a, --attach FILE        attach file; repeatable\n");
    printf("  -l, --loopback           also print AI output to stdout\n");
    printf("      --loopback-file FILE mirror clean AI output to FILE\n");
    printf("      --demo               use local demo models/responses; no network/API key\n");
    printf("  -i, --interactive        line-oriented chat loop with colon commands\n");
    printf("      --list               list session logs in SESSION_DIR\n");
    printf("      --rename-session OLD NEW rename a session log\n");
    printf("      --search TEXT        filter model list with --models, otherwise search sessions\n");
    printf("      --search-sessions TEXT search all session logs\n");
    printf("      --model-type TYPE    all, routers, fixed, free, auto\n");
    printf("      --sort KEY           name, context, tokens, type, favorites\n");
    printf("      --min-input-tokens N filter model rows by context length\n");
    printf("      --hide-preview       hide preview/experimental-looking model IDs\n");
    printf("      --favorites-only     show only favorite models in --models\n");
    printf("      --yes                confirm live send when CONFIRM_LIVE_SEND=1\n");
    printf("      --select-model       numbered cached model selector\n");
    printf("      --set-api-key        save API key\n");
    printf("      --gpg                encrypt API key with gpg when used with --set-api-key\n");
    printf("      --forget-api-key     remove stored plaintext/encrypted API key\n");
    printf("      --config             print config summary\n");
    printf("      --set KEY=VALUE      write a config key\n");
    printf("      --unset KEY          remove a config key\n");
    printf("      --doctor             local diagnostics\n");
    printf("  -h, --help               show help\n");
    printf("  -v, --version            show version\n");
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
    printf("attachments: %s\n", cfg->attachment_dir);
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

static int parse_args(int argc, char **argv, TCArgs *args) {
    static struct option longopts[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"model", required_argument, 0, 'm'},
        {"attach", required_argument, 0, 'a'},
        {"loopback", no_argument, 0, 'l'},
        {"set-api-key", no_argument, 0, 1000},
        {"gpg", no_argument, 0, 1001},
        {"forget-api-key", no_argument, 0, 1002},
        {"models", no_argument, 0, 1003},
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
        {"loopback-file", required_argument, 0, 1016},
        {"sort", required_argument, 0, 1017},
        {"favorites-only", no_argument, 0, 1018},
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
        {"require-tokens", no_argument, 0, 1034},
        {"send-input", required_argument, 0, 1035},
        {"theme", required_argument, 0, 1036},
        {"code-attachment-min-lines", required_argument, 0, 1037},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "hvim:a:l", longopts, NULL)) != -1) {
        switch (c) {
            case 'h': args->show_help = true; break;
            case 'v': args->show_version = true; break;
            case 'i': args->interactive = true; break;
            case 'm': args->model_override = xstrdup(optarg); break;
            case 'a': args_add_attachment(args, optarg); break;
            case 'l': args->loopback = true; break;
            case 1000: args->set_api_key = true; break;
            case 1001: args->set_api_key_gpg = true; break;
            case 1002: args->forget_api_key = true; break;
            case 1003: args->list_models = true; break;
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
            case 1019: args->yes = true; break;
            case 1020: args->list_sessions_cmd = true; break;
            case 1021:
                args->rename_session_cmd = true;
                args->rename_old = xstrdup(optarg);
                if (optind < argc && argv[optind][0] != '-') {
                    args->rename_new = xstrdup(argv[optind++]);
                }
                break;
            case 1022: args->show_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1023: args->clear_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1024: args->edit_memory_cmd = true; args->memory_path = xstrdup(optarg); break;
            case 1025: args->editor_prompt_cmd = true; if (optarg) args->output_path = xstrdup(optarg); break;
            case 1026: args->credits_cmd = true; break;
            case 1027: args->show_all_models = true; break;
            case 1028: args->search_sessions_cmd = true; break;
            case 1029: args->select_model_cmd = true; break;
            case 1030: args->min_input_tokens = atol(optarg); break;
            case 1031: args->min_output_tokens = atol(optarg); break;
            case 1032: args->hide_preview = true; break;
            case 1033: args->hide_preview = false; break;
            case 1034: args->require_tokens = true; break;
            case 1035: args->config_set_cmd = true; args->config_key = xstrdup("SEND_INPUT"); args->config_value = xstrdup(optarg); break;
            case 1036: args->config_set_cmd = true; args->config_key = xstrdup("THEME"); args->config_value = xstrdup(optarg); break;
            case 1037: args->config_set_cmd = true; args->config_key = xstrdup("CODE_ATTACHMENT_MIN_LINES"); args->config_value = xstrdup(optarg); break;
            case 1038: args->search = xstrdup(optarg); args->search_sessions_cmd = true; break;
            default: return 1;
        }
    }
    if (optind < argc) args->input_path = xstrdup(argv[optind++]);
    if (optind < argc) args->output_path = xstrdup(argv[optind++]);
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

static void maybe_write_startup_notice(const TCConfig *cfg, const TCArgs *args, const char *output_path) {
    /*
     * The GUI clients show the Project Lead note as a temporary transcript item.
     * The C CLI has no chat window, so the closest faithful mapping is a
     * human-readable, non-context notice block at the beginning of a new output
     * file.  It is intentionally not wrapped in USER/AI markers, which means the
     * context loader ignores it.  The output file remains readable to humans
     * without accidentally sending the notice back to the provider.
     */
    (void)args;
    if (!cfg->startup_notice) return;
    if (file_exists(output_path) && file_size(output_path) > 0) return;
    TCBuffer b;
    buffer_init(&b);
    char *now = now_string();
    buffer_appendf(&b, "%s\n", TC_NOTICE_BEGIN);
    buffer_appendf(&b, "time: %s\n", now);
    buffer_append(&b, "speaker: remfan1994\n");
    buffer_append(&b, "I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994\n");
    buffer_append(&b, "This is a ttychatter session file.  Future user inputs and AI responses will be appended below.\n");
    buffer_appendf(&b, "%s\n\n", TC_NOTICE_END);
    append_file_text(output_path, b.data);
    free(now);
    free(b.data);
}

static int test_model_command(const TCConfig *cfg, const char *model, bool demo) {
    TCMessageList ctx = {0};
    TCArgs empty;
    args_init(&empty);
    empty.demo = demo;
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

static char *interactive_default_session_path(const TCConfig *cfg) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char name[128];
    strftime(name, sizeof(name), "session-%Y-%m-%d-%H-%M-%S.log", &tmv);
    return path_join2(cfg->session_dir, name);
}

static void runtime_command_help(void) {
    printf("Runtime colon commands:\n");
    printf("  :help                         show this command list\n");
    printf("  :models                       list cached models\n");
    printf("  :routers                      list router models\n");
    printf("  :update-models                refresh model cache from OpenRouter\n");
    printf("  :test-model MODEL             test one model\n");
    printf("  :model MODEL                  use model for this run\n");
    printf("  :model-save MODEL             use model and save MODEL config\n");
    printf("  :favorites                    list favorite models\n");
    printf("  :favorite MODEL               add favorite model\n");
    printf("  :unfavorite MODEL             remove favorite model\n");
    printf("  :config                       print config summary\n");
    printf("  :set KEY VALUE                set config key\n");
    printf("  :unset KEY                    remove config key\n");
    printf("  :set-api-key [gpg]            store API key, optionally encrypted\n");
    printf("  :forget-api-key               remove stored API-key material\n");
    printf("  :memory                       show current context buffer\n");
    printf("  :clear-memory                 clear current context for future sends\n");
    printf("  :attach FILE                  queue attachment for next message\n");
    printf("  :attachments                  list pending attachments\n");
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

static int list_sessions_command(const TCConfig *cfg) {
    DIR *d = opendir(cfg->session_dir);
    if (!d) {
        fprintf(stderr, "could not open session dir %s: %s\n", cfg->session_dir, strerror(errno));
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        printf("%s\n", de->d_name);
    }
    closedir(d);
    return 0;
}

static int rename_session_command(const TCConfig *cfg, const char *old_name, const char *new_name) {
    if (!old_name || !new_name || !*old_name || !*new_name) {
        fprintf(stderr, "usage: --rename-session OLD NEW\n");
        return 2;
    }
    char *oldp = session_path_from_arg(cfg, old_name);
    char *newp = session_path_from_arg(cfg, new_name);
    if (!file_exists(oldp)) {
        fprintf(stderr, "session not found: %s\n", oldp);
        free(oldp); free(newp);
        return 1;
    }
    if (file_exists(newp)) {
        fprintf(stderr, "target already exists: %s\n", newp);
        free(oldp); free(newp);
        return 1;
    }
    if (rename(oldp, newp) != 0) {
        fprintf(stderr, "rename failed: %s\n", strerror(errno));
        free(oldp); free(newp);
        return 1;
    }
    fprintf(stderr, "renamed session to %s\n", newp);
    free(oldp); free(newp);
    return 0;
}

static void print_memory_for_file(const char *path, long context_turns) {
    TCMessageList ctx = load_context_from_session(path, context_turns);
    print_context_buffer(&ctx);
    msglist_free(&ctx);
}

static int clear_memory_command(const char *path) {
    if (!path || !*path) return 2;
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "%s\n", TC_MEMORY_CLEAR);
    append_file_text(path, b.data);
    free(b.data);
    fprintf(stderr, "memory clear marker appended to %s\n", path);
    return 0;
}

static void write_context_snapshot_file(const char *path, const char *context_text) {
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "%s\n", TC_CONTEXT_BEGIN);
    buffer_append(&b, context_text ? context_text : "");
    if (b.len && b.data[b.len - 1] != '\n') buffer_append(&b, "\n");
    buffer_appendf(&b, "%s\n", TC_CONTEXT_END);
    append_file_text(path, b.data);
    free(b.data);
}

static int edit_memory_command(const TCConfig *cfg, const char *path) {
    (void)cfg;
    if (!path || !*path) return 2;
    TCMessageList ctx = load_context_from_session(path, 9999);
    TCBuffer initial;
    buffer_init(&initial);
    for (size_t i = 0; i < ctx.count; i++) buffer_appendf(&initial, "%s: %s\n", ctx.items[i].role, ctx.items[i].content);
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
    clear_memory_command(path);
    write_context_snapshot_file(path, edited);
    free(edited);
    fprintf(stderr, "memory snapshot appended to %s\n", path);
    return 0;
}

static int editor_prompt_command(const TCConfig *cfg, const char *output_path, TCArgs *base_args) {
    char *msg = compose_with_external_editor();
    if (!msg || !*msg) { free(msg); fprintf(stderr, "editor returned empty prompt\n"); return 1; }
    const char *session = output_path && *output_path ? output_path : "ttychatter-editor-session.log";
    TCMessageList ctx = load_context_from_session(session, cfg->context_turns);
    char *raw = openrouter_chat(cfg, base_args->model_override ? base_args->model_override : cfg->model, msg, &ctx, base_args);
    char *base = basename_no_ext(session);
    char *cleaned = extract_code_blocks(cfg, base, raw);
    append_turn(session, base_args->model_override ? base_args->model_override : cfg->model, "editor", base_args, msg, cleaned);
    printf("%s\n", cleaned);
    free(msg); free(raw); free(base); free(cleaned); msglist_free(&ctx);
    return 0;
}

static void credits_command(void) {
    printf("ttychatter - terminal chat clients for AI conversation.\n");
    printf("Project Lead notice: I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994\n");
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
                                    TCMessageList *ctx, TCArgs *pending_args) {
    if (!pending_args->demo && !cfg->api_key) {
        fprintf(stderr, "API key missing; use :set-api-key, set OPENROUTER_API_KEY, or run --set-api-key\n");
        return 1;
    }
    char *raw = openrouter_chat(cfg, model, text, ctx, pending_args);
    char *session_base = basename_no_ext(output_path);
    char *cleaned = extract_code_blocks(cfg, session_base, raw);
    append_turn(output_path, model, "interactive", pending_args, text, cleaned);
    interactive_context_add(ctx, "user", text, cfg->context_turns);
    interactive_context_add(ctx, "assistant", cleaned, cfg->context_turns);
    printf("%s\n", cleaned);
    free(raw);
    free(cleaned);
    free(session_base);
    free_pending_attachments(pending_args);
    return 0;
}

static int interactive_handle_command(TCConfig *cfg, TCArgs *pending_args, TCMessageList *ctx,
                                      const char *output_path, char **active_model, char *line) {
    char *cmdline = trim_in_place(line + 1);
    char *cmd = strtok(cmdline, " \t");
    char *rest = strtok(NULL, "");
    rest = rest ? trim_in_place(rest) : NULL;
    if (!cmd || strcmp(cmd, "help") == 0) { runtime_command_help(); return 0; }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) return 1;
    if (strcmp(cmd, "models") == 0) { list_models(cfg, NULL, pending_args->model_type, pending_args->model_sort, false, pending_args->demo); return 0; }
    if (strcmp(cmd, "routers") == 0) { list_models(cfg, NULL, "routers", pending_args->model_sort, false, pending_args->demo); return 0; }
    if (strcmp(cmd, "update-models") == 0) { update_models(cfg, pending_args->demo); return 0; }
    if (strcmp(cmd, "favorites") == 0) { list_favorites(cfg); return 0; }
    if (strcmp(cmd, "favorite") == 0) { if (rest) add_favorite(cfg, rest); else fprintf(stderr, "usage: :favorite MODEL\n"); return 0; }
    if (strcmp(cmd, "unfavorite") == 0) { if (rest) remove_favorite(cfg, rest); else fprintf(stderr, "usage: :unfavorite MODEL\n"); return 0; }
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
    if (strcmp(cmd, "clear-memory") == 0) { msglist_free(ctx); ctx->items = NULL; ctx->count = 0; ctx->cap = 0; clear_memory_command(output_path); return 0; }
    if (strcmp(cmd, "attach") == 0) { if (rest) { args_add_attachment(pending_args, rest); fprintf(stderr, "queued attachment: %s\n", rest); } else fprintf(stderr, "usage: :attach FILE\n"); return 0; }
    if (strcmp(cmd, "attachments") == 0) { if (pending_args->attachment_count == 0) printf("[no pending attachments]\n"); for (size_t i = 0; i < pending_args->attachment_count; i++) printf("%zu: %s\n", i + 1, pending_args->attachments[i]); return 0; }
    if (strcmp(cmd, "clear-attachments") == 0) { free_pending_attachments(pending_args); fprintf(stderr, "cleared pending attachments\n"); return 0; }
    if (strcmp(cmd, "editor") == 0) { char *msg = compose_with_external_editor(); if (msg) { interactive_send_message(cfg, output_path, *active_model, msg, ctx, pending_args); free(msg); } return 0; }
    if (strcmp(cmd, "search") == 0) { if (rest) search_file_lines(output_path, rest); else fprintf(stderr, "usage: :search TEXT\n"); return 0; }
    if (strcmp(cmd, "search-all") == 0) { if (rest) search_all_sessions(cfg, rest); else fprintf(stderr, "usage: :search-all TEXT\n"); return 0; }
    if (strcmp(cmd, "credits") == 0) { printf("ttychatter - terminal chat clients. Project Lead notice: I strongly encourage everyone to get cruetly-free VEGETARIAN food and remember the 'bloodguilt' curse from the Bible.  -Project Lead, remfan1994\n"); return 0; }
    if (strcmp(cmd, "doctor") == 0) { doctor(cfg); return 0; }
    fprintf(stderr, "unknown command: :%s (try :help)\n", cmd);
    return 0;
}

static int interactive_loop(TCConfig *cfg, TCArgs *base_args, const char *maybe_output_path) {
    char *output_path = maybe_output_path ? xstrdup(maybe_output_path) : interactive_default_session_path(cfg);
    mkdir_p(cfg->session_dir);
    mkdir_p(cfg->attachment_dir);
    maybe_write_startup_notice(cfg, base_args, output_path);
    TCMessageList ctx = load_context_from_session(output_path, cfg->context_turns);
    TCArgs pending;
    args_init(&pending);
    pending.demo = base_args->demo;
    pending.loopback = base_args->loopback;
    free(pending.model_type);
    pending.model_type = xstrdup(base_args->model_type ? base_args->model_type : "all");
    pending.model_sort = base_args->model_sort ? xstrdup(base_args->model_sort) : NULL;
    char *active_model = xstrdup(base_args->model_override ? base_args->model_override : cfg->model);

    printf("ttychatter interactive session: %s\n", output_path);
    printf("Type :help for local commands.  Lines beginning with ':' are not sent to the AI.\n");
    char line[65536];
    while (true) {
        printf("ttychatter> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *s = trim_in_place(line);
        if (!*s) continue;
        if (s[0] == ':' && s[1] != '\0') {
            int done = interactive_handle_command(cfg, &pending, &ctx, output_path, &active_model, s);
            if (done) break;
            continue;
        }
        if (s[0] == '\\' && s[1] == ':') s++;
        interactive_send_message(cfg, output_path, active_model, s, &ctx, &pending);
    }
    free_pending_attachments(&pending);
    free(pending.model_type);
    free(pending.model_sort);
    free(active_model);
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
    if (args.show_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); print_memory_for_file(p, cfg.context_turns); free(p); return 0; }
    if (args.clear_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); int rc = clear_memory_command(p); free(p); return rc; }
    if (args.edit_memory_cmd) { char *p = session_path_from_arg(&cfg, args.memory_path); int rc = edit_memory_command(&cfg, p); free(p); return rc; }
    if (args.search_sessions_cmd || (args.search && !args.list_models)) { search_all_sessions(&cfg, args.search ? args.search : ""); return 0; }
    if (args.editor_prompt_cmd) { if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key"); return editor_prompt_command(&cfg, args.output_path, &args); }
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
        if (args.hide_preview) cfg.model_filter_hide_preview = true;
        if (args.min_input_tokens > 0) cfg.model_min_input_tokens = args.min_input_tokens;
        if (args.min_output_tokens > 0) cfg.model_min_output_tokens = args.min_output_tokens;
        return list_models(&cfg, args.search, args.model_type, args.model_sort, args.favorites_only, args.demo);
    }
    if (args.test_model) {
        if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");
        return test_model_command(&cfg, args.test_model_id, args.demo);
    }

    if (args.interactive) {
        if (args.output_path) {
            fprintf(stderr, "interactive mode accepts at most one session/output path\n");
            return 2;
        }
        return interactive_loop(&cfg, &args, args.input_path);
    }

    if (!args.input_path || !args.output_path) {
        print_help();
        return 2;
    }
    if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");

    if (!confirm_live_send_if_needed(&cfg, &args)) return 1;
    maybe_write_startup_notice(&cfg, &args, args.output_path);

    size_t input_len = 0;
    char *input_text = read_file(args.input_path, &input_len);
    TCMessageList context = load_context_from_session(args.output_path, cfg.context_turns);
    const char *model = args.model_override ? args.model_override : cfg.model;

    char *raw = openrouter_chat(&cfg, model, input_text, &context, &args);
    char *session_base = basename_no_ext(args.output_path);
    char *cleaned = extract_code_blocks(&cfg, session_base, raw);

    append_turn(args.output_path, model, args.input_path, &args, input_text, cleaned);
    fprintf(stderr, "appended response to %s\n", args.output_path);
    if (args.loopback) printf("%s\n", cleaned);
    if (args.loopback_file) write_file_mode(args.loopback_file, cleaned, 0600);

    free(input_text);
    free(raw);
    free(cleaned);
    free(session_base);
    msglist_free(&context);
    curl_global_cleanup();
    return 0;
}
