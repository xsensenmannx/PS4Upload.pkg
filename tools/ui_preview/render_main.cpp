/* Rendert ps4app/ui.h auf dem PC in Bitmaps (Vorschau der PS4-Oberflaeche). */

#include "ui.h"

#include <cstdio>
#include <cstring>
#include <vector>

class BitmapCanvas : public Canvas
{
public:
    BitmapCanvas(int width, int height) : width_(width), height_(height), pixels_((size_t)width * height * 3, 0) {}

    void fill(const UiColor &color) override
    {
        for (int y = 0; y < height_; ++y)
        {
            unsigned char *row = &pixels_[(size_t)y * width_ * 3];
            for (int x = 0; x < width_; ++x)
            {
                row[x * 3 + 0] = color.r;
                row[x * 3 + 1] = color.g;
                row[x * 3 + 2] = color.b;
            }
        }
    }

    void rect(int x, int y, int width, int height, const UiColor &color) override
    {
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = x + width > width_ ? width_ : x + width;
        int y1 = y + height > height_ ? height_ : y + height;

        for (int yy = y0; yy < y1; ++yy)
        {
            unsigned char *row = &pixels_[((size_t)yy * width_ + x0) * 3];
            for (int xx = x0; xx < x1; ++xx, row += 3)
            {
                row[0] = color.r;
                row[1] = color.g;
                row[2] = color.b;
            }
        }
    }

    bool write_bmp(const char *path) const
    {
        int row_bytes = (width_ * 3 + 3) & ~3;
        int image_size = row_bytes * height_;
        int file_size = 54 + image_size;
        unsigned char header[54] = { 0 };

        header[0] = 'B';
        header[1] = 'M';
        memcpy(header + 2, &file_size, 4);
        int offset = 54;
        memcpy(header + 10, &offset, 4);
        int info_size = 40;
        memcpy(header + 14, &info_size, 4);
        memcpy(header + 18, &width_, 4);
        memcpy(header + 22, &height_, 4);
        short planes = 1;
        memcpy(header + 26, &planes, 2);
        short bits_per_pixel = 24;
        memcpy(header + 28, &bits_per_pixel, 2);
        memcpy(header + 34, &image_size, 4);
        int pixels_per_meter = 2835;
        memcpy(header + 38, &pixels_per_meter, 4);
        memcpy(header + 42, &pixels_per_meter, 4);

        FILE *file = fopen(path, "wb");
        if (file == nullptr)
        {
            return false;
        }

        fwrite(header, 1, sizeof(header), file);
        std::vector<unsigned char> row((size_t)row_bytes, 0);
        for (int y = height_ - 1; y >= 0; --y)
        {
            for (int x = 0; x < width_; ++x)
            {
                const unsigned char *pixel = &pixels_[((size_t)y * width_ + x) * 3];
                row[x * 3 + 0] = pixel[2];
                row[x * 3 + 1] = pixel[1];
                row[x * 3 + 2] = pixel[0];
            }
            fwrite(row.data(), 1, (size_t)row_bytes, file);
        }

        fclose(file);
        return true;
    }

private:
    int width_;
    int height_;
    std::vector<unsigned char> pixels_;
};

static UiLogEntry g_preview_log[3];

static void prepare_log(void)
{
    const char *times[3] = { "0:12", "0:12", "0:11" };
    const char *clients[3] = { "192.168.0.77", "192.168.0.77", "192.168.0.31" };
    const char *lines[3] = { "GET / HTTP/1.1", "GET /status HTTP/1.1", "GET / HTTP/1.1" };

    for (int index = 0; index < 3; index++)
    {
        memset(&g_preview_log[index], 0, sizeof(UiLogEntry));
        snprintf(g_preview_log[index].time, sizeof(g_preview_log[index].time), "%s", times[index]);
        snprintf(g_preview_log[index].client, sizeof(g_preview_log[index].client), "%s", clients[index]);
        snprintf(g_preview_log[index].line, sizeof(g_preview_log[index].line), "%s", lines[index]);
    }
}

int main()
{
    prepare_log();

    for (int tab = 0; tab < UI_TAB_COUNT; tab++)
    {
        BitmapCanvas canvas(UI_WIDTH, UI_HEIGHT);
        UiState state = {};

        state.address = "192.168.0.42";
        state.client = "192.168.0.77";
        state.requests = 3;
        state.uptime_seconds = 754;
        state.server_ok = true;
        state.tab = tab;
        state.log_scroll = 0;
        state.log = g_preview_log;
        state.log_count = 3;

        static UiInstallView install_view;

        install_view.phase = "INSTALLATION";
        install_view.file = "PS4Upload-Direkt.pkg";
        install_view.title = "PS4Upload Direkt";
        install_view.content_id = "IV0000-BREW00090_00-PS4UPLOAD0000000";
        install_view.note = "";
        install_view.pin = "4821";
        install_view.percent = 42;
        install_view.error = 0;
        install_view.done = false;
        install_view.failed = false;
        install_view.active = true;
        state.install = &install_view;

        ui_draw(canvas, state);

        char path[64];
        snprintf(path, sizeof(path), ".tmp/render/ui-%d.bmp", tab);
        if (!canvas.write_bmp(path))
        {
            return 1;
        }
    }

    return 0;
}
