#pragma once

#include <stddef.h>
#include <stdint.h>

// Decodes PNG/JPEG/GIF (first frame)/WebP and fits it into a box x box RGBA buffer (row-major, top-left
// anchored, contents w x h). Returns a malloc'd box*box*4 buffer or NULL.
uint8_t *image_decode_fit(const uint8_t *data, size_t len, int box, int *w, int *h);
