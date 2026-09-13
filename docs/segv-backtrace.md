# segv-backtrace: crash backtraces for bionic GPU processes

`tools/segv-backtrace.c` is an `LD_PRELOAD` helper that was used to find exactly where Chromium's
GPU process was dying on this device (the user's original "the WebGPU piece freezes" bug, later
identified as a crash inside Chromium's bundled SwiftShader). It is not part of the shim ICD
itself — it's a separate diagnostic tool that happens to live in this repo because it was built
for the same investigation.

## What it does

It's a small shared library, built for **bionic** (Termux's libc), that:

1. On load (a `__attribute__((constructor))` function), installs its own `sigaction` handler for
   `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGABRT`, and `SIGFPE`.
2. **Interposes `sigaction()`** itself. If the watched process later calls `sigaction()` to install
   its own handler for one of those signals (Chromium installs Crashpad handlers for exactly
   these), the interposed call doesn't actually replace the real handler — it just records the
   requested handler in a table and returns success. This keeps segv-backtrace's handler installed
   and first in line no matter what the target process does afterwards.
3. When a watched signal actually fires, the handler:
   - Prints pid/tid/thread name (`/proc/self/task/<tid>/comm`), the signal number, and the fault
     address (`siginfo_t.si_addr`).
   - Reads the program counter and link register out of the `ucontext_t` passed to the signal
     handler (AArch64: `uc_mcontext.pc`, `uc_mcontext.regs[30]`), masking off the top pointer-tag
     bits bionic/ARM can set.
   - Walks the frame-pointer chain (`uc_mcontext.regs[29]`) manually, printing each return address,
     since there's no `ptrace`/`gdb`/`strace` available inside proot to attach a real debugger.
   - Resolves each address to a module + offset (and symbol + offset, if exported) with
     `dladdr()`, since there's no external symbolizer step available either.
   - Guards the frame walk with a `write()`-to-`/dev/null` probe (`readable()`) on each candidate
     frame pointer before dereferencing it, since walking a corrupted stack can itself fault.
   - Finally chains to whatever handler the target process originally tried to install (Crashpad,
     etc.), or restores `SIG_DFL` and re-raises, so the process still dies/reports the same way it
     normally would after the backtrace is printed.

Output goes straight to stderr via raw `write()` (async-signal-safe; no `printf`/stdio buffering
inside a signal handler).

## Why it's bionic-only, and why the wrapper execs the chrome binary directly

This device's stack mixes two userlands on one `PATH`: Termux's bionic tools and the Ubuntu/proot
glibc tools Claude Code runs in. `segv-backtrace.c` is compiled with Termux's clang against
bionic — it uses bionic-specific paths (`/proc/self/task/<tid>/comm`) and depends on being loaded
into a bionic process's address space (bionic and glibc `sigaction`/`ucontext_t` layouts are not
interchangeable). **Preloading it into a glibc process would not just fail to work, it can crash
or misbehave the glibc program**, since `LD_PRELOAD` forces the library into the process and its
constructor runs unconditionally.

The normal launch path for testing (`chromium-gpu`, outside this repo) is a shell script that
itself runs under glibc-adjacent tooling and does some setup before finally invoking Chromium.
Exporting `LD_PRELOAD` for that *whole script* would also inject the bionic `.so` into every glibc
helper program the script calls along the way (`sed`, `cat`, etc.), which would immediately break.
So the wrapper only sets `LD_PRELOAD` for the final `exec` of the actual bionic chrome binary,
scoped to that one command:

```sh
if [ -n "${THOR_PRELOAD:-}" ]; then
  CHROME="/usr/bin/env LD_PRELOAD=$THOR_PRELOAD $T/lib/chromium/chrome --extra-plugin-dir=$T/lib/nsbrowser/plugins"
else
  CHROME="$T/bin/chromium-browser"
fi
```

i.e. `env LD_PRELOAD=... chrome ...` is exec'd directly as the target bionic binary, rather than
the preload variable being exported into the wrapper's own shell environment.

## How it was used

Built directly with Termux clang (no build.sh entry for it — it's a standalone diagnostic, not
part of the shim):

```sh
$PREFIX/bin/clang -O2 -shared -fPIC -fvisibility=default tools/segv-backtrace.c -ldl \
  -o $PREFIX/lib/libsegv-backtrace.so
```

Then launched via the wrapper's `THOR_PRELOAD` variable pointing at that `.so`, with the GPU
process ultimately crashing inside Chromium's bundled SwiftShader
(`libvk_swiftshader.so+0xc03aec`, fault address `0x14`) while creating a WebGPU device — the
signature that identified the crash as being inside SwiftShader's device/adapter creation path
rather than in Turnip, Dawn, or the shim.

## Limitations

- AArch64-only (`ucontext_t.uc_mcontext.pc`/`regs[]` layout is architecture-specific).
- Frame-pointer walking only — no DWARF/CFI unwinding, so a frame built without preserving `x29`
  (aggressive optimization, hand-written asm) will truncate the backtrace early.
- Symbol resolution is limited to what `dladdr()` can see (dynamic/exported symbols only); a
  stripped release binary will show module+offset without a symbol name, which is what happened
  for the crash frame inside `libvk_swiftshader.so` above.
- Diagnostic only — not something to leave preloaded permanently, and never appropriate to preload
  into any glibc/proot process.
