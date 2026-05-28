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
#define TC_VERSION "0.2.0-c-cli"
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
    bool show_config;
    bool list_favorites;
    char *set_key;
    char *set_value;
    char *unset_key;
    char *favorite_model;
    char *unfavorite_model;
    char *search_sessions;
    char *search;
    char *model_type;
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
    cfg->model_favorites_file = xdg_data_dir("model-favorites");
    cfg->session_dir = xdg_data_dir("sessions");
    cfg->attachment_dir = xdg_data_dir("attachments");
    cfg->context_turns = 12;
    cfg->code_attachment_min_lines = 5;
    cfg->max_attachment_bytes = 1024 * 1024;
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ttychatter-c-cli/0.2");
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
/* Model catalog helpers                                                     */
/* ------------------------------------------------------------------------- */

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


/*
 * Demo model catalog.  Demo mode exists because users and maintainers need to
 * exercise the client without spending API calls or touching the network.  The
 * JSON shape intentionally resembles OpenRouter's model list response enough
 * for the normal listing/filtering code to process it.  If the real provider
 * changes its schema, the demo catalog should remain a stable internal test
 * fixture rather than trying to mimic every provider detail.
 */
static const char *demo_models_json(void) {
    return "{\"data\":["
           "{\"id\":\"openrouter/auto\",\"name\":\"OpenRouter Auto Router\",\"context_length\":128000},"
           "{\"id\":\"openrouter/free\",\"name\":\"OpenRouter Free Router\",\"context_length\":8192},"
           "{\"id\":\"demo/large-context\",\"name\":\"Demo Large Context Fixed Model\",\"context_length\":1048576},"
           "{\"id\":\"demo/code-helper:free\",\"name\":\"Demo Free Code Helper\",\"context_length\":32768},"
           "{\"id\":\"demo/tiny\",\"name\":\"Demo Tiny Fixed Model\",\"context_length\":4096}"
           "]}";
}

/*
 * Favorites are deliberately a plain newline-delimited text file.  A JSON array
 * would be easy enough with json-c, but a line file is grep-friendly, editable
 * with vi, and easy to repair by hand.  The file contains model IDs, one per
 * line.  The listing code marks favorites with a leading '*'.
 */
static bool favorite_contains(const TCConfig *cfg, const char *model_id) {
    if (!cfg->model_favorites_file || !model_id || !file_exists(cfg->model_favorites_file)) return false;
    size_t len = 0;
    char *text = read_file(cfg->model_favorites_file, &len);
    bool found = false;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = trim_in_place(line);
        if (strcmp(t, model_id) == 0) { found = true; break; }
    }
    free(text);
    return found;
}

static int favorite_add(const TCConfig *cfg, const char *model_id) {
    if (!model_id || !*model_id) return 1;
    if (favorite_contains(cfg, model_id)) {
        fprintf(stderr, "favorite already present: %s\n", model_id);
        return 0;
    }
    char *dircopy = xstrdup(cfg->model_favorites_file);
    char *slash = strrchr(dircopy, '/');
    if (slash) { *slash = '\0'; mkdir_p(dircopy); }
    free(dircopy);
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "%s\n", model_id);
    append_file_text(cfg->model_favorites_file, b.data);
    free(b.data);
    fprintf(stderr, "added favorite: %s\n", model_id);
    return 0;
}

static int favorite_remove(const TCConfig *cfg, const char *model_id) {
    if (!cfg->model_favorites_file || !file_exists(cfg->model_favorites_file)) return 0;
    size_t len = 0;
    char *text = read_file(cfg->model_favorites_file, &len);
    TCBuffer out;
    buffer_init(&out);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = trim_in_place(line);
        if (*t && strcmp(t, model_id) != 0) buffer_appendf(&out, "%s\n", t);
    }
    write_file_mode(cfg->model_favorites_file, out.data, 0600);
    fprintf(stderr, "removed favorite if present: %s\n", model_id ? model_id : "");
    free(text);
    free(out.data);
    return 0;
}

static int favorite_list(const TCConfig *cfg) {
    if (!cfg->model_favorites_file || !file_exists(cfg->model_favorites_file)) {
        fprintf(stderr, "No favorites file found: %s\n", cfg->model_favorites_file ? cfg->model_favorites_file : "(unset)");
        return 1;
    }
    size_t len = 0;
    char *text = read_file(cfg->model_favorites_file, &len);
    printf("%s", text);
    free(text);
    return 0;
}

