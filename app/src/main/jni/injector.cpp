#include "frida-core.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "awesomeCAM"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define AGENT_BASENAME "agent.js"
#define AGENT_PLACEHOLDER "__PAYLOAD_PATH__"

#define info_log(fmt, ...) do { \
    ALOGI(fmt, ##__VA_ARGS__); \
    fprintf(stdout, "[+] " fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

#define err_log(fmt, ...) do { \
    ALOGE(fmt, ##__VA_ARGS__); \
    fprintf(stderr, "[-] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while(0)

typedef struct {
    GMainLoop *loop;
    gboolean completed;
    gboolean success;
    gboolean timeout_fired;
    gchar *failure_message;
} InjectContext;

static int find_pid_by_name(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        err_log("opendir /proc failed: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR) continue;

        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        char path[256];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char cmdline[256] = {0};
        size_t size = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        fclose(f);
        if (size == 0) continue;

        const char *base = strrchr(cmdline, '/');
        const char *process_name = (base != NULL && base[1] != '\0') ? (base + 1) : cmdline;

        if (strcmp(cmdline, name) == 0 || strcmp(process_name, name) == 0) {
            closedir(dir);
            return pid;
        }
    }

    closedir(dir);
    return -1;
}

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        err_log("open %s failed: %s", path, strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        err_log("fseek %s failed: %s", path, strerror(errno));
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        err_log("ftell %s failed: %s", path, strerror(errno));
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        err_log("malloc %ld bytes failed", size + 1);
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        err_log("short read from %s: got %zu expected %ld", path, read_size, size);
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

static char *replace_token(const char *source, const char *token, const char *replacement) {
    const char *match = strstr(source, token);
    if (!match) {
        err_log("agent template missing token %s", token);
        return NULL;
    }

    size_t prefix_len = (size_t)(match - source);
    size_t token_len = strlen(token);
    size_t replacement_len = strlen(replacement);
    size_t suffix_len = strlen(match + token_len);
    size_t total_len = prefix_len + replacement_len + suffix_len;

    char *result = (char *)malloc(total_len + 1);
    if (!result) {
        err_log("malloc %zu bytes failed", total_len + 1);
        return NULL;
    }

    memcpy(result, source, prefix_len);
    memcpy(result + prefix_len, replacement, replacement_len);
    memcpy(result + prefix_len + replacement_len, match + token_len, suffix_len);
    result[total_len] = '\0';
    return result;
}

static char *escape_as_js_string_literal(const char *value) {
    size_t extra = 2;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                extra += 2;
                break;
            default:
                extra += 1;
                break;
        }
    }

    char *out = (char *)malloc(extra + 1);
    if (!out) {
        err_log("malloc %zu bytes failed", extra + 1);
        return NULL;
    }

    char *dst = out;
    *dst++ = '"';
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '\\':
                *dst++ = '\\';
                *dst++ = '\\';
                break;
            case '"':
                *dst++ = '\\';
                *dst++ = '"';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                *dst++ = (char)*cursor;
                break;
        }
    }
    *dst++ = '"';
    *dst = '\0';
    return out;
}

static char *make_agent_path_for_payload(const char *payload_path) {
    const char *last_slash = strrchr(payload_path, '/');
    if (!last_slash) {
        return strdup(AGENT_BASENAME);
    }

    size_t dir_len = (size_t)(last_slash - payload_path);
    size_t total = dir_len + 1 + strlen(AGENT_BASENAME);
    char *path = (char *)malloc(total + 1);
    if (!path) {
        err_log("malloc %zu bytes failed", total + 1);
        return NULL;
    }
    memcpy(path, payload_path, dir_len);
    path[dir_len] = '/';
    strcpy(path + dir_len + 1, AGENT_BASENAME);
    return path;
}

static char *build_agent_source(const char *agent_path, const char *payload_path) {
    char *template_source = read_text_file(agent_path);
    if (!template_source) {
        return NULL;
    }

    char *escaped_payload = escape_as_js_string_literal(payload_path);
    if (!escaped_payload) {
        free(template_source);
        return NULL;
    }

    char *result = replace_token(template_source, AGENT_PLACEHOLDER, escaped_payload);
    free(escaped_payload);
    free(template_source);
    return result;
}

static void complete_result(InjectContext *ctx, gboolean success, const char *fmt, ...) {
    if (ctx->completed) {
        return;
    }

    ctx->completed = TRUE;
    ctx->success = success;

    if (!success && fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        ctx->failure_message = g_strdup_vprintf(fmt, ap);
        va_end(ap);
    }

    if (ctx->loop != NULL && g_main_loop_is_running(ctx->loop)) {
        g_main_loop_quit(ctx->loop);
    }
}

static void on_message(FridaScript *script,
                       const gchar *message,
                       GBytes *data,
                       gpointer user_data) {
    (void)script;
    (void)data;

    InjectContext *ctx = (InjectContext *)user_data;
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, message, -1, &error)) {
        complete_result(ctx, FALSE, "Failed to parse script message: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    const gchar *type = json_object_get_string_member_with_default(root, "type", "");
    if (strcmp(type, "send") == 0) {
        JsonObject *payload = json_object_get_object_member(root, "payload");
        if (payload == NULL) {
            info_log("script send without object payload: %s", message);
            g_object_unref(parser);
            return;
        }

        const gchar *payload_type = json_object_get_string_member_with_default(payload, "type", "");
        if (strcmp(payload_type, "loaded") == 0) {
            const gchar *path = json_object_get_string_member_with_default(payload, "path", "(unknown)");
            const gchar *base = json_object_get_string_member_with_default(payload, "base", "(unknown)");
            info_log("Payload loaded path=%s base=%s", path, base);
            complete_result(ctx, TRUE, NULL);
        } else if (strcmp(payload_type, "error") == 0) {
            const gchar *msg = json_object_get_string_member_with_default(payload, "message", "unknown error");
            const gchar *stack = json_object_get_string_member_with_default(payload, "stack", "");
            complete_result(ctx, FALSE, "Payload load failed: %s%s%s",
                            msg,
                            stack[0] != '\0' ? " | " : "",
                            stack);
        } else {
            info_log("script send: %s", message);
        }
    } else if (strcmp(type, "log") == 0) {
        info_log("script log: %s",
                 json_object_get_string_member_with_default(root, "payload", ""));
    } else if (strcmp(type, "error") == 0) {
        const gchar *description = json_object_get_string_member_with_default(root, "description", "script error");
        const gchar *stack = json_object_get_string_member_with_default(root, "stack", "");
        complete_result(ctx, FALSE, "Script error: %s%s%s",
                        description,
                        stack[0] != '\0' ? " | " : "",
                        stack);
    } else {
        info_log("script message: %s", message);
    }

    g_object_unref(parser);
}

static void on_detached(FridaSession *session,
                        FridaSessionDetachReason reason,
                        FridaCrash *crash,
                        gpointer user_data) {
    (void)session;
    (void)crash;

    InjectContext *ctx = (InjectContext *)user_data;
    gchar *reason_str = g_enum_to_string(FRIDA_TYPE_SESSION_DETACH_REASON, reason);
    info_log("Session detached reason=%s", reason_str != NULL ? reason_str : "(unknown)");
    if (!ctx->completed) {
        complete_result(ctx, FALSE, "Session detached before payload load");
    }
    g_free(reason_str);
}

static gboolean on_timeout(gpointer user_data) {
    InjectContext *ctx = (InjectContext *)user_data;
    ctx->timeout_fired = TRUE;
    complete_result(ctx, FALSE, "Timed out waiting for payload load result");
    return G_SOURCE_REMOVE;
}

static const char *device_type_to_string(FridaDeviceType type) {
    switch (type) {
        case FRIDA_DEVICE_TYPE_LOCAL:
            return "local";
        case FRIDA_DEVICE_TYPE_REMOTE:
            return "remote";
        case FRIDA_DEVICE_TYPE_USB:
            return "usb";
        default:
            return "unknown";
    }
}

static void find_candidate_devices(FridaDeviceManager *manager,
                                   FridaDevice **out_local_device,
                                   FridaDevice **out_remote_device) {
    *out_local_device = NULL;
    *out_remote_device = NULL;

    GError *error = NULL;
    FridaDeviceList *devices = frida_device_manager_enumerate_devices_sync(manager, NULL, &error);
    if (error != NULL) {
        err_log("enumerate devices failed: %s", error->message);
        g_error_free(error);
        return;
    }

    gint num_devices = frida_device_list_size(devices);
    for (gint i = 0; i != num_devices; i++) {
        FridaDevice *device = frida_device_list_get(devices, i);
        FridaDeviceType type = frida_device_get_dtype(device);
        info_log("Found device: name=%s id=%s type=%s",
                 frida_device_get_name(device),
                 frida_device_get_id(device),
                 device_type_to_string(type));
        if (type == FRIDA_DEVICE_TYPE_REMOTE && *out_remote_device == NULL) {
            *out_remote_device = (FridaDevice *)g_object_ref(device);
        } else if (type == FRIDA_DEVICE_TYPE_LOCAL && *out_local_device == NULL) {
            *out_local_device = (FridaDevice *)g_object_ref(device);
        }
        g_object_unref(device);
    }

    frida_unref(devices);
}

static FridaSession *attach_with_device(FridaDevice *device,
                                        guint pid,
                                        const char *label,
                                        gchar **out_error_message) {
    GError *error = NULL;
    FridaSession *session = frida_device_attach_sync(device, pid, NULL, NULL, &error);
    if (error == NULL) {
        return session;
    }

    if (out_error_message != NULL) {
        if (*out_error_message == NULL) {
            *out_error_message = g_strdup_printf("%s: %s", label, error->message);
        } else {
            gchar *combined = g_strdup_printf("%s | %s: %s", *out_error_message, label, error->message);
            g_free(*out_error_message);
            *out_error_message = combined;
        }
    }
    err_log("%s attach failed: %s", label, error->message);
    g_error_free(error);
    return NULL;
}

int do_inject(const char *target_name, const char *loader_path) {
    int exit_code = 1;
    pid_t pid = -1;
    char *agent_path = NULL;
    char *script_source = NULL;
    FridaDeviceManager *manager = NULL;
    FridaDevice *local_device = NULL;
    FridaDevice *remote_device = NULL;
    FridaSession *session = NULL;
    FridaScript *script = NULL;
    FridaScriptOptions *options = NULL;
    GError *error = NULL;
    guint timeout_source = 0;
    gboolean frida_initialized = FALSE;
    gchar *attach_error = NULL;
    InjectContext ctx = {};

    info_log("Start Frida inject [%s -> %s]", target_name, loader_path);

    pid = find_pid_by_name(target_name);
    if (pid <= 0) {
        err_log("target process %s not found", target_name);
        goto cleanup;
    }
    info_log("Resolved %s pid=%d", target_name, pid);

    agent_path = make_agent_path_for_payload(loader_path);
    if (agent_path == NULL) {
        goto cleanup;
    }
    info_log("Agent path = %s", agent_path);

    script_source = build_agent_source(agent_path, loader_path);
    if (script_source == NULL) {
        goto cleanup;
    }

    frida_init();
    frida_initialized = TRUE;
    ctx.loop = g_main_loop_new(NULL, FALSE);

    manager = frida_device_manager_new();
    find_candidate_devices(manager, &local_device, &remote_device);
    if (local_device == NULL && remote_device == NULL) {
        err_log("usable Frida device not found");
        goto cleanup;
    }

    if (local_device != NULL) {
        info_log("Attaching with Frida local backend");
        session = attach_with_device(local_device, (guint)pid, "local backend", &attach_error);
    }
    if (session == NULL && remote_device != NULL) {
        info_log("Attaching with Frida remote backend");
        session = attach_with_device(remote_device, (guint)pid, "remote backend", &attach_error);
    }
    if (session == NULL) {
        if (attach_error != NULL) {
            err_log("attach failed: %s", attach_error);
        }
        goto cleanup;
    }

    g_signal_connect(session, "detached", G_CALLBACK(on_detached), &ctx);
    if (frida_session_is_detached(session)) {
        err_log("session detached immediately");
        goto cleanup;
    }
    info_log("Attached");

    options = frida_script_options_new();
    frida_script_options_set_name(options, "vcam-loader");
    frida_script_options_set_runtime(options, FRIDA_SCRIPT_RUNTIME_QJS);

    script = frida_session_create_script_sync(session, script_source, options, NULL, &error);
    if (error != NULL) {
        err_log("create_script failed: %s", error->message);
        g_error_free(error);
        error = NULL;
        goto cleanup;
    }
    info_log("Script created");

    g_signal_connect(script, "message", G_CALLBACK(on_message), &ctx);

    frida_script_load_sync(script, NULL, &error);
    if (error != NULL) {
        err_log("script load failed: %s", error->message);
        g_error_free(error);
        error = NULL;
        goto cleanup;
    }
    info_log("Script loaded");

    if (!ctx.completed) {
        timeout_source = g_timeout_add_seconds(10, on_timeout, &ctx);
        g_main_loop_run(ctx.loop);
    }

    if (!ctx.completed) {
        complete_result(&ctx, FALSE, "Injection ended without result");
    }

    if (!ctx.success) {
        err_log("%s", ctx.failure_message != NULL ? ctx.failure_message : "Injection failed");
        goto cleanup;
    }

    info_log("Frida payload load confirmed");
    exit_code = 0;

cleanup:
    if (timeout_source != 0 && !ctx.timeout_fired) {
        g_source_remove(timeout_source);
    }

    if (script != NULL) {
        frida_script_unload_sync(script, NULL, NULL);
        frida_unref(script);
    }

    if (session != NULL) {
        if (!frida_session_is_detached(session)) {
            frida_session_detach_sync(session, NULL, NULL);
        }
        frida_unref(session);
    }

    if (local_device != NULL) {
        frida_unref(local_device);
    }

    if (remote_device != NULL) {
        frida_unref(remote_device);
    }

    if (manager != NULL) {
        frida_device_manager_close_sync(manager, NULL, NULL);
        frida_unref(manager);
    }

    if (ctx.loop != NULL) {
        g_main_loop_unref(ctx.loop);
    }

    if (options != NULL) {
        g_object_unref(options);
    }

    g_free(ctx.failure_message);
    g_free(attach_error);
    free(script_source);
    free(agent_path);

    if (frida_initialized) {
        frida_shutdown();
        frida_deinit();
    }

    if (exit_code == 0) {
        info_log("Finish Inject");
    }

    return exit_code;
}
