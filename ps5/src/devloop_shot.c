/*
 * Ghostship for the PlayStation 5 - the title's own frame, for PS5 DevLoop's grab.
 *
 * DevLoop asks by leaving /app0/devloop-shot: removing it is the check, one
 * system call that fails on every ordinary launch. The renderer then hands
 * over a frame it read back, which is written here as a new PNG (at most 1280
 * wide) in /app0/userdata/screenshots, a folder that holds nothing else.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "platform.h"

#define SHOT_REQUEST "/app0/devloop-shot"
#define SHOT_FOLDER  "/app0/userdata/screenshots"

#ifdef PS5_DEVTOOLS
/* the dev build's debug server asks too (screenshot command, ps5/src/devtools) */
bool ps5_devtools_shot_wanted(void);
int ps5_devtools_shot_max_width(void);
void ps5_devtools_shot_png(const uint8_t *png, size_t size, int width, int height);
static bool devtools_pending;
#endif

bool
ps5_shot_requested(void)
{
   static unsigned calls;

#ifdef PS5_DEVTOOLS
   if (ps5_devtools_shot_wanted()) {
      devtools_pending = true;
      return true;
   }
#endif
   return ++calls % 4 == 0 && unlink(SHOT_REQUEST) == 0;
}

struct buf {
   uint8_t *data;
   size_t size, cap;
   bool failed;
};

static void
put(struct buf *b, const void *data, size_t size)
{
   if (b->failed)
      return;
   if (b->size + size > b->cap) {
      size_t cap = b->cap ? b->cap * 2 : 65536;
      while (cap < b->size + size)
         cap *= 2;
      uint8_t *grown = realloc(b->data, cap);
      if (!grown) {
         b->failed = true;
         return;
      }
      b->data = grown;
      b->cap = cap;
   }
   memcpy(b->data + b->size, data, size);
   b->size += size;
}

static void
chunk(struct buf *b, const char *type, const uint8_t *data, uint32_t size)
{
   uint8_t head[8] = { size >> 24, size >> 16, size >> 8, size, type[0], type[1], type[2], type[3] };
   uint32_t crc = crc32(0, head + 4, 4);
   if (size) /* crc32(crc, NULL, 0) is zlib's "initial value", 0, not crc */
      crc = crc32(crc, data, size);
   uint8_t tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };

   put(b, head, 8);
   if (size)
      put(b, data, size);
   put(b, tail, 4);
}

/* pixels: width x height, four bytes each, rows top first; bgr says which end red is at.
 * Every step-th pixel each way so the PNG is at most max_width wide (0: full size). */
static bool
encode_png(struct buf *png, const uint8_t *pixels, int width, int height, bool bgr, int max_width, int *out_w,
           int *out_h)
{
   const int step = max_width > 0 ? (width + max_width - 1) / max_width : 1;
   const int w = width / step, h = height / step;
   const size_t raw_size = (size_t)h * (1 + (size_t)w * 3);
   uint8_t *raw = malloc(raw_size);
   uLongf packed_size = compressBound(raw_size);
   uint8_t *packed = malloc(packed_size);
   bool ok = false;

   if (!raw || !packed)
      goto done;
   for (int y = 0; y < h; y++) {
      const uint8_t *in = pixels + (size_t)y * step * width * 4;
      uint8_t *out = raw + (size_t)y * (1 + (size_t)w * 3);
      *out++ = 0; /* no filter */
      for (int x = 0; x < w; x++, in += (size_t)step * 4, out += 3) {
         out[0] = in[bgr ? 2 : 0];
         out[1] = in[1];
         out[2] = in[bgr ? 0 : 2];
      }
   }
   if (compress2(packed, &packed_size, raw, raw_size, 1) != Z_OK)
      goto done;
   {
      const uint8_t header[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
      put(png, "\x89PNG\r\n\x1a\n", 8);
      chunk(png, "IHDR", header, sizeof header);
      chunk(png, "IDAT", packed, (uint32_t)packed_size);
      chunk(png, "IEND", NULL, 0);
   }
   ok = !png->failed;
   *out_w = w;
   *out_h = h;
done:
   free(raw);
   free(packed);
   return ok;
}

void
ps5_shot_write(const uint8_t *pixels, int width, int height, bool bgr)
{
   static unsigned count;
   struct buf png = { 0 };
   char path[128];
   FILE *file;
   int w, h;

#ifdef PS5_DEVTOOLS
   if (devtools_pending) {
      devtools_pending = false;
      if (encode_png(&png, pixels, width, height, bgr, ps5_devtools_shot_max_width(), &w, &h))
         ps5_devtools_shot_png(png.data, png.size, w, h);
      free(png.data);
      return;
   }
#endif
   if (!encode_png(&png, pixels, width, height, bgr, 1280, &w, &h))
      goto done;
   mkdir(SHOT_FOLDER, 0777);
   snprintf(path, sizeof path, SHOT_FOLDER "/shot-%04u.png", count++);
   file = fopen(path, "wb");
   if (file) {
      fwrite(png.data, 1, png.size, file);
      fclose(file);
      chmod(path, 0666);
      say("devloop: %s (%dx%d)", path, w, h);
   }
done:
   free(png.data);
}
