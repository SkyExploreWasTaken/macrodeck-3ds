#include "imgdecode.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "webp/decode.h"

static int is_webp(const uint8_t *d, size_t len)
{
	return len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0;
}

uint8_t *image_decode_fit(const uint8_t *data, size_t len, int box, int *ow, int *oh)
{
	int w = 0, h = 0;
	uint8_t *src = NULL;
	int from_webp = 0;
	if (is_webp(data, len)) {
		src = WebPDecodeRGBA(data, len, &w, &h);
		from_webp = 1;
	} else {
		int comp;
		src = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
	}
	if (!src || w <= 0 || h <= 0) {
		if (src) {
			if (from_webp) WebPFree(src);
			else stbi_image_free(src);
		}
		return NULL;
	}

	int tw, th;
	if (w >= h) {
		tw = w < box ? w : box;
		th = (int)((long)h * tw / w);
	} else {
		th = h < box ? h : box;
		tw = (int)((long)w * th / h);
	}
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;

	uint8_t *out = calloc((size_t)box * box, 4);
	if (out) {
		// Box filter: every target pixel averages the source pixels it covers (premultiplied by alpha so
		// transparent edges do not bleed dark fringes).
		for (int y = 0; y < th; y++) {
			int sy0 = (int)((long)y * h / th), sy1 = (int)((long)(y + 1) * h / th);
			if (sy1 <= sy0) sy1 = sy0 + 1;
			for (int x = 0; x < tw; x++) {
				int sx0 = (int)((long)x * w / tw), sx1 = (int)((long)(x + 1) * w / tw);
				if (sx1 <= sx0) sx1 = sx0 + 1;
				unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
				for (int sy = sy0; sy < sy1 && sy < h; sy++) {
					const uint8_t *p = src + ((size_t)sy * w + sx0) * 4;
					for (int sx = sx0; sx < sx1 && sx < w; sx++, p += 4) {
						r += (unsigned long)p[0] * p[3];
						g += (unsigned long)p[1] * p[3];
						b += (unsigned long)p[2] * p[3];
						a += p[3];
						n++;
					}
				}
				uint8_t *d = out + ((size_t)y * box + x) * 4;
				if (a) {
					d[0] = (uint8_t)(r / a);
					d[1] = (uint8_t)(g / a);
					d[2] = (uint8_t)(b / a);
				}
				d[3] = n ? (uint8_t)(a / n) : 0;
			}
		}
	}
	if (from_webp) WebPFree(src);
	else stbi_image_free(src);
	*ow = tw;
	*oh = th;
	return out;
}
