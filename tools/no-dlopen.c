/* no-dlopen: LD_PRELOAD helper (bionic). Makes dlopen() fail for library paths containing the
 * substring in $NO_DLOPEN_MATCH (comma-separated list allowed). Used to keep Chromium's Dawn from
 * loading its bundled libvk_swiftshader.so, which crashes on this device, so the only "SwiftShader"
 * adapter it can find is the shim-disguised Turnip GPU. Set NO_DLOPEN_DEBUG=1 to log denials. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *__no_dlopen_mark(void);
static int matches(const char *name) {
    const char *list = getenv("NO_DLOPEN_MATCH"); if (!list || !*list || !name) return 0;
    char buf[512]; strncpy(buf, list, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) if (*tok && strstr(name, tok)) return 1;
    return 0;
}
void *dlopen(const char *name, int flags) {
    static void *(*real)(const char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "dlopen");
    if (matches(name)) { if (getenv("NO_DLOPEN_DEBUG")) fprintf(stderr, "[no-dlopen] denied %s\n", name); return __no_dlopen_mark(); }
    return real(name, flags);
}
/* dlerror() must report the denial: callers (Dawn's DynamicLib) build a std::string from it, and a
 * NULL there crashes. */
static __thread int g_denied_pending;
static const char *g_deny_msg = "denied by no-dlopen (NO_DLOPEN_MATCH)";
void *__no_dlopen_mark(void) { g_denied_pending = 1; return NULL; }
char *dlerror(void) {
    static char *(*real)(void);
    if (!real) real = dlsym(RTLD_NEXT, "dlerror");
    if (g_denied_pending) { g_denied_pending = 0; return (char *)g_deny_msg; }
    return real();
}
