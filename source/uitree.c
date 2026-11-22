#include "uitree.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *KNOWN_TYPES[] = {
	"ui.stack", "ui.text", "ui.image", "ui.range-bar", "ui.slider", "ui.button", "ui.layer", "ui.chart",
	"ui.text-field", "ui.list", "ui.transform", "ui.shape", "ui.icon", "ui.grid", "ui.gauge", "ui.toggle",
	"ui.segmented", "ui.dial", "ui.modifier", "ui.responsive", "ui.first-fit", NULL,
};

static bool is_known(const char *type)
{
	if (!type) return false;
	for (int i = 0; KNOWN_TYPES[i]; i++) {
		if (strcmp(type, KNOWN_TYPES[i]) == 0) return true;
	}
	return false;
}

static const char *str_of(const cJSON *obj, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const char *node_type(const cJSON *node) { return str_of(node, "type"); }
static const char *node_id(const cJSON *node) { return str_of(node, "id"); }
static const cJSON *node_props(const cJSON *node) { return cJSON_GetObjectItemCaseSensitive(node, "properties"); }

/* ---------- patches ---------- */

typedef struct {
	cJSON *node;
	cJSON *parent;    // NULL for the root
	bool as_fallback; // node sits in parent's "fallback" slot rather than "children"
} Found;

static bool find_node(cJSON *node, cJSON *parent, bool as_fallback, const char *id, Found *out)
{
	if (!node) return false;
	const char *nid = node_id(node);
	if (nid && strcmp(nid, id) == 0) {
		out->node = node;
		out->parent = parent;
		out->as_fallback = as_fallback;
		return true;
	}
	cJSON *children = cJSON_GetObjectItemCaseSensitive(node, "children");
	cJSON *child;
	cJSON_ArrayForEach(child, children) {
		if (find_node(child, node, false, id, out)) return true;
	}
	cJSON *fb = cJSON_GetObjectItemCaseSensitive(node, "fallback");
	if (fb && find_node(fb, node, true, id, out)) return true;
	return false;
}

static bool detach(cJSON **root, Found *f)
{
	if (!f->parent) return false; // the root cannot be removed
	if (f->as_fallback) {
		cJSON_DetachItemFromObjectCaseSensitive(f->parent, "fallback");
		return true;
	}
	cJSON *children = cJSON_GetObjectItemCaseSensitive(f->parent, "children");
	cJSON_DetachItemViaPointer(children, f->node);
	(void)root;
	return true;
}

static bool insert_child(cJSON *parent, cJSON *node, int index)
{
	cJSON *children = cJSON_GetObjectItemCaseSensitive(parent, "children");
	if (!children) {
		children = cJSON_AddArrayToObject(parent, "children");
		if (!children) return false;
	}
	int size = cJSON_GetArraySize(children);
	if (index < 0 || index > size) return false;
	if (index == size) return cJSON_AddItemToArray(children, node);
	return cJSON_InsertItemInArray(children, index, node);
}

static bool apply_op(cJSON **root, const cJSON *op)
{
	const char *kind = str_of(op, "op");
	const char *id = str_of(op, "nodeId");
	if (!kind || !id) return false;
	const cJSON *index_item = cJSON_GetObjectItemCaseSensitive(op, "index");
	const char *parent_id = str_of(op, "parentId");

	Found f = {0};
	if (strcmp(kind, "set-properties") == 0) {
		if (!find_node(*root, NULL, false, id, &f)) return false;
		cJSON *props = cJSON_GetObjectItemCaseSensitive(f.node, "properties");
		if (!props) props = cJSON_AddObjectToObject(f.node, "properties");
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(op, "properties");
		const cJSON *item;
		cJSON_ArrayForEach(item, set) {
			cJSON *copy = cJSON_Duplicate(item, 1);
			if (!copy) return false;
			if (cJSON_GetObjectItemCaseSensitive(props, item->string)) {
				cJSON_ReplaceItemInObjectCaseSensitive(props, item->string, copy);
			} else {
				cJSON_AddItemToObject(props, item->string, copy);
			}
		}
		const cJSON *removed = cJSON_GetObjectItemCaseSensitive(op, "removedProperties");
		cJSON_ArrayForEach(item, removed) {
			if (cJSON_IsString(item)) cJSON_DeleteItemFromObjectCaseSensitive(props, item->valuestring);
		}
		return true;
	}
	if (strcmp(kind, "insert-node") == 0) {
		const cJSON *node = cJSON_GetObjectItemCaseSensitive(op, "node");
		if (!node || !parent_id) return false;
		if (!find_node(*root, NULL, false, parent_id, &f)) return false;
		int size = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(f.node, "children"));
		int index = cJSON_IsNumber(index_item) ? index_item->valueint : size;
		cJSON *copy = cJSON_Duplicate(node, 1);
		if (!copy) return false;
		if (!insert_child(f.node, copy, index)) {
			cJSON_Delete(copy);
			return false;
		}
		return true;
	}
	if (strcmp(kind, "remove-node") == 0) {
		if (!find_node(*root, NULL, false, id, &f) || !f.parent) return false;
		detach(root, &f);
		cJSON_Delete(f.node);
		return true;
	}
	if (strcmp(kind, "replace-node") == 0) {
		const cJSON *node = cJSON_GetObjectItemCaseSensitive(op, "node");
		if (!node || !find_node(*root, NULL, false, id, &f)) return false;
		cJSON *copy = cJSON_Duplicate(node, 1);
		if (!copy) return false;
		if (!f.parent) {
			cJSON_Delete(*root);
			*root = copy;
			return true;
		}
		if (f.as_fallback) {
			cJSON_ReplaceItemInObjectCaseSensitive(f.parent, "fallback", copy);
		} else {
			cJSON_ReplaceItemViaPointer(cJSON_GetObjectItemCaseSensitive(f.parent, "children"), f.node, copy);
		}
		return true;
	}
	if (strcmp(kind, "move-node") == 0) {
		if (!parent_id || !find_node(*root, NULL, false, id, &f) || !f.parent) return false;
		cJSON *node = f.node;
		detach(root, &f);
		Found p = {0};
		if (!find_node(*root, NULL, false, parent_id, &p)) {
			cJSON_Delete(node);
			return false;
		}
		int size = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p.node, "children"));
		int index = cJSON_IsNumber(index_item) ? index_item->valueint : size;
		if (!insert_child(p.node, node, index)) {
			cJSON_Delete(node);
			return false;
		}
		return true;
	}
	return false;
}

