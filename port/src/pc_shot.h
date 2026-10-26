/* Project Oogie: screenshots (F12), see pc_shot.c. */
#ifndef PC_SHOT_H
#define PC_SHOT_H

/* Saves a copy of the picture (BGRA rows, pitch bytes apart) as a PNG in
 * screenshots\, on a thread of its own. Returns at once. */
void pc_shot_save_bgra(const void *bgra, unsigned w, unsigned h, unsigned pitch);

/* Texture packs: queue a PNG (with alpha) to be written by a background
 * thread; returns 0 if it was skipped (queue full). */
int pc_png_queue_bgra(const char *path, const void *bgra, unsigned w, unsigned h, unsigned pitch);

/* Read an image file (PNG, DDS, ...) as BGRA rows; NULL if it cannot be
 * read. The caller frees it. */
unsigned char *pc_image_load_bgra(const char *path, unsigned *w, unsigned *h);

#endif
