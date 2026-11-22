#!/bin/sh
# Builds the protocol core for the host machine (Linux/macOS) to test it against a real Macro Deck.
set -e
cd "$(dirname "$0")/.."
mkdir -p build-desktop
cc -O1 -g -Wall -Wno-format-truncation -DWEBP_USE_THREAD=0 \
	-DHAVE_CONFIG_H -Itools/desktop-include -Isource -Ilib/cjson -Ilib/stb -Ilib/libwebp/src -Ilib/libwebp \
	tools/desktop_test.c \
	source/client.c source/config.c source/http.c source/imgdecode.c source/mdns.c source/net.c \
	source/platform.c source/uitree.c source/ws.c lib/cjson/cJSON.c \
	lib/libwebp/src/dec/*.c lib/libwebp/src/dsp/*.c lib/libwebp/src/utils/*.c \
	-lpthread -lm -o build-desktop/desktop_test
echo built build-desktop/desktop_test