bool tree_apply_patch(cJSON **root, int *revision, const cJSON *patch)
{
	if (!*root || !patch) return false;
	const cJSON *from = cJSON_GetObjectItemCaseSensitive(patch, "fromRevision");
	const cJSON *to = cJSON_GetObjectItemCaseSensitive(patch, "toRevision");
	const cJSON *ops = cJSON_GetObjectItemCaseSensitive(patch, "operations");
	if (!cJSON_IsNumber(from) || !cJSON_IsNumber(to) || !cJSON_IsArray(ops)) return false;
	if (from->valueint != *revision || to->valueint <= from->valueint) return false;

	// Atomic: work on a copy so a failing operation leaves the current tree untouched.
	cJSON *work = cJSON_Duplicate(*root, 1);
	if (!work) return false;
	const cJSON *op;
	cJSON_ArrayForEach(op, ops) {
		if (!apply_op(&work, op)) {
			cJSON_Delete(work);
			return false;
		}
	}
	cJSON_Delete(*root);
	*root = work;
	*revision = to->valueint;
	return true;
}

/* ---------- colours ---------- */

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	c = (char)tolower((unsigned char)c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

bool parse_color(const char *s, uint32_t *rgba)
{
	if (!s || s[0] != '#') return false;
	size_t len = strlen(s + 1);
	int v[8];
	for (size_t i = 0; i < len && i < 8; i++) {
		v[i] = hexv(s[1 + i]);
		if (v[i] < 0) return false;
	}
	if (len == 3 || len == 4) {
		uint32_t r = (uint32_t)(v[0] * 17), g = (uint32_t)(v[1] * 17), b = (uint32_t)(v[2] * 17);
		uint32_t a = len == 4 ? (uint32_t)(v[3] * 17) : 255;
		*rgba = (r << 24) | (g << 16) | (b << 8) | a;
		return true;
	}
	if (len == 6 || len == 8) {
		uint32_t r = (uint32_t)(v[0] * 16 + v[1]), g = (uint32_t)(v[2] * 16 + v[3]), b = (uint32_t)(v[4] * 16 + v[5]);
		uint32_t a = len == 8 ? (uint32_t)(v[6] * 16 + v[7]) : 255;
		*rgba = (r << 24) | (g << 16) | (b << 8) | a;
		return true;
	}
	return false;
}

/* ---------- flattening ---------- */

static bool events_contain(const cJSON *node, const char *event)
{
	const cJSON *events = cJSON_GetObjectItemCaseSensitive(node_props(node), "events");
	const cJSON *e;
	cJSON_ArrayForEach(e, events) {
		if (cJSON_IsString(e) && strcmp(e->valuestring, event) == 0) return true;
	}
	return false;
}

static void add_line(WidgetView *v, const char *text, const cJSON *props)
{
	// A label may carry newlines: each becomes its own line, up to the tile's budget.
	const char *p = text;
	while (*p && v->nlines < MAX_LINES) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		TextLine *line = &v->lines[v->nlines];
		memset(line, 0, sizeof *line);
		if (len >= sizeof line->text) len = sizeof line->text - 1;
		memcpy(line->text, p, len);
		line->text[len] = 0;
		// Trim trailing spaces/CR so empty lines are dropped.
		while (len && (line->text[len - 1] == ' ' || line->text[len - 1] == '\r')) line->text[--len] = 0;
		if (len) {
			line->has_color = parse_color(str_of(props, "color"), &line->color);
			const char *role = str_of(props, "role");
			line->muted = role && (strcmp(role, "muted") == 0 || strcmp(role, "secondary") == 0);
			v->nlines++;
		}
		if (!nl) break;
		p = nl + 1;
	}
}

