#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "raylib.h"
#include "pulso.h"

#define HISTORY 160
#define PAD 12
#define GAP 18
#define FONT 20
#define ROW 26

struct history {
	float cpu[HISTORY];
	float mem[HISTORY];
	float net_rx[HISTORY];
	float net_tx[HISTORY];
	float disk_read[HISTORY];
	float disk_write[HISTORY];
};

static const Color background = { 14, 16, 22, 255 };
static const Color border = { 58, 64, 80, 255 };
static const Color text = { 222, 224, 230, 255 };
static const Color dim = { 120, 126, 142, 255 };
static const Color title = { 120, 200, 255, 255 };

static bool fetch(struct snapshot *snapshot)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = SOCKET_PATH };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	bool ok = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
		  recv(fd, snapshot, sizeof(*snapshot), MSG_WAITALL) == sizeof(*snapshot);
	close(fd);
	return ok;
}

static void push(float *values, float value)
{
	memmove(values, values + 1, (HISTORY - 1) * sizeof(*values));
	values[HISTORY - 1] = value;
}

static void record(struct history *history, const struct snapshot *snapshot)
{
	push(history->cpu, snapshot->cpu_total);
	push(history->mem, snapshot->mem_total ? 100.0f * snapshot->mem_used / snapshot->mem_total : 0);
	push(history->net_rx, snapshot->net_rx);
	push(history->net_tx, snapshot->net_tx);
	push(history->disk_read, snapshot->disk_read);
	push(history->disk_write, snapshot->disk_write);
}

static float peak(const float *values)
{
	float highest = 0;
	for (int i = 0; i < HISTORY; i++)
		highest = fmaxf(highest, values[i]);
	return highest;
}

static const char *human(double bytes)
{
	const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	int unit = 0;

	while (bytes >= 1024 && unit < 4) {
		bytes /= 1024;
		unit++;
	}
	return TextFormat("%.1f %s", bytes, units[unit]);
}

static Color shade(float from_hue, float to_hue, float level)
{
	return ColorFromHSV(from_hue + (to_hue - from_hue) * level, 0.6f, 0.9f);
}

static Rectangle draw_box(Rectangle box, const char *name)
{
	DrawRectangleLinesEx(box, 1, border);
	DrawRectangle(box.x + 10, box.y - FONT / 2, MeasureText(name, FONT) + 12, FONT, background);
	DrawText(name, box.x + 16, box.y - FONT / 2, FONT, title);
	return (Rectangle){ box.x + PAD, box.y + PAD + 4, box.width - 2 * PAD, box.height - 2 * PAD - 4 };
}

static void draw_graph(Rectangle area, const float *values, float max, float from_hue, float to_hue)
{
	float step = area.width / HISTORY;

	for (int i = 0; i < HISTORY; i++) {
		float level = max > 0 ? fminf(values[i] / max, 1) : 0;
		int height = level * area.height;
		Color top = shade(from_hue, to_hue, level);
		DrawRectangleGradientV(area.x + i * step, area.y + area.height - height, fmaxf(step - 1, 1), height, top, Fade(top, 0.15f));
	}
}

static void draw_meter(Rectangle area, float level, float from_hue, float to_hue)
{
	level = fminf(fmaxf(level, 0), 1);
	DrawRectangleRec(area, Fade(border, 0.5f));
	DrawRectangle(area.x, area.y, area.width * level, area.height, shade(from_hue, to_hue, level));
}
