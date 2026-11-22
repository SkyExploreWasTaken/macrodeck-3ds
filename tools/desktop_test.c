// Desktop harness for the protocol core: runs the same worker as the 3DS against a real Macro Deck host and
// prints what the 3DS would draw. Build with tools/build_desktop_test.sh.
//   desktop_test <host> <port> [pairing-code] [label to press, or slider to set to 50%]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "config.h"
#include "model.h"
#include "net.h"
#include "platform.h"

static const char *phase_name(Phase p)
{
	static const char *names[] = {"IDLE", "DISCOVERING", "PICK_HOST", "CONNECTING", "NEED_PAIRING", "PAIRING", "ONLINE",
		"RETRY_WAIT", "ERROR"};
	return p <= PHASE_ERROR ? names[p] : "?";
}

static void dump(void)
{
	plat_mutex_lock(&g_model.lock);
	printf("phase=%s status='%s' error='%s' toast='%s'\n", phase_name(g_model.phase), g_model.status, g_model.error,
		g_model.toast);
	printf("host='%s' addr=%s version=%s profile='%s' (%d/%d) folder='%s' path='%s' grid=%dx%d widgets=%d\n",
		g_model.host_name, g_model.host_addr, g_model.host_version, g_model.profile_name, g_model.profile_index + 1,
		g_model.profile_count, g_model.folder_name, g_model.folder_path, g_model.cols, g_model.rows, g_model.nwidgets);
	for (int i = 0; i < g_model.nwidgets; i++) {
		WidgetView *w = &g_model.widgets[i];
		printf("  [%d,%d %dx%d] %-14s tree=%d bg=%s%08x img=%d(%d) value=%s%.2f lines:", w->x, w->y, w->w, w->h, w->type,
			w->has_tree, w->has_bg ? "" : "-", w->bg, w->image,
			w->image >= 0 ? (int)g_model.images[w->image].state : -1, w->has_value ? "" : "-", w->value);
		for (int l = 0; l < w->nlines; l++) printf(" '%s'", w->lines[l].text);
		printf("\n");
	}
	int nimg = 0, ready = 0, failed = 0;
	for (int i = 0; i < MAX_IMAGES; i++) {
		if (g_model.images[i].state != IMG_EMPTY) nimg++;
		if (g_model.images[i].state == IMG_DECODED || g_model.images[i].state == IMG_READY) ready++;
		if (g_model.images[i].state == IMG_FAILED) failed++;
	}
	printf("images: %d known, %d decoded, %d failed\n", nimg, ready, failed);
	plat_mutex_unlock(&g_model.lock);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s host port [code] [press-label] | --discover\n", argv[0]);
		return 2;
	}
	net_init();
	model_init();
	Config cfg;
	config_load(&cfg);
	client_start(&cfg);
	if (strcmp(argv[1], "--discover") == 0) {
		client_cmd(CMD_DISCOVER, NULL, NULL, 0);
		plat_sleep_ms(4500);
		plat_mutex_lock(&g_model.lock);
		for (int i = 0; i < g_model.nfound; i++)
			printf("found %s %s:%d v%s\n", g_model.found[i].name, g_model.found[i].ip, g_model.found[i].port,
				g_model.found[i].version);
		printf("found %d\n", g_model.nfound);
		plat_mutex_unlock(&g_model.lock);
		client_stop();
		return 0;
	}
	client_cmd(CMD_CONNECT, argv[1], "Test host", atoi(argv[2]));
	bool paired = false, pressed = false;
	for (int t = 0; t < 40; t++) {
		plat_sleep_ms(500);
		plat_mutex_lock(&g_model.lock);
		Phase p = g_model.phase;
		plat_mutex_unlock(&g_model.lock);
		if (p == PHASE_NEED_PAIRING && argc > 3 && !paired) {
			paired = true;
			printf("-> pairing with %s\n", argv[3]);
			client_cmd(CMD_PAIR, argv[3], NULL, 0);
		}
		if (p == PHASE_ONLINE && t >= 12 && argc > 4 && !pressed) {
			pressed = true;
			plat_mutex_lock(&g_model.lock);
			char id[40] = "";
			bool slider = false;
			for (int i = 0; i < g_model.nwidgets; i++) {
				for (int l = 0; l < g_model.widgets[i].nlines; l++) {
					if (strcmp(g_model.widgets[i].lines[l].text, argv[4]) == 0) {
						snprintf(id, sizeof id, "%s", g_model.widgets[i].id);
						slider = g_model.widgets[i].slider;
					}
				}
			}
			plat_mutex_unlock(&g_model.lock);
			if (slider) {
				printf("-> slide '%s' (%s) to 0.5\n", argv[4], id);
				client_cmd(CMD_SLIDER, id, NULL, 5000);
			} else {
				printf("-> press '%s' (%s)\n", argv[4], id);
				if (id[0]) client_cmd(CMD_PRESS, id, "press", 0);
			}
		}
		if (t % 4 == 3) dump();
	}
	dump();
	client_stop();
	net_exit();
	return 0;
}
