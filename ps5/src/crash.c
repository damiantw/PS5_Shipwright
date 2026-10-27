/*
 * Ship of Harkinian for the PlayStation 5 - a crash report a player can send.
 *
 * On SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT the handler writes
 * <data>/logs/crash-<UTC time>.txt and then hands the signal back to the
 * console (its default action: the system's own crash record and the title's
 * end, as without this). The report has:
 *
 *   - the build (/app0/build_id.txt: this repo's commit and eboot.bin's sha256,
 *     param.json's contentVersion) and the executable's load address
 *   - the signal, the fault address and the registers
 *   - a frame-pointer backtrace (the whole title is built -fno-omit-frame-pointer),
 *     each frame as an address, its offset in the ELF and, when an eboot.sym that
 *     matches this build is in /app0 (dev builds) or the data folder, a name;
 *     then the code addresses found on the top of the stack, for frames without
 *     a frame pointer
 *   - the last lines of logs/Ship of Harkinian.log
 *   - the game's own context (scene, room, Link), last, as that state may be
 *     what broke
 *
 * Offsets are what ps5/tools/symbolize.sh (llvm-addr2line on the release's
 * unstripped ELF, a separate release asset) takes. Nothing here uses the
 * network. Everything the handler needs is prepared at install: it only reads
 * memory it has checked is mapped, and writes with open/write/pread.
 *
 * SPDX-License-Identifier: MIT
 */
#include "crash.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>

int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, int *prot);

#define SYM_MAGIC "SOHSYM1\n"

static char g_data[256] = "/data/soh";
static char g_build[192] = "unknown";
static char g_version[32] = "?";
static uintptr_t g_base, g_text_end;
static ps5_crash_context_fn g_context;
static atomic_int g_busy;
static uint8_t g_altstack[64 * 1024] __attribute__((aligned(16)));
static const int g_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

/* The handler's reference point: its address minus its link address (eboot.sym
 * records that) is the load address, and tells a symbol file of another build. */
__attribute__((noinline, used)) void
ps5_crash_anchor(void)
{
   __asm__ volatile("");
}

unsigned long
ps5_image_base(void)
{
   return g_base;
}

void
ps5_crash_set_context(ps5_crash_context_fn fn)
{
   g_context = fn;
}

/* --------------------------------------------------------------- output */

struct out {
   int fd;
   char buf[2048];
   size_t len;
};

static void
out_flush(struct out *o)
{
   size_t done = 0;
   while (done < o->len) {
      ssize_t n = write(o->fd, o->buf + done, o->len - done);
      if (n <= 0)
         break;
      done += (size_t)n;
   }
   o->len = 0;
}

static void
out_raw(struct out *o, const char *s, size_t n)
{
   while (n) {
      size_t room = sizeof o->buf - o->len;
      size_t take = n < room ? n : room;
      memcpy(o->buf + o->len, s, take);
      o->len += take;
      s += take;
      n -= take;
      if (o->len == sizeof o->buf)
         out_flush(o);
   }
}

