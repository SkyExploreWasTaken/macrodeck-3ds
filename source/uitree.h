// Macro Deck UI model trees (one per widget session): patches and the flattened view the 3DS draws.
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "model.h"

// Applies a UiPatch ({fromRevision,toRevision,operations}) in place. False means resync needed.
bool tree_apply_patch(cJSON **root, int *revision, const cJSON *patch);

// Resolves an image resource handle ({resourceId, contentHash}) to an image slot index, or -1.
typedef int (*ImageResolver)(const cJSON *resource, void *ctx);

// Flattens a tree into what the tile renderer understands (face, artwork, a few text lines, a level).
void tree_extract(const cJSON *root, WidgetView *view, ImageResolver resolve, void *ctx);

// The id of the first node that emits `event` ("press", "long-press", ...), or NULL.
const char *tree_find_event_node(const cJSON *root, const char *event);

// The first ui.slider that emits "change": its id and whether it runs vertically.
const char *tree_find_slider(const cJSON *root, bool *vertical);

// Parses "#rgb", "#rrggbb", "#rrggbbaa". Returns false for anything else (vars, "transparent").
bool parse_color(const char *s, uint32_t *rgba);
