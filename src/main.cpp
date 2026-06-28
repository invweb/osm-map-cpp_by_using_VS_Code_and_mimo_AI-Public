#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>
#include <curl/curl.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <sys/stat.h>
#endif

static const int TILE_SIZE = 256;
static const int LOGO_SIZE = 200;
static const float SPLASH_DURATION = 3.0f;
static const int DEFAULT_ZOOM = 14;
static const double FALLBACK_LAT = 55.7558;
static const double FALLBACK_LON = 37.6173;
static const float LONG_PRESS_DURATION = 0.6f;
static const float LONG_PRESS_RADIUS = 8.0f;

static const ImVec4 FLAG_COLORS[] = {
    {0.9f, 0.2f, 0.2f, 1.0f},
    {0.2f, 0.7f, 0.2f, 1.0f},
    {0.2f, 0.4f, 0.9f, 1.0f},
    {0.9f, 0.7f, 0.1f, 1.0f},
    {0.8f, 0.3f, 0.8f, 1.0f},
    {0.1f, 0.8f, 0.8f, 1.0f},
    {0.9f, 0.5f, 0.1f, 1.0f},
    {0.5f, 0.2f, 0.9f, 1.0f},
};
static const int FLAG_COLOR_COUNT = sizeof(FLAG_COLORS) / sizeof(FLAG_COLORS[0]);

struct TileRequest { int x, y, zoom; };

struct LoadedTile {
    int x, y, zoom;
    std::vector<unsigned char> pixels;
    int width, height;
};

struct TileKey {
    int x, y, zoom;
    bool operator==(const TileKey& o) const { return x == o.x && y == o.y && zoom == o.zoom; }
};

struct TileKeyHash {
    size_t operator()(const TileKey& k) const {
        size_t h = std::hash<int>()(k.x);
        h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.zoom) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct TileTexture { GLuint tex; int width, height; };

struct Flag {
    double lat, lon;
    int color_index;
};

struct MapApp {
    int zoom = DEFAULT_ZOOM;
    double center_lat = FALLBACK_LAT;
    double center_lon = FALLBACK_LON;

    std::unordered_map<TileKey, TileTexture, TileKeyHash> tiles;
    std::unordered_set<TileKey, TileKeyHash> loading;

    std::mutex queue_mutex;
    std::deque<TileRequest> queue;

    std::mutex result_mutex;
    std::deque<LoadedTile> results;

    std::thread worker;
    bool worker_running = true;

    ImVec2 drag_start = {0, 0};
    bool dragging = false;

    std::chrono::steady_clock::time_point start_time;
    bool splash_done = false;

    GLuint logo_tex = 0;

    bool has_location = false;
    double my_lat = 0, my_lon = 0;
    bool location_loading = false;
    std::thread location_thread;

    double location_lat = 0, location_lon = 0;
    bool location_result_ready = false;

    std::vector<Flag> flags;
    int next_color = 0;

    bool long_press_active = false;
    ImVec2 long_press_start = {0, 0};
    std::chrono::steady_clock::time_point long_press_time;
    bool long_press_fired = false;

    std::string flags_path;

    void load_flags() {
        std::ifstream f(flags_path);
        if (!f.is_open()) return;
        flags.clear();
        double lat, lon;
        int ci;
        while (f >> lat >> lon >> ci) {
            flags.push_back({lat, lon, ci % FLAG_COLOR_COUNT});
        }
        if (!flags.empty()) next_color = (flags.back().color_index + 1) % FLAG_COLOR_COUNT;
    }

    void save_flags() {
        std::ofstream f(flags_path);
        if (!f.is_open()) return;
        for (auto& fl : flags) {
            f << fl.lat << " " << fl.lon << " " << fl.color_index << "\n";
        }
    }