__attribute__((format(printf, 2, 3))) static void
out_f(struct out *o, const char *format, ...)
{
   char line[512];
   va_list args;
   va_start(args, format);
   int n = vsnprintf(line, sizeof line, format, args);
   va_end(args);
   if (n > 0)
      out_raw(o, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* ------------------------------------------------------- memory checks */

/* [start, end) of the mapping that holds addr, or false */
static int
mapping(uintptr_t addr, uintptr_t *start, uintptr_t *end)
{
   void *s = NULL, *e = NULL;
   int prot = 0;
   if (addr < 0x10000 || sceKernelQueryMemoryProtection((void *)addr, &s, &e, &prot) != 0)
      return 0;
   if (!(prot & 1)) /* not readable */
      return 0;
   *start = (uintptr_t)s;
   *end = (uintptr_t)e;
   return *start <= addr && addr < *end;
}

/* --------------------------------------------------------------- symbols */

struct symfile {
   int fd;
   uint32_t count, strtab;
};

static int
sym_open(struct symfile *s)
{
   static const char *const names[] = { "/app0/eboot.sym", NULL };
   char path[300];
   unsigned char head[24];
   for (int i = 0; i < 2; i++) {
      const char *name = names[i];
      if (!name) {
         snprintf(path, sizeof path, "%s/eboot.sym", g_data);
         name = path;
      }
      s->fd = open(name, O_RDONLY);
      if (s->fd < 0)
         continue;
      if (pread(s->fd, head, sizeof head, 0) == (ssize_t)sizeof head && !memcmp(head, SYM_MAGIC, 8)) {
         uint64_t anchor;
         memcpy(&anchor, head + 8, 8);
         memcpy(&s->count, head + 16, 4);
         memcpy(&s->strtab, head + 20, 4);
         /* another build's symbols would name every frame wrongly */
         if (g_base && anchor == (uintptr_t)ps5_crash_anchor - g_base)
            return 1;
      }
      close(s->fd);
   }
   s->fd = -1;
   return 0;
}

/* The function holding the link-time address off: its name and off's distance into it */
static int
sym_find(const struct symfile *s, uint64_t off, char *name, size_t size, uint64_t *delta)
{
   if (s->fd < 0 || !s->count)
      return 0;
   uint32_t lo = 0, hi = s->count; /* the last entry with addr <= off */
   unsigned char e[16];
   while (hi - lo > 1) {
      uint32_t mid = lo + (hi - lo) / 2;
      uint64_t a;
      if (pread(s->fd, e, 16, 24 + (off_t)mid * 16) != 16)
         return 0;
      memcpy(&a, e, 8);
      if (a <= off)
         lo = mid;
      else
         hi = mid;
   }
   if (pread(s->fd, e, 16, 24 + (off_t)lo * 16) != 16)
      return 0;
   uint64_t addr;
   uint32_t len, name_off;
   memcpy(&addr, e, 8);
   memcpy(&len, e + 8, 4);
   memcpy(&name_off, e + 12, 4);
   if (addr > off || (len && off >= addr + len + 16))
      return 0;
   ssize_t n = pread(s->fd, name, size - 1, (off_t)s->strtab + name_off);
   if (n <= 0)
      return 0;
   name[n] = 0;
   name[strnlen(name, (size_t)n)] = 0;
   *delta = off - addr;
   return 1;
}

static void
frame_line(struct out *o, const struct symfile *s, int index, uintptr_t pc)
{
   char name[200];
   uint64_t delta;
   if (pc >= g_base && pc < g_text_end) {
      /* a return address is past its call: name the call's own byte */
      uint64_t off = pc - g_base;
      if (sym_find(s, index ? off - 1 : off, name, sizeof name, &delta))
         out_f(o, "  #%02d 0x%012lx  eboot+0x%08lx  %s+0x%lx\n", index, (unsigned long)pc, (unsigned long)off,
               name, (unsigned long)(index ? delta + 1 : delta));
      else
         out_f(o, "  #%02d 0x%012lx  eboot+0x%08lx\n", index, (unsigned long)pc, (unsigned long)off);
   } else {
      out_f(o, "  #%02d 0x%012lx  (outside eboot: a system module)\n", index, (unsigned long)pc);
   }
}

/* --------------------------------------------------------------- report */

static void
utc_stamp(char *buf, size_t size, int compact)
{
   time_t t = time(NULL);
   long days = (long)(t / 86400), secs = (long)(t % 86400);
   /* civil from days (Howard Hinnant) */
   long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
   long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
   long d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
   y += m <= 2;
   if (compact)
      snprintf(buf, size, "%04ld%02ld%02ld-%02ld%02ld%02ld", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
   else
      snprintf(buf, size, "%04ld-%02ld-%02ld %02ld:%02ld:%02ld UTC", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
}

static const char *
signal_name(int sig)
{
   switch (sig) {
   case SIGSEGV: return "SIGSEGV";
   case SIGBUS: return "SIGBUS";
   case SIGILL: return "SIGILL";
   case SIGFPE: return "SIGFPE";
   case SIGABRT: return "SIGABRT";
   default: return "?";
   }
}

static void
log_tail(struct out *o)
{
   char path[320];
   snprintf(path, sizeof path, "%s/logs/Ship of Harkinian.log", g_data);
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
      out_f(o, "(no %s)\n", path);
      return;
   }
   static char tail[6144];
   off_t size = lseek(fd, 0, SEEK_END);
   off_t from = size > (off_t)sizeof tail ? size - (off_t)sizeof tail : 0;
   ssize_t n = pread(fd, tail, sizeof tail, from);
   close(fd);
   if (n <= 0)
      return;
   size_t start = 0;
   if (from > 0) { /* from the first whole line */
      while (start < (size_t)n && tail[start] != '\n')
         start++;
      start++;
   }
   if (start < (size_t)n)
      out_raw(o, tail + start, (size_t)n - start);
   if (tail[n - 1] != '\n')
      out_raw(o, "\n", 1);
}

static void
write_report(int sig, siginfo_t *info, ucontext_t *uc)
{
   char stamp[32], when[40], path[320];
   utc_stamp(stamp, sizeof stamp, 1);
   utc_stamp(when, sizeof when, 0);
   snprintf(path, sizeof path, "%s/logs", g_data);
   mkdir(path, 0777);
   snprintf(path, sizeof path, "%s/logs/crash-%s.txt", g_data, stamp);
   static struct out o;
   o.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   o.len = 0;
   if (o.fd < 0) {
      say("crash: %s, and %s could not be written (%d)", signal_name(sig), path, errno);
      return;
   }
   const mcontext_t *m = &uc->uc_mcontext;
   out_f(&o, "Ship of Harkinian (PS5) crash report\n");
   out_f(&o, "time:    %s\n", when);
   out_f(&o, "version: %s\nbuild:   %s\n", g_version, g_build);
   out_f(&o, "base:    0x%lx (eboot+0x... below is an offset into the ELF: ps5/tools/symbolize.sh takes it)\n",
         (unsigned long)g_base);
   out_f(&o, "signal:  %d %s, code %d, fault address 0x%lx\n", sig, signal_name(sig), info ? info->si_code : 0,
         info ? (unsigned long)info->si_addr : 0ul);
   out_f(&o, "trap:    %ld, error %ld, thread %p\n\n", (long)m->mc_trapno, (long)m->mc_err, (void *)pthread_self());
   out_f(&o, "registers:\n");
   out_f(&o, "  rip %016lx  rsp %016lx  rbp %016lx  rflags %08lx\n", (unsigned long)m->mc_rip,
         (unsigned long)m->mc_rsp, (unsigned long)m->mc_rbp, (unsigned long)m->mc_rflags);
   out_f(&o, "  rax %016lx  rbx %016lx  rcx %016lx  rdx %016lx\n", (unsigned long)m->mc_rax,
         (unsigned long)m->mc_rbx, (unsigned long)m->mc_rcx, (unsigned long)m->mc_rdx);
   out_f(&o, "  rsi %016lx  rdi %016lx  r8  %016lx  r9  %016lx\n", (unsigned long)m->mc_rsi,
         (unsigned long)m->mc_rdi, (unsigned long)m->mc_r8, (unsigned long)m->mc_r9);
   out_f(&o, "  r10 %016lx  r11 %016lx  r12 %016lx  r13 %016lx\n", (unsigned long)m->mc_r10,
         (unsigned long)m->mc_r11, (unsigned long)m->mc_r12, (unsigned long)m->mc_r13);
   out_f(&o, "  r14 %016lx  r15 %016lx  fsbase %016lx\n\n", (unsigned long)m->mc_r14, (unsigned long)m->mc_r15,
         (unsigned long)m->mc_fsbase);
   out_flush(&o);

   struct symfile syms;
   int named = sym_open(&syms);
   out_f(&o, "backtrace (frame pointers%s):\n", named ? ", names from eboot.sym" : "; no eboot.sym for this build");
   uintptr_t pc = (uintptr_t)m->mc_rip, fp = (uintptr_t)m->mc_rbp, sp = (uintptr_t)m->mc_rsp;
   uintptr_t stack_lo = 0, stack_hi = 0;
   int have_stack = mapping(sp, &stack_lo, &stack_hi);
   frame_line(&o, &syms, 0, pc);
   for (int i = 1; i < 64 && have_stack; i++) {
      if ((fp & 7) || fp < sp || fp + 16 > stack_hi)
         break;
      uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
      if (!ret)
         break;
      frame_line(&o, &syms, i, ret);
      if (next <= fp)
         break;
      fp = next;
   }
   out_flush(&o);
   /* code addresses on the stack's top: callers of frames built without a frame pointer */
   if (have_stack) {
      out_f(&o, "\nstack scan (eboot addresses in the top 8 KB, newest first):\n");
      int found = 0;
      for (uintptr_t a = sp & ~(uintptr_t)7; a + 8 <= stack_hi && a < sp + 8192 && found < 32; a += 8) {
         uintptr_t v = *(uintptr_t *)a;
         if (v > g_base && v < g_text_end)
            frame_line(&o, &syms, ++found, v);
      }
   }
   if (syms.fd >= 0)
      close(syms.fd);

   out_f(&o, "\nlog (last lines of logs/Ship of Harkinian.log):\n");
   log_tail(&o);
   out_flush(&o);
   fsync(o.fd);

   if (g_context) {
      static char context[2048];
      context[0] = 0;
      out_f(&o, "\ngame:\n");
      out_flush(&o);
      g_context(context, sizeof context);
      context[sizeof context - 1] = 0;
      out_raw(&o, context, strlen(context));
      out_flush(&o);
   }
   out_f(&o, "\n(end of report)\n");
   out_flush(&o);
   fsync(o.fd);
   close(o.fd);
   say("crash: %s at 0x%lx (eboot+0x%lx): report in %s", signal_name(sig), (unsigned long)pc,
       (unsigned long)(pc - g_base), path);
}

static void
on_signal(int sig, siginfo_t *info, void *context)
{
   /* the console's own handling from here on: the default action, once this returns */
   for (size_t i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
      signal(g_signals[i], SIG_DFL);
   if (atomic_fetch_add(&g_busy, 1) == 0)
      write_report(sig, info, (ucontext_t *)context);
   /* a fault comes back at once, as the instruction runs again; abort() raises
    * again itself; anything else is raised here */
   if (sig != SIGSEGV && sig != SIGBUS && sig != SIGILL && sig != SIGFPE && sig != SIGABRT)
      raise(sig);
}

static void
read_small(const char *path, char *buf, size_t size, bool first_line)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return;
   ssize_t n = read(fd, buf, size - 1);
   close(fd);
   if (n > 0) {
      buf[n] = 0;
      if (first_line)
         buf[strcspn(buf, "\r\n")] = 0;
   }
}

void
ps5_crash_install(const char *data_dir)
{
   snprintf(g_data, sizeof g_data, "%s", data_dir);
   read_small("/app0/build_id.txt", g_build, sizeof g_build, true);
   char param[4096] = "";
   read_small("/app0/sce_sys/param.json", param, sizeof param, false);
   const char *v = strstr(param, "\"contentVersion\"");
   if (v && (v = strchr(v + 16, '"')))
      sscanf(v + 1, "%31[^\"]", g_version);

   uintptr_t start, end;
   if (mapping((uintptr_t)ps5_crash_anchor, &start, &end)) {
      g_base = start;
      g_text_end = end;
      /* the text starts with 16 bytes of int3 (PS5_Vulkan's ps5-pie.ld): a mapping split
       * below the anchor would not, and the consoles seen load at 0x400000 */
      if (((const uint64_t *)start)[0] != 0xccccccccccccccccull) {
         say("crash: the text mapping at 0x%lx does not start the image; assuming 0x400000", (unsigned long)start);
         g_base = 0x400000;
      }
   } else {
      g_base = 0x400000;
      g_text_end = 0x400000 + 0x4000000;
   }

   stack_t alt = { .ss_sp = g_altstack, .ss_size = sizeof g_altstack, .ss_flags = 0 };
   sigaltstack(&alt, NULL);
   struct sigaction action;
   memset(&action, 0, sizeof action);
   action.sa_sigaction = on_signal;
   action.sa_flags = SA_SIGINFO | SA_ONSTACK;
   sigemptyset(&action.sa_mask);
   int ok = 0;
   for (size_t i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
      ok += sigaction(g_signals[i], &action, NULL) == 0;
   say("crash handler: %d signals, image 0x%lx-0x%lx, reports in %s/logs", ok, (unsigned long)g_base,
       (unsigned long)g_text_end, g_data);
}
