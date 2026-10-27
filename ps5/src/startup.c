/*
 * Ship of Harkinian for the PlayStation 5 (from PS5_Ghostship) - what runs before the game's main.
 *
 * The game's main is its own (vendor/shipwright/soh/soh/OTRGlobals.cpp), so the
 * console's start-up is a constructor: standard error into klog and the
 * shell's splash dismissed, and where a title may keep its files.
 *
 * The title's folder is /app0 (/data/homebrew/<TITLE_ID>); what it carries is
 * replaced on every reinstall. What is yours lives in the data folder, outside
 * it, so a reinstall or a deleted title folder leaves it alone:
 *
 *   /data/soh/                 the data folder (PS5_DATA_DIR in env.txt moves it)
 *   /data/soh/oot.o2r          the game's data, made from your own ROM
 *   /data/soh/shipofharkinian.json, Save/, logs/, mods/, imgui.ini
 *   /app0/soh.o2r              Ship of Harkinian's own assets (part of the release)
 *   /app0/env.txt              optional: NAME=value lines for the environment
 *
 * If the data folder cannot be made or written, /app0/userdata is used as
 * before. On the first start with a new data folder, what /app0/userdata holds
 * (saves, settings, oot.o2r, mods) is copied into it; files already there win.
 *
 * SPDX-License-Identifier: MIT
 */
#include <SDL_stdinc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "crash.h"
#include "platform.h"

#ifndef PS5_DATA_DIR_DEFAULT
#define PS5_DATA_DIR_DEFAULT "/data/soh"
#endif

/* Both copies of the environment: the console's libc keeps what setenv sets
 * apart from the `environ` SDL copies. */
static void
set(const char *name, const char *value, int overwrite)
{
   setenv(name, value, overwrite);
   SDL_setenv(name, value, overwrite);
}

/* The game leaves through exit() from many places (its Quit, a first-run prompt
 * answered No): the shell is asked to close the title, as when main returns.
 * Without that the console shows a title that ended by itself as crashed. */
void catchReturnFromMain(int status);

static void
leave(void)
{
   static int left;

   if (!left++)
      catchReturnFromMain(0);
}

#define LEGACY_DATA "/app0/userdata"
static char data_dir[256] = LEGACY_DATA;

const char *
ps5_data_dir(void)
{
   return data_dir;
}

static int
usable_dir(const char *path)
{
   char probe[300];
   if (mkdir(path, 0777) != 0 && errno != EEXIST)
      return 0;
   snprintf(probe, sizeof probe, "%s/.probe", path);
   FILE *file = fopen(probe, "w");
   if (!file)
      return 0;
   int ok = fputs("ok", file) >= 0;
   ok &= fclose(file) == 0;
   unlink(probe);
   return ok;
}

static void
copy_file(const char *from, const char *to)
{
   FILE *in = fopen(from, "rb");
   if (!in)
      return;
   char tmp[600];
   snprintf(tmp, sizeof tmp, "%s.part", to);
   FILE *out = fopen(tmp, "wb");
   if (!out) {
      fclose(in);
      return;
   }
   static char buffer[1 << 20];
   size_t n;
   int ok = 1;
   while ((n = fread(buffer, 1, sizeof buffer, in)) > 0)
      ok &= fwrite(buffer, 1, n, out) == n;
   ok &= !ferror(in);
   fclose(in);
   ok &= fclose(out) == 0;
   if (ok && rename(tmp, to) == 0)
      say("data: copied %s -> %s", from, to);
   else
      unlink(tmp);
}

/* Copies what `from` holds into `to`, never replacing a file already there. */
static void
migrate(const char *from, const char *to, int depth)
{
   DIR *dir = opendir(from);
   if (!dir || depth > 4) {
      if (dir)
         closedir(dir);
      return;
   }
   struct dirent *entry;
   while ((entry = readdir(dir))) {
      const char *name = entry->d_name;
      if (name[0] == '.' || !strcmp(name, "screenshots"))
         continue;
      char a[512], b[512];
      struct stat st;
      snprintf(a, sizeof a, "%s/%s", from, name);
      snprintf(b, sizeof b, "%s/%s", to, name);
      if (stat(a, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode)) {
         mkdir(b, 0777);
         migrate(a, b, depth + 1);
      } else if (stat(b, &st) != 0) {
         copy_file(a, b);
      }
   }
   closedir(dir);
}

static void
choose_data_dir(void)
{
   const char *wanted = getenv("PS5_DATA_DIR");
   if (!wanted || !*wanted)
      wanted = PS5_DATA_DIR_DEFAULT;
   if (strcmp(wanted, LEGACY_DATA) && usable_dir(wanted)) {
      snprintf(data_dir, sizeof data_dir, "%s", wanted);
      struct stat st;
      char marker[300];
      snprintf(marker, sizeof marker, "%s/.migrated", data_dir);
      if (stat(marker, &st) != 0) {
         migrate(LEGACY_DATA, data_dir, 0);
         FILE *file = fopen(marker, "w");
         if (file)
            fclose(file);
      }
   } else {
      mkdir(LEGACY_DATA, 0777);
      say("data: %s is not writable (%s); using %s", wanted, strerror(errno), LEGACY_DATA);
   }
   char path[300];
   snprintf(path, sizeof path, "%s/mods", data_dir);
   mkdir(path, 0777);
   /* the console has no /tmp: the ROM extractor's scratch files go here */
   snprintf(path, sizeof path, "%s/.tmp", data_dir);
   mkdir(path, 0777);
   setenv("TMPDIR", path, 1);
   say("data folder: %s", data_dir);
}

__attribute__((constructor)) static void
ps5_startup(void)
{
   platform_init(PS5_TITLE_NAME);
   atexit(leave);
   mkdir(LEGACY_DATA, 0777);
   /* PS5 DevLoop's grab takes the title's pictures from here, and removes them */
   mkdir("/app0/userdata/screenshots", 0777);
   chmod("/app0/userdata/screenshots", 0777);

   FILE *file = fopen("/app0/env.txt", "r");
   if (file) {
      char line[512];
      while (fgets(line, sizeof line, file)) {
         line[strcspn(line, "\r\n")] = 0;
         char *equals = strchr(line, '=');
         if (line[0] == '#' || !equals || equals == line)
            continue;
         *equals = 0;
         set(line, equals + 1, 1);
         say("env.txt: %s=%s", line, equals + 1);
      }
      fclose(file);
   }
   choose_data_dir();
   ps5_crash_install(data_dir);
}