    void start_location_request() {
        if (location_loading) return;
        location_loading = true;
        if (location_thread.joinable()) location_thread.join();
        location_thread = std::thread([this]() {
            CURL* curl = curl_easy_init();
            if (curl) {
                std::string response;
                curl_easy_setopt(curl, CURLOPT_URL, "https://ipapi.co/json/");
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "OsmMapViewer/1.0 (cpp)");
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
                    return size * nmemb;
                });
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                CURLcode res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);
                if (res == CURLE_OK) {
                    try {
                        auto lp = response.find("\"latitude\":");
                        auto lop = response.find("\"longitude\":");
                        if (lp != std::string::npos && lop != std::string::npos) {
                            double lat = std::stod(response.substr(lp + 12));
                            double lon = std::stod(response.substr(lop + 13));
                            std::lock_guard<std::mutex> lock(result_mutex);
                            location_lat = lat;
                            location_lon = lon;
                            location_result_ready = true;
                        }
                    } catch (...) {}
                }
            }
            location_loading = false;
        });
    }

    void worker_thread() {
        CURL* curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OsmMapViewer/1.0 (cpp)");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        while (worker_running) {
            TileRequest req{};
            bool have_req = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!queue.empty()) {
                    req = queue.front();
                    queue.pop_front();
                    have_req = true;
                }
            }
            if (!have_req) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            char url[256];
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", req.zoom, req.x, req.y);

            std::string body;
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                static_cast<std::string*>(ud)->append(ptr, size * nmemb);
                return size * nmemb;
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK && !body.empty()) {
                int w, h, ch;
                unsigned char* data = stbi_load_from_memory((const unsigned char*)body.data(), (int)body.size(), &w, &h, &ch, 4);
                if (data && w == TILE_SIZE && h == TILE_SIZE) {
                    LoadedTile lt;
                    lt.x = req.x; lt.y = req.y; lt.zoom = req.zoom;
                    lt.pixels.assign(data, data + w * h * 4);
                    lt.width = w; lt.height = h;
                    std::lock_guard<std::mutex> lock(result_mutex);
                    results.push_back(std::move(lt));
                }
                if (data) stbi_image_free(data);
            }
        }
        curl_easy_cleanup(curl);
    }

    static void tile_coords(double lat, double lon, int zoom, int& ox, int& oy) {
        double n = pow(2.0, zoom);
        ox = (int)floor((lon + 180.0) / 360.0 * n);
        double lat_rad = lat * M_PI / 180.0;
        oy = (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n);
    }

    void screen_to_latlon(ImVec2 screen, ImVec2 center, int center_tx, int center_ty, double& lat, double& lon) {
        float px = screen.x - center.x;
        float py = screen.y - center.y;
        double n = pow(2.0, zoom);
        double tile_x = center_tx + px / TILE_SIZE;
        double tile_y = center_ty + py / TILE_SIZE;
        lon = tile_x / n * 360.0 - 180.0;
        lat = atan(sinh(M_PI * (1.0 - 2.0 * tile_y / n))) * 180.0 / M_PI;
        if (lat > 85.0) lat = 85.0;
        if (lat < -85.0) lat = -85.0;
        while (lon > 180.0) lon -= 360.0;
        while (lon < -180.0) lon += 360.0;
    }

    void request_tile(int x, int y, int z) {
        TileKey key{x, y, z};
        if (tiles.count(key) || loading.count(key)) return;
        loading.insert(key);
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back({x, y, z});
    }

    void clear_tiles() {
        for (auto& [key, tex] : tiles) glDeleteTextures(1, &tex.tex);
        tiles.clear();
        loading.clear();
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.clear();
    }

    GLuint create_logo() {
        std::vector<unsigned char> pixels(LOGO_SIZE * LOGO_SIZE * 4);
        for (int y = 0; y < LOGO_SIZE; y++) {
            for (int x = 0; x < LOGO_SIZE; x++) {
                float cx = ((float)x - LOGO_SIZE / 2.0f) / (LOGO_SIZE / 2.0f);
                float cy = ((float)y - LOGO_SIZE / 2.0f) / (LOGO_SIZE / 2.0f);
                float dist = sqrtf(cx * cx + cy * cy);
                int idx = (y * LOGO_SIZE + x) * 4;
                if (dist < 0.85f) {
                    float grid = (sinf(cx * 12.0f) * cosf(cy * 12.0f) + 1.0f) * 0.5f;
                    float detail = 1.0f - dist * 0.5f;
                    pixels[idx] = (unsigned char)(20 * detail + grid * 15);
                    pixels[idx+1] = (unsigned char)(80 * detail + grid * 40);
                    pixels[idx+2] = (unsigned char)(120 * detail + grid * 30);
                    pixels[idx+3] = 255;
                } else if (dist < 0.9f) {
                    float alpha = (0.9f - dist) / 0.05f * 255.0f;
                    pixels[idx] = 40; pixels[idx+1] = 100; pixels[idx+2] = 140;
                    pixels[idx+3] = (unsigned char)alpha;
                } else {
                    pixels[idx] = pixels[idx+1] = pixels[idx+2] = pixels[idx+3] = 0;
                }
            }
        }
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LOGO_SIZE, LOGO_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return tex;
    }

    void draw_marker(ImDrawList* dl, ImVec2 center, float ts) {
        int mx, my, ctx_x, cty_y;
        tile_coords(my_lat, my_lon, zoom, mx, my);
        tile_coords(center_lat, center_lon, zoom, ctx_x, cty_y);
        float sx = center.x + (mx - ctx_x) * ts;
        float sy = center.y + (my - cty_y) * ts;
        dl->AddCircleFilled(ImVec2(sx + 1, sy + 2), 12, IM_COL32(0, 0, 0, 60));
        const int N = 20;
        ImVec2 pts[N];
        for (int i = 0; i < N; i++) {
            float a = 2.0f * M_PI * i / N;
            pts[i] = ImVec2(sx + cosf(a) * 10, sy - 2 + sinf(a) * 10);
        }
        dl->AddConvexPolyFilled(pts, N, IM_COL32(220, 50, 50, 255));
        dl->AddCircleFilled(ImVec2(sx, sy - 2), 5, IM_COL32(255, 255, 255, 255));
        dl->AddText(ImVec2(sx - 30, sy + 14), IM_COL32(255, 255, 255, 255), "You are here");
    }

    void draw_flags(ImDrawList* dl, ImVec2 center, float ts) {
        int ctx_x, cty_y;
        tile_coords(center_lat, center_lon, zoom, ctx_x, cty_y);
        for (auto& fl : flags) {
            int fx, fy;
            tile_coords(fl.lat, fl.lon, zoom, fx, fy);
            float sx = center.x + (fx - ctx_x) * ts;
            float sy = center.y + (fy - cty_y) * ts;
            if (sx < -50 || sx > 2000 || sy < -50 || sy > 2000) continue;

            ImVec4 c = FLAG_COLORS[fl.color_index % FLAG_COLOR_COUNT];
            ImU32 col = IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), 255);
            ImU32 col_dark = IM_COL32((int)(c.x * 150), (int)(c.y * 150), (int)(c.z * 150), 255);

            dl->AddCircleFilled(ImVec2(sx + 1, sy + 2), 8, IM_COL32(0, 0, 0, 50));
            dl->AddCircleFilled(ImVec2(sx, sy), 7, col);
            dl->AddCircle(ImVec2(sx, sy), 7, col_dark, 0, 1.5f);
            dl->AddCircleFilled(ImVec2(sx, sy), 3, IM_COL32(255, 255, 255, 255));
        }
    }

    void init() {
        start_time = std::chrono::steady_clock::now();
        logo_tex = create_logo();
        worker = std::thread(&MapApp::worker_thread, this);
        start_location_request();

        const char* home = getenv("HOME");
        if (home) {
            flags_path = std::string(home) + "/.osm-map-flags.txt";
        } else {
            flags_path = ".osm-map-flags.txt";
        }
        load_flags();
    }

    void process_results() {
        std::deque<LoadedTile> batch;
        bool got_location = false;
        double loc_lat = 0, loc_lon = 0;
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            batch.swap(results);
            if (location_result_ready) {
                location_result_ready = false;
                got_location = true;
                loc_lat = location_lat;
                loc_lon = location_lon;
            }
        }
        for (auto& lt : batch) {
            TileKey key{lt.x, lt.y, lt.zoom};
            loading.erase(key);
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, lt.width, lt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, lt.pixels.data());
            tiles[key] = {tex, lt.width, lt.height};
        }
        if (got_location) {
            center_lat = loc_lat; center_lon = loc_lon; zoom = 14;
            my_lat = loc_lat; my_lon = loc_lon;
            has_location = true;
            clear_tiles();
        }
    }

    void update() {
        process_results();
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 ds = io.DisplaySize;

        if (!splash_done) {
            float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= SPLASH_DURATION) {
                splash_done = true;
            } else {
                auto* dl = ImGui::GetBackgroundDrawList();
                dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(20, 25, 40, 255));
                ImVec2 lp(ds.x / 2 - 60, ds.y / 2 - 120);
                dl->AddImage((ImTextureID)(intptr_t)logo_tex, lp, ImVec2(lp.x + 120, lp.y + 120));
                auto t = ImGui::CalcTextSize("OpenStreetMap Viewer");
                dl->AddText(ImVec2(ds.x / 2 - t.x / 2, ds.y / 2 + 20), IM_COL32(255, 255, 255, 255), "OpenStreetMap Viewer");
                auto s = ImGui::CalcTextSize("powered by OpenStreetMap");
                dl->AddText(ImVec2(ds.x / 2 - s.x / 2, ds.y / 2 + 55), IM_COL32(150, 150, 150, 255), "powered by OpenStreetMap");
                return;
            }
        }

        int ctx, cty;
        tile_coords(center_lat, center_lon, zoom, ctx, cty);
        int tx_count = (int)ceil(ds.x / 2.0f / TILE_SIZE) + 1;
        int ty_count = (int)ceil(ds.y / 2.0f / TILE_SIZE) + 1;
        ImVec2 cs(ds.x / 2, ds.y / 2);

        for (int dy = -ty_count; dy <= ty_count; dy++)
            for (int dx = -tx_count; dx <= tx_count; dx++)
                request_tile(ctx + dx, cty + dy, zoom);

        auto* dl = ImGui::GetBackgroundDrawList();
        for (int dy = -ty_count; dy <= ty_count; dy++) {
            for (int dx = -tx_count; dx <= tx_count; dx++) {
                float sx = cs.x + dx * TILE_SIZE;
                float sy = cs.y + dy * TILE_SIZE;
                ImVec2 mn(sx, sy), mx(sx + TILE_SIZE, sy + TILE_SIZE);
                TileKey key{ctx + dx, cty + dy, zoom};
                auto it = tiles.find(key);
                if (it != tiles.end())
                    dl->AddImage((ImTextureID)(intptr_t)it->second.tex, mn, mx);
                else
                    dl->AddRectFilled(mn, mx, IM_COL32(230, 230, 230, 255));
            }
        }

        draw_flags(dl, cs, (float)TILE_SIZE);
        if (has_location) draw_marker(dl, cs, (float)TILE_SIZE);

        float bs = 36, mg = 10, sp = 8;
        float bx = ds.x - mg - bs, by = ds.y - mg - bs;

        bool over_button = io.MousePos.x > bx - mg && io.MousePos.y > by - bs * 3 - sp * 2 - mg;

        if (ImGui::IsMouseClicked(0) && !over_button) {
            ImVec2 mp = io.MousePos;
            if (mp.x > 0 && mp.x < ds.x && mp.y > 0 && mp.y < ds.y) {
                dragging = true;
                drag_start = mp;
                long_press_active = true;
                long_press_start = mp;
                long_press_time = std::chrono::steady_clock::now();
                long_press_fired = false;
            }
        }

        if (long_press_active && ImGui::IsMouseDown(0) && !long_press_fired) {
            ImVec2 mp = io.MousePos;
            float dist = sqrtf((mp.x - long_press_start.x) * (mp.x - long_press_start.x) +
                               (mp.y - long_press_start.y) * (mp.y - long_press_start.y));
            if (dist > LONG_PRESS_RADIUS) {
                long_press_active = false;
            } else {
                float held = std::chrono::duration<float>(std::chrono::steady_clock::now() - long_press_time).count();
                if (held >= LONG_PRESS_DURATION) {
                    long_press_fired = true;
                    long_press_active = false;
                    double lat, lon;
                    screen_to_latlon(long_press_start, cs, ctx, cty, lat, lon);
                    flags.push_back({lat, lon, next_color});
                    next_color = (next_color + 1) % FLAG_COLOR_COUNT;
                    save_flags();
                }
            }
        }

        if (ImGui::IsMouseReleased(0)) {
            dragging = false;
            long_press_active = false;
        }

        if (dragging && ImGui::IsMouseDragging(0)) {
            ImVec2 mp = io.MousePos;
            float dx = mp.x - drag_start.x, dy = mp.y - drag_start.y;
            double cl = cos(center_lat * M_PI / 180.0);
            if (fabs(cl) < 0.01) cl = 0.01;
            double mpp = 156543.03 * cl / pow(2.0, zoom);
            center_lon -= dx * mpp / 111320.0;
            center_lat += dy * mpp / 110540.0;
            if (center_lat > 85.0) center_lat = 85.0;
            if (center_lat < -85.0) center_lat = -85.0;
            while (center_lon > 180.0) center_lon -= 360.0;
            while (center_lon < -180.0) center_lon += 360.0;
            drag_start = mp;
        }

        float scroll = io.MouseWheel;
        if (scroll > 0 && zoom < 19) { zoom++; clear_tiles(); }
        else if (scroll < 0 && zoom > 1) { zoom--; clear_tiles(); }

        auto* fg = ImGui::GetForegroundDrawList();

        auto draw_btn = [&](float x, float y, const char* label, bool active) {
            ImVec2 mn(x, y), mx(x + bs, y + bs);
            ImU32 bg = active ? IM_COL32(60, 60, 60, 220) : IM_COL32(40, 40, 40, 200);
            ImU32 border = IM_COL32(100, 100, 100, 255);
            fg->AddRectFilled(mn, mx, bg, 4.0f);
            fg->AddRect(mn, mx, border, 4.0f);
            auto ts = ImGui::CalcTextSize(label);
            fg->AddText(ImVec2(x + bs / 2 - ts.x / 2, y + bs / 2 - ts.y / 2), IM_COL32(255, 255, 255, 255), label);
        };

        bool minus_hovered = io.MousePos.x > bx && io.MousePos.x < bx + bs && io.MousePos.y > by - bs * 2 - sp * 2 && io.MousePos.y < by - bs * 2 - sp * 2 + bs;
        bool plus_hovered = io.MousePos.x > bx && io.MousePos.x < bx + bs && io.MousePos.y > by - bs - sp && io.MousePos.y < by - bs - sp + bs;
        bool loc_hovered = io.MousePos.x > bx && io.MousePos.x < bx + bs && io.MousePos.y > by && io.MousePos.y < by + bs;

        draw_btn(bx, by - bs * 2 - sp * 2, "-", minus_hovered);
        draw_btn(bx, by - bs - sp, "+", plus_hovered);

        {
            float lx = bx, ly = by;
            ImVec2 mn(lx, ly), mx(lx + bs, ly + bs);
            ImU32 bg = loc_hovered ? IM_COL32(60, 60, 60, 220) : IM_COL32(40, 40, 40, 200);
            fg->AddRectFilled(mn, mx, bg, 4.0f);
            fg->AddRect(mn, mx, IM_COL32(100, 100, 100, 255), 4.0f);
            float cx = lx + bs / 2, cy = ly + bs / 2;
            ImU32 ic = IM_COL32(220, 50, 50, 255);
            fg->AddCircle(ImVec2(cx, cy), 8, ic, 0, 2.0f);
            fg->AddCircleFilled(ImVec2(cx, cy), 2.5f, ic);
            fg->AddLine(ImVec2(cx, cy - 12), ImVec2(cx, cy - 5), ic, 2.0f);
            fg->AddLine(ImVec2(cx, cy + 5), ImVec2(cx, cy + 12), ic, 2.0f);
            fg->AddLine(ImVec2(cx - 12, cy), ImVec2(cx - 5, cy), ic, 2.0f);
            fg->AddLine(ImVec2(cx + 5, cy), ImVec2(cx + 12, cy), ic, 2.0f);
        }

        if (io.MouseClicked[0]) {
            if (minus_hovered && zoom > 1) { zoom--; clear_tiles(); }
            if (plus_hovered && zoom < 19) { zoom++; clear_tiles(); }
            if (loc_hovered && !location_loading) start_location_request();
        }

        char ct[256];
        snprintf(ct, sizeof(ct), "Zoom: %d | Lat: %.4f | Lon: %.4f", zoom, center_lat, center_lon);
        dl->AddText(ImVec2(mg, ds.y - mg - 20), IM_COL32(255, 255, 255, 255), ct);
    }

    void shutdown() {
        worker_running = false;
        if (worker.joinable()) worker.join();
        if (location_thread.joinable()) location_thread.join();
        for (auto& [key, tex] : tiles) glDeleteTextures(1, &tex.tex);
        if (logo_tex) glDeleteTextures(1, &logo_tex);
    }
};

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(800, 600, "OpenStreetMap Viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#ifdef __APPLE__
    {
        Class cls = objc_getClass("NSApplication");
        id nsApp = ((id(*)(Class, SEL))objc_msgSend)(cls, sel_registerName("sharedApplication"));
        ((void(*)(id, SEL, BOOL))objc_msgSend)(nsApp, sel_registerName("activateIgnoringOtherApps:"), YES);
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    MapApp app;
    app.init();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.update();
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    curl_global_cleanup();
    return 0;
}
