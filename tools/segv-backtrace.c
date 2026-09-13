/* segv-backtrace: LD_PRELOAD helper for bionic processes. On SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGFPE it
 * prints a backtrace (module+offset, symbol when exported) to stderr, then chains to whatever handler
 * the program installed (crashpad etc.) or re-raises. Interposes sigaction() so later handlers can't
 * push ours out of the way. Used to find where the Chromium GPU process dies. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <errno.h>
#define NSIG_ 65
static struct sigaction g_chain[NSIG_]; static int g_watched[NSIG_];
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static void w(const char *s) { write(2, s, strlen(s)); }
static volatile int g_in_handler;
static void frame(int i, void *pc) {
    char line[512]; Dl_info di; const char *mod = "?", *sym = "?"; uintptr_t off = 0, soff = 0;
    if (dladdr(pc, &di)) { mod = di.dli_fname ? di.dli_fname : "?"; off = (uintptr_t)pc - (uintptr_t)di.dli_fbase; if (di.dli_sname) { sym = di.dli_sname; soff = (uintptr_t)pc - (uintptr_t)di.dli_saddr; } }
    const char *base = strrchr(mod, '/'); base = base ? base + 1 : mod;
    snprintf(line, sizeof line, "  #%02d %p  %s+0x%lx  %s+0x%lx\n", i, pc, base, (unsigned long)off, sym, (unsigned long)soff); w(line);
}
static int readable(const void *p) { /* probe a pointer without faulting: use write() to /dev/null */
    static int devnull = -1; if (devnull < 0) devnull = open("/dev/null", 1);
    return write(devnull, p, 8) == 8;
}
static void handler(int sig, siginfo_t *si, void *ucv) {
    if (g_in_handler++) { struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL; real_sigaction(sig, &d, NULL); raise(sig); return; }
    char line[256]; char comm[32] = "?"; int fd = open("/proc/self/task/0/comm", 0);
    { char path[64]; snprintf(path, sizeof path, "/proc/self/task/%d/comm", gettid()); fd = open(path, 0); if (fd >= 0) { int n = read(fd, comm, 31); if (n > 0) { comm[n] = 0; if (comm[n-1]=='\n') comm[n-1]=0; } close(fd); } }
    snprintf(line, sizeof line, "\n[segv-backtrace] pid %d tid %d (%s) got signal %d (addr %p)\n", getpid(), gettid(), comm, sig, si ? si->si_addr : NULL); w(line);
    ucontext_t *uc = (ucontext_t *)ucv;
    void *pc = (void *)(uc->uc_mcontext.pc & 0x7FFFFFFFFFULL), *lr = (void *)(uc->uc_mcontext.regs[30] & 0x7FFFFFFFFFULL); uintptr_t fp = uc->uc_mcontext.regs[29];
    frame(0, pc); frame(1, lr);
    for (int i = 2; i < 40 && fp && (fp & 7) == 0 && readable((void *)fp) && readable((void *)(fp + 8)); i++) {
        uintptr_t next = *(uintptr_t *)fp; void *ret = (void *)(*(uintptr_t *)(fp + 8) & 0x7FFFFFFFFFULL);
        if (!ret) break; frame(i, ret);
        if (next <= fp) break; fp = next;
    }
    w("[segv-backtrace] end\n"); g_in_handler = 0;
    struct sigaction *c = &g_chain[sig];
    if (c->sa_flags & SA_SIGINFO) { if (c->sa_sigaction) { c->sa_sigaction(sig, si, ucv); return; } }
    else if (c->sa_handler && c->sa_handler != SIG_DFL && c->sa_handler != SIG_IGN) { c->sa_handler(sig); return; }
    struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL; real_sigaction(sig, &d, NULL); raise(sig);
}
static void install(int sig) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    real_sigaction(sig, &sa, NULL); g_watched[sig] = 1;
}
/* interposed: keep our handler first, remember theirs to chain */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if (sig > 0 && sig < NSIG_ && g_watched[sig]) {
        if (oldact) *oldact = g_chain[sig];
        if (act) g_chain[sig] = *act;
        return 0;
    }
    return real_sigaction(sig, act, oldact);
}
__attribute__((constructor)) static void init(void) {
    if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) install(sigs[i]);
    char line[96]; snprintf(line, sizeof line, "[segv-backtrace] armed in pid %d\n", getpid()); w(line);
}
