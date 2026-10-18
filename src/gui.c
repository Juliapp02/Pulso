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

static void draw_cpu(Rectangle box, const struct snapshot *snapshot, const struct history *history)
{
	Rectangle area = draw_box(box, TextFormat("cpu %.0f%%", snapshot->cpu_total));
	float cores_width = fminf(area.width * 0.45f, 520);
	Rectangle graph = { area.x, area.y, area.width - cores_width - PAD, area.height };
	draw_graph(graph, history->cpu, 100, 120, 0);

	int rows = fmaxf(1, area.height / ROW);
	int columns = (snapshot->cpu_count + rows - 1) / rows;
	float column_width = cores_width / columns;

	for (int cpu = 0; cpu < snapshot->cpu_count; cpu++) {
		float x = area.x + area.width - cores_width + (cpu / rows) * column_width;
		float y = area.y + (cpu % rows) * ROW;
		float usage = snapshot->cpu[cpu];
		draw_meter((Rectangle){ x, y + 6, column_width - PAD, FONT - 10 }, usage / 100, 120, 0);
		DrawText(TextFormat("%-3.0f", usage), x + column_width - PAD - 36, y, FONT, text);
	}
}

static void draw_memory(Rectangle box, const struct snapshot *snapshot, const struct history *history)
{
	Rectangle area = draw_box(box, "memory");
	float used = snapshot->mem_total ? (float)snapshot->mem_used / snapshot->mem_total : 0;

	DrawText(TextFormat("used %s", human(snapshot->mem_used)), area.x, area.y, FONT, text);
	const char *total = TextFormat("of %s", human(snapshot->mem_total));
	DrawText(total, area.x + area.width - MeasureText(total, FONT), area.y, FONT, dim);
	draw_meter((Rectangle){ area.x, area.y + ROW, area.width, 8 }, used, 120, 0);

	Rectangle graph = { area.x, area.y + ROW + 16, area.width, area.height - ROW - 16 };
	draw_graph(graph, history->mem, 100, 120, 0);
}

static void draw_series(Rectangle area, const char *name, const float *values, float max, float hue)
{
	DrawText(name, area.x, area.y, FONT, dim);
	const char *current = TextFormat("%s/s", human(values[HISTORY - 1]));
	DrawText(current, area.x + area.width - MeasureText(current, FONT), area.y, FONT, text);
	draw_graph((Rectangle){ area.x, area.y + ROW, area.width, area.height - ROW }, values, max, hue, hue);
}

static void draw_traffic(Rectangle box, const char *name, const char *in_name, const float *in,
			 const char *out_name, const float *out)
{
	Rectangle area = draw_box(box, name);
	float max = fmaxf(fmaxf(peak(in), peak(out)), 1024);
	float half = (area.height - PAD) / 2;

	draw_series((Rectangle){ area.x, area.y, area.width, half }, in_name, in, max, 190);
	draw_series((Rectangle){ area.x, area.y + half + PAD, area.width, half }, out_name, out, max, 290);
}

static void draw_processes(Rectangle box, const struct snapshot *snapshot)
{
	Rectangle area = draw_box(box, "processes");
	float name_x = area.x + 90;
	float meter_x = name_x + 210;
	float percent_x = area.x + area.width - 70;

	DrawText("pid", area.x, area.y, FONT, dim);
	DrawText("name", name_x, area.y, FONT, dim);
	DrawText("cpu", percent_x, area.y, FONT, dim);

	int rows = (area.height - ROW) / ROW;
	for (int i = 0; i < snapshot->proc_count && i < rows; i++) {
		const struct proc_stat *proc = &snapshot->procs[i];
		float y = area.y + ROW * (i + 1);

		DrawText(TextFormat("%d", proc->pid), area.x, y, FONT, dim);
		DrawText(proc->comm, name_x, y, FONT, text);
		draw_meter((Rectangle){ meter_x, y + 6, percent_x - meter_x - PAD, FONT - 10 }, proc->cpu / 100, 120, 0);
		DrawText(TextFormat("%5.1f", proc->cpu), percent_x, y, FONT, text);
	}
}

static void draw_dashboard(const struct snapshot *snapshot, const struct history *history)
{
	float width = GetScreenWidth();
	float height = GetScreenHeight();
	float left_width = width * 0.42f;

	Rectangle cpu = { PAD, GAP, width - 2 * PAD, height * 0.36f };
	float top = cpu.y + cpu.height + GAP;
	float rest = height - top - PAD;
	float third = (rest - 2 * GAP) / 3;

	Rectangle memory = { PAD, top, left_width, third };
	Rectangle network = { PAD, top + third + GAP, left_width, third };
	Rectangle disk = { PAD, top + 2 * (third + GAP), left_width, third };
	Rectangle processes = { left_width + 2 * PAD, top, width - left_width - 3 * PAD, rest };

	draw_cpu(cpu, snapshot, history);
	draw_memory(memory, snapshot, history);
	draw_traffic(network, "network", "download", history->net_rx, "upload", history->net_tx);
	draw_traffic(disk, "disk", "read", history->disk_read, "write", history->disk_write);
	draw_processes(processes, snapshot);
}

int main(void)
{
	SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
	InitWindow(1280, 820, "pulso");
	SetWindowMinSize(900, 600);
	SetTargetFPS(30);

	struct snapshot snapshot = { 0 };
	struct history history = { 0 };
	unsigned int last_tick = 0;
	double last_fetch = -1;
	bool connected = false;

	while (!WindowShouldClose()) {
		if (GetTime() - last_fetch >= 0.25) {
			last_fetch = GetTime();
			connected = fetch(&snapshot);
			if (connected && snapshot.tick != last_tick) {
				last_tick = snapshot.tick;
				record(&history, &snapshot);
			}
		}

		BeginDrawing();
		ClearBackground(background);
		if (connected && last_tick)
			draw_dashboard(&snapshot, &history);
		else
			DrawText("waiting for pulsod on " SOCKET_PATH, PAD * 2, PAD * 2, FONT, dim);
		EndDrawing();
	}

	CloseWindow();
	return 0;
}