static int write_demo_model_cache(const TCConfig *cfg) {
    write_file_mode(cfg->model_cache_file, demo_models_json(), 0600);
    fprintf(stderr, "wrote demo model cache: %s\n", cfg->model_cache_file);
    return 0;
}

static int update_models(const TCConfig *cfg) {
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

static int list_models_from_json(const TCConfig *cfg, const char *json, const char *search, const char *type_filter) {
    json_object *root = json_tokener_parse(json);
    if (!root) die("could not parse model catalog JSON");
    json_object *data = NULL;
    if (!json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_array)) {
        json_object_put(root);
        die("model catalog does not contain data[]");
    }

    /*
     * This is intentionally a terminal-friendly table rather than a pretty
     * curses browser.  The C CLI is meant to be piped through grep, sort, less,
     * awk, or cut.  A leading '*' marks user favorites without requiring color
     * or control sequences, which keeps output safe for logs and scripts.
     */
    printf("F %-42s %-14s %-10s %s\n", "MODEL", "TYPE", "CONTEXT", "NAME");
    printf("- %-42s %-14s %-10s %s\n", "-----", "----", "-------", "----");
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
        if (!model_type_matches(id, type_filter)) continue;
        if (search && *search && !contains_casefold(id, search) && !contains_casefold(name, search)) continue;
        printf("%c %-42.42s %-14s %-10ld %s\n", favorite_contains(cfg, id) ? '*' : ' ', id, model_type_of(id), ctx, name);
    }
    json_object_put(root);
    return 0;
}

