#include "ch341_loader.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* --- Meshstick library loader ---------------------------------------------- */

static LorahamCh341Api g_api;
static bool g_api_loaded = false;

#define CH341_LIBRARY_NAME "libloraham-ch341.so"

template <typename T>
static bool resolve(void *handle, const char *name, T *out,
                    const char *path, char *why, size_t why_size)
{
    void *sym = dlsym(handle, name);

    if (!sym) {
        snprintf(why, why_size, "missing symbol %s in %s", name, path);
        return false;
    }
    *out = reinterpret_cast<T>(sym);
    return true;
}

bool daemon_ch341_load_from(const char *path, const char *expected_pin,
                            LorahamCh341Api *api, char *why, size_t why_size)
{
    LorahamCh341Api tmp;
    void *handle;

    if (access(path, F_OK) != 0) {
        snprintf(why, why_size,
                 "Meshstick library not found: %s (build the daemon with the "
                 "pine64 source, see docs/hardware.md)", path);
        return false;
    }

    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        snprintf(why, why_size, "cannot load %s: %s", path, err ? err : "unknown error");
        return false;
    }

    /* An accepted library stays loaded for the life of the process. A refused
     * one is unloaded before returning (after the reason is written: the
     * reported commit string lives in the library). */
    if (!resolve(handle, "loraham_ch341_pin", &tmp.pin, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_open", &tmp.open, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_close", &tmp.close, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_transceive", &tmp.transceive, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_pin_mode", &tmp.pin_mode, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_digital_write", &tmp.digital_write, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_digital_read", &tmp.digital_read, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_attach_interrupt", &tmp.attach_interrupt, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_detach_interrupt", &tmp.detach_interrupt, path, why, why_size) ||
        !resolve(handle, "loraham_ch341_in_error", &tmp.in_error, path, why, why_size)) {
        dlclose(handle);
        return false;
    }

    const char *pin = tmp.pin();
    if (!expected_pin || expected_pin[0] == '\0' || !pin || strcmp(pin, expected_pin) != 0) {
        snprintf(why, why_size,
                 "%s was built from pine64 %s, this daemon expects %s",
                 path, pin ? pin : "(none)",
                 (expected_pin && expected_pin[0]) ? expected_pin : "(no lock at build time)");
        dlclose(handle);
        return false;
    }

    *api = tmp;
    return true;
}

bool daemon_ch341_library_path(char *buf, size_t buf_size)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *slash;

    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash)
        return false;
    *slash = '\0';

    return (size_t)snprintf(buf, buf_size, "%s/lib/" CH341_LIBRARY_NAME, exe) < buf_size;
}

bool daemon_ch341_load(const char *expected_pin, char *why, size_t why_size)
{
    char path[PATH_MAX + 32];

    if (!daemon_ch341_library_path(path, sizeof(path))) {
        snprintf(why, why_size, "cannot locate the daemon's own directory");
        return false;
    }
    if (!daemon_ch341_load_from(path, expected_pin, &g_api, why, why_size))
        return false;
    g_api_loaded = true;
    return true;
}

const LorahamCh341Api *daemon_ch341_api(void)
{
    return g_api_loaded ? &g_api : NULL;
}