static void text_of(const cJSON *props, char *out, size_t len)
{
	out[0] = 0;
	const cJSON *t = cJSON_GetObjectItemCaseSensitive(props, "text");
	if (cJSON_IsString(t)) {
		snprintf(out, len, "%s", t->valuestring);
		return;
	}
	if (cJSON_IsNumber(t)) {
		snprintf(out, len, "%g", t->valuedouble);
		return;
	}
	// Styled spans: concatenate their text parts.
	const cJSON *spans = cJSON_GetObjectItemCaseSensitive(props, "spans");
	const cJSON *s;
	size_t o = 0;
	cJSON_ArrayForEach(s, spans) {
		const char *st = str_of(s, "text");
		if (st && o + 1 < len) o += (size_t)snprintf(out + o, len - o, "%s", st);
	}
	if (o) return;
	// Localized text ({"$localized":{...}}) has no catalogue here; show its fallback when it has one.
	if (cJSON_IsObject(t)) {
		const char *fb = str_of(t, "fallback");
		if (!fb) fb = str_of(t, "text");
		if (fb) snprintf(out, len, "%s", fb);
	}
}

static double num_of(const cJSON *props, const char *key, double def)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(props, key);
	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void walk(const cJSON *node, int depth, WidgetView *v, ImageResolver resolve, void *ctx)
{
	if (!node || depth > 24) return;
	const char *type = node_type(node);
	const cJSON *props = node_props(node);

	if (!is_known(type)) {
		// A component this reader does not know: its fallback subtree is what we are meant to draw.
		const cJSON *fb = cJSON_GetObjectItemCaseSensitive(node, "fallback");
		if (fb) walk(fb, depth + 1, v, resolve, ctx);
		return;
	}

	const char *bg = str_of(props, "background");
	if (bg && depth <= 3 && !v->has_bg) {
		if (strcmp(bg, "transparent") == 0) {
			if (depth == 0) v->transparent = true;
		} else if (parse_color(bg, &v->bg)) {
			v->has_bg = true;
		}
	}
	if (depth == 0) {
		const char *justify = str_of(props, "justify");
		if (justify) {
			if (strcmp(justify, "start") == 0) v->justify = -1;
			else if (strcmp(justify, "end") == 0) v->justify = 1;
			else v->justify = 0;
		}
		if (str_of(props, "borderStyle")) {
			v->has_border = parse_color(str_of(props, "borderColor"), &v->border);
			if (!v->has_border) {
				v->has_border = true;
				v->border = 0xFFFFFFFF;
			}
		}
	}
	if (events_contain(node, "press") || events_contain(node, "long-press") || events_contain(node, "tap")) {
		v->pressable = true;
	}

	const cJSON *source = cJSON_GetObjectItemCaseSensitive(props, "source");
	if (source && cJSON_IsObject(source) && v->image < 0 && resolve) {
		v->image = resolve(source, ctx);
		if (v->image >= 0) v->has_tint = parse_color(str_of(props, "tint"), &v->tint);
	}

	if (type && strcmp(type, "ui.text") == 0) {
		char text[256];
		text_of(props, text, sizeof text);
		if (text[0]) add_line(v, text, props);
	} else if (type && strcmp(type, "ui.slider") == 0) {
		v->has_value = true;
		v->value = (float)num_of(props, "level", 0);
		v->pressable = true;
		if (events_contain(node, "change")) {
			const char *dir = str_of(props, "direction");
			v->slider = true;
			v->vertical = dir && strcmp(dir, "vertical") == 0;
		}
	} else if (type && strcmp(type, "ui.range-bar") == 0 && !v->has_value) {
		v->has_value = true;
		v->value = (float)num_of(props, "end", num_of(props, "marker", 0));
	} else if (type && strcmp(type, "ui.gauge") == 0 && !v->has_value) {
		double val = num_of(props, "value", 0), mn = num_of(props, "min", 0), mx = num_of(props, "max", 1);
		if (mx > mn) {
			v->has_value = true;
			v->value = (float)((val - mn) / (mx - mn));
		}
	} else if (type && strcmp(type, "ui.toggle") == 0) {
		const cJSON *on = cJSON_GetObjectItemCaseSensitive(props, "value");
		if (cJSON_IsBool(on)) {
			v->has_value = true;
			v->value = cJSON_IsTrue(on) ? 1.0f : 0.0f;
		}
		v->pressable = true;
	}

	const cJSON *children = cJSON_GetObjectItemCaseSensitive(node, "children");
	const cJSON *child;
	int count = 0;
	cJSON_ArrayForEach(child, children) {
		walk(child, depth + 1, v, resolve, ctx);
		count++;
	}
	(void)count;
}