static int list_models(const TCConfig *cfg, const char *search, const char *type_filter) {
    if (!file_exists(cfg->model_cache_file)) {
        fprintf(stderr, "No model cache found: %s\n", cfg->model_cache_file);
        fprintf(stderr, "Run: %s --update-models\n", TC_PROGRAM);
        return 1;
    }
    size_t len = 0;
    char *json = read_file(cfg->model_cache_file, &len);
    int rc = list_models_from_json(cfg, json, search, type_filter);
    free(json);
    return rc;
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
    const char *p = text;
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

static char *demo_chat_response(const char *model, const char *user_text, const TCArgs *args) {
    (void)args;
    TCBuffer b;
    buffer_init(&b);
    buffer_appendf(&b, "Demo response from %s. No network request was made.\n\n", model ? model : "demo/auto");
    buffer_append(&b, "The input file was read successfully and the session-output file can be used again to continue this conversation.\n\n");
    buffer_append(&b, "Short inline code block, intentionally below the attachment threshold:\n\n");
    buffer_append(&b, "```sh\necho short demo\n```\n\n");
    buffer_append(&b, "Longer code block, intentionally above the default threshold so attachment extraction can be tested:\n\n");
    buffer_append(&b, "```python\n");
    buffer_append(&b, "def demo():\n");
    buffer_append(&b, "    print('ttychatter demo mode')\n");
    buffer_append(&b, "    print('this block should become an attachment')\n");
    buffer_append(&b, "    print('no provider request was made')\n");
    buffer_append(&b, "\ndemo()\n```\n\n");
    if (user_text && *user_text) {
        buffer_append(&b, "Your prompt began with: ");
        size_t preview = strlen(user_text);
        if (preview > 120) preview = 120;
        buffer_append_n(&b, user_text, preview);
        if (strlen(user_text) > preview) buffer_append(&b, "...");
        buffer_append(&b, "\n");
    }
    return buffer_take(&b);
}

static char *openrouter_chat(const TCConfig *cfg, const char *model, const char *user_text, const TCMessageList *context, const TCArgs *args) {
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
    printf("  ttychatter --set-api-key [--gpg]\n");
    printf("  ttychatter --models [--search TEXT] [--model-type TYPE] [--demo]\n");
    printf("  ttychatter --update-models [--demo]\n");
    printf("  ttychatter --test-model MODEL [--demo]\n");
    printf("  ttychatter --favorites | --favorite-model MODEL | --unfavorite-model MODEL\n");
    printf("  ttychatter --config | --set KEY VALUE | --unset KEY\n");
    printf("  ttychatter --search-sessions TEXT\n");
    printf("\nOptions:\n");
    printf("  -m, --model MODEL        model/router to use (default from config)\n");
    printf("  -a, --attach FILE        attach file; repeatable\n");
    printf("  -l, --loopback           also print AI output to stdout\n");
    printf("      --demo               use local demo models/replies; no network/API key\n");
    printf("      --search TEXT        filter model list\n");
    printf("      --model-type TYPE    all, routers, fixed, free, auto\n");
    printf("      --favorites          list favorite models\n");
    printf("      --favorite-model M   add favorite model\n");
    printf("      --unfavorite-model M remove favorite model\n");
    printf("      --config             print active config summary\n");
    printf("      --set KEY VALUE      write config key\n");
    printf("      --unset KEY          remove config key\n");
    printf("      --search-sessions T  search local session files\n");
    printf("      --set-api-key        save API key\n");
    printf("      --gpg                encrypt API key with gpg when used with --set-api-key\n");
    printf("      --forget-api-key     remove stored plaintext/encrypted API key\n");
    printf("      --doctor             local diagnostics\n");
    printf("  -h, --help               show help\n");
    printf("  -v, --version            show version\n");
}

static void print_version(void) {
    printf("%s %s\n", TC_PROGRAM, TC_VERSION);
}


/*
 * Print the active configuration in a human-readable form.  This is not meant
 * to expose secrets: API keys are intentionally summarized as loaded/missing.
 * The command is useful because ttychatter has multiple XDG paths and users
 * should not have to guess where sessions, attachments, cache files, and GPG
 * key files live.
 */
static int print_config(const TCConfig *cfg) {
    printf("config file:      %s\n", cfg->config_file);
    printf("api key:          %s\n", cfg->api_key ? "loaded" : "missing");
    printf("api key gpg file: %s\n", cfg->api_key_gpg_file);
    printf("model:            %s\n", cfg->model);
    printf("model cache:      %s\n", cfg->model_cache_file);
    printf("model favorites:  %s\n", cfg->model_favorites_file);
    printf("session dir:      %s\n", cfg->session_dir);
    printf("attachment dir:   %s\n", cfg->attachment_dir);
    printf("context turns:    %ld\n", cfg->context_turns);
    printf("code threshold:   %ld lines\n", cfg->code_attachment_min_lines);
    printf("max attachment:   %ld bytes\n", cfg->max_attachment_bytes);
    return 0;
}

/*
 * Session search is intentionally simple: scan the local session directory and
 * print any lines containing the requested substring.  This gives the C client
 * a small but useful piece of the transcript-search behavior from the richer
 * clients without requiring ncurses, an indexer, or a database.
 */
static int search_sessions(const TCConfig *cfg, const char *needle) {
    if (!needle || !*needle) {
        fprintf(stderr, "--search-sessions requires text to search for\n");
        return 2;
    }
    char *qdir = shell_quote(cfg->session_dir);
    char *qneedle = shell_quote(needle);
    TCBuffer cmd;
    buffer_init(&cmd);
    buffer_appendf(&cmd, "grep -RIn -- %s %s 2>/dev/null", qneedle, qdir);
    char *cmds = buffer_take(&cmd);
    int rc = system(cmds);
    free(cmds);
    free(qdir);
    free(qneedle);
    if (rc != 0) return 1;
    return 0;
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
    return cfg->api_key ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Main argument parser and command dispatch                                 */
/* ------------------------------------------------------------------------- */

static void args_init(TCArgs *a) {
    memset(a, 0, sizeof(*a));
    a->model_type = xstrdup("all");
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
        {"demo", no_argument, 0, 1009},
        {"config", no_argument, 0, 1010},
        {"set", required_argument, 0, 1011},
        {"unset", required_argument, 0, 1012},
        {"favorites", no_argument, 0, 1013},
        {"favorite-model", required_argument, 0, 1014},
        {"unfavorite-model", required_argument, 0, 1015},
        {"search-sessions", required_argument, 0, 1016},
        {"set-api-key", no_argument, 0, 1000},
        {"gpg", no_argument, 0, 1001},
        {"forget-api-key", no_argument, 0, 1002},
        {"models", no_argument, 0, 1003},
        {"update-models", no_argument, 0, 1004},
        {"search", required_argument, 0, 1005},
        {"model-type", required_argument, 0, 1006},
        {"test-model", required_argument, 0, 1007},
        {"doctor", no_argument, 0, 1008},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "hvm:a:l", longopts, NULL)) != -1) {
        switch (c) {
            case 'h': args->show_help = true; break;
            case 'v': args->show_version = true; break;
            case 'm': args->model_override = xstrdup(optarg); break;
            case 'a': args_add_attachment(args, optarg); break;
            case 'l': args->loopback = true; break;
            case 1009: args->demo = true; break;
            case 1010: args->show_config = true; break;
            case 1011:
                args->set_key = xstrdup(optarg);
                if (optind < argc) args->set_value = xstrdup(argv[optind++]);
                else { fprintf(stderr, "--set requires KEY VALUE\n"); return 1; }
                break;
            case 1012: args->unset_key = xstrdup(optarg); break;
            case 1013: args->list_favorites = true; break;
            case 1014: args->favorite_model = xstrdup(optarg); break;
            case 1015: args->unfavorite_model = xstrdup(optarg); break;
            case 1016: args->search_sessions = xstrdup(optarg); break;
            case 1000: args->set_api_key = true; break;
            case 1001: args->set_api_key_gpg = true; break;
            case 1002: args->forget_api_key = true; break;
            case 1003: args->list_models = true; break;
            case 1004: args->update_models = true; break;
            case 1005: args->search = xstrdup(optarg); break;
            case 1006: free(args->model_type); args->model_type = xstrdup(optarg); break;
            case 1007: args->test_model = true; args->test_model_id = xstrdup(optarg); break;
            case 1008: args->doctor = true; break;
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

static int test_model_command(const TCConfig *cfg, const char *model, bool demo) {
    TCMessageList ctx = {0};
    TCArgs empty;
    args_init(&empty);
    empty.demo = demo;
    const char *prompt = "Please reply with a short sentence confirming this model is available for text generation.";
    char *reply = demo ? demo_chat_response(model, prompt, &empty) : openrouter_chat(cfg, model, prompt, &ctx, &empty);
    printf("Model: %s\n", model);
    printf("Response:\n%s\n", reply);
    free(reply);
    free(empty.model_type);
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

    if (args.show_help) { print_help(); return 0; }
    if (args.show_version) { print_version(); return 0; }
    if (args.set_api_key) return cmd_set_api_key(&cfg, args.set_api_key_gpg);
    if (args.forget_api_key) return cmd_forget_api_key(&cfg);
    if (args.show_config) return print_config(&cfg);
    if (args.set_key) { config_write_key_value(&cfg, args.set_key, args.set_value ? args.set_value : ""); return 0; }
    if (args.unset_key) { config_remove_key(&cfg, args.unset_key); return 0; }
    if (args.list_favorites) return favorite_list(&cfg);
    if (args.favorite_model) return favorite_add(&cfg, args.favorite_model);
    if (args.unfavorite_model) return favorite_remove(&cfg, args.unfavorite_model);
    if (args.search_sessions) return search_sessions(&cfg, args.search_sessions);
    if (args.doctor) return doctor(&cfg);
    if (args.update_models) return args.demo ? write_demo_model_cache(&cfg) : update_models(&cfg);
    if (args.list_models) return args.demo ? list_models_from_json(&cfg, demo_models_json(), args.search, args.model_type) : list_models(&cfg, args.search, args.model_type);
    if (args.test_model) {
        if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");
        return test_model_command(&cfg, args.test_model_id, args.demo);
    }

    if (!args.input_path || !args.output_path) {
        print_help();
        return 2;
    }
    if (!args.demo && !cfg.api_key) die("API key missing; set OPENROUTER_API_KEY or run --set-api-key");

    size_t input_len = 0;
    char *input_text = read_file(args.input_path, &input_len);
    TCMessageList context = load_context_from_session(args.output_path, cfg.context_turns);
    const char *model = args.model_override ? args.model_override : cfg.model;

    char *raw = args.demo ? demo_chat_response(model, input_text, &args) : openrouter_chat(&cfg, model, input_text, &context, &args);
    char *session_base = basename_no_ext(args.output_path);
    char *cleaned = extract_code_blocks(&cfg, session_base, raw);

    append_turn(args.output_path, model, args.input_path, &args, input_text, cleaned);
    fprintf(stderr, "appended response to %s\n", args.output_path);
    if (args.loopback) printf("%s\n", cleaned);

    free(input_text);
    free(raw);
    free(cleaned);
    free(session_base);
    msglist_free(&context);
    curl_global_cleanup();
    return 0;
}