void tree_extract(const cJSON *root, WidgetView *v, ImageResolver resolve, void *ctx)
{
	v->has_tree = root != NULL;
	v->transparent = false;
	v->has_bg = false;
	v->has_border = false;
	v->image = -1;
	v->has_tint = false;
	v->justify = 0;
	v->has_value = false;
	v->value = 0;
	v->nlines = 0;
	v->pressable = false;
	v->slider = false;
	v->vertical = false;
	if (!root) return;
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "root");
	walk(r ? r : root, 0, v, resolve, ctx);
	if (v->value < 0) v->value = 0;
	if (v->value > 1) v->value = 1;
}

static const char *find_event(const cJSON *node, const char *event, int depth)
{
	if (!node || depth > 24) return NULL;
	const char *type = node_type(node);
	if (!is_known(type)) {
		// Unknown components claim gestures themselves; prefer them, then their fallback.
		if (events_contain(node, event)) return node_id(node);
		return find_event(cJSON_GetObjectItemCaseSensitive(node, "fallback"), event, depth + 1);
	}
	if (events_contain(node, event)) return node_id(node);
	const cJSON *child;
	cJSON_ArrayForEach(child, cJSON_GetObjectItemCaseSensitive(node, "children")) {
		const char *id = find_event(child, event, depth + 1);
		if (id) return id;
	}
	return NULL;
}

const char *tree_find_event_node(const cJSON *root, const char *event)
{
	if (!root) return NULL;
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "root");
	return find_event(r ? r : root, event, 0);
}

static const char *find_slider(const cJSON *node, bool *vertical, int depth)
{
	if (!node || depth > 24) return NULL;
	const char *type = node_type(node);
	if (!is_known(type)) return find_slider(cJSON_GetObjectItemCaseSensitive(node, "fallback"), vertical, depth + 1);
	if (type && strcmp(type, "ui.slider") == 0 && events_contain(node, "change")) {
		const char *dir = str_of(node_props(node), "direction");
		*vertical = dir && strcmp(dir, "vertical") == 0;
		return node_id(node);
	}
	const cJSON *child;
	cJSON_ArrayForEach(child, cJSON_GetObjectItemCaseSensitive(node, "children")) {
		const char *id = find_slider(child, vertical, depth + 1);
		if (id) return id;
	}
	return NULL;
}

const char *tree_find_slider(const cJSON *root, bool *vertical)
{
	if (!root) return NULL;
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "root");
	return find_slider(r ? r : root, vertical, 0);
}
