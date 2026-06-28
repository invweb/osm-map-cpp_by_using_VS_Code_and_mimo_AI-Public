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
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static void crash_handler(int sig) {
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    backtrace_symbols_fd(callstack, frames, 2);
    _exit(1);
}
#endif

static const int TILE_SIZE = 256;
static const int LOGO_SIZE = 200;
static const float SPLASH_DURATION = 3.0f;
static const int DEFAULT_ZOOM = 14;
static const double FALLBACK_LAT = 55.7558;
static const double FALLBACK_LON = 37.6173;

struct TileRequest {
    int x, y, zoom;
};

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

struct TileTexture {
    GLuint tex;
    int width, height;
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
    bool location_request_pending = false;

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
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                    auto* str = static_cast<std::string*>(userdata);
                    str->append(ptr, size * nmemb);
                    return size * nmemb;
                });
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                CURLcode res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);
                if (res == CURLE_OK) {
                    try {
                        auto lat_pos = response.find("\"latitude\":");
                        auto lon_pos = response.find("\"longitude\":");
                        if (lat_pos != std::string::npos && lon_pos != std::string::npos) {
                            double lat = std::stod(response.substr(lat_pos + 12));
                            double lon = std::stod(response.substr(lon_pos + 13));
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

    double location_lat = 0, location_lon = 0;
    bool location_result_ready = false;

    void worker_thread() {
        CURL* curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "OsmMapViewer/1.0 (cpp)");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        while (worker_running) {
            TileRequest req;
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
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                auto* str = static_cast<std::string*>(userdata);
                str->append(ptr, size * nmemb);
                return size * nmemb;
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK && !body.empty()) {
                int w, h, ch;
                unsigned char* data = stbi_load_from_memory((const unsigned char*)body.data(), (int)body.size(), &w, &h, &ch, 4);
                if (data && w == TILE_SIZE && h == TILE_SIZE) {
                    LoadedTile lt;
                    lt.x = req.x;
                    lt.y = req.y;
                    lt.zoom = req.zoom;
                    lt.pixels.assign(data, data + w * h * 4);
                    lt.width = w;
                    lt.height = h;
                    std::lock_guard<std::mutex> lock(result_mutex);
                    results.push_back(std::move(lt));
                }
                if (data) stbi_image_free(data);
            }
        }
        curl_easy_cleanup(curl);
    }

    static void tile_coords(double lat, double lon, int zoom, int& out_x, int& out_y) {
        double n = pow(2.0, zoom);
        out_x = (int)floor((lon + 180.0) / 360.0 * n);
        double lat_rad = lat * M_PI / 180.0;
        out_y = (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n);
    }

    void request_tile(int x, int y, int zoom) {
        TileKey key{x, y, zoom};
        if (tiles.count(key) || loading.count(key)) return;
        loading.insert(key);
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back({x, y, zoom});
    }

    void clear_tiles() {
        for (auto& [key, tex] : tiles) {
            glDeleteTextures(1, &tex.tex);
        }
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
                    unsigned char r = (unsigned char)(20 * detail + grid * 15);
                    unsigned char g = (unsigned char)(80 * detail + grid * 40);
                    unsigned char b = (unsigned char)(120 * detail + grid * 30);
                    pixels[idx + 0] = r;
                    pixels[idx + 1] = g;
                    pixels[idx + 2] = b;
                    pixels[idx + 3] = 255;
                } else if (dist < 0.9f) {
                    float alpha = (0.9f - dist) / 0.05f * 255.0f;
                    pixels[idx + 0] = 40;
                    pixels[idx + 1] = 100;
                    pixels[idx + 2] = 140;
                    pixels[idx + 3] = (unsigned char)alpha;
                } else {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0;
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

    void draw_marker(ImDrawList* draw_list, ImVec2 center, float tile_size) {
        int mx, my;
        tile_coords(my_lat, my_lon, zoom, mx, my);
        int ctx_x, cty_y;
        tile_coords(center_lat, center_lon, zoom, ctx_x, cty_y);

        float sx = center.x + (mx - ctx_x) * tile_size;
        float sy = center.y + (my - cty_y) * tile_size;

        draw_list->AddCircleFilled(ImVec2(sx + 1, sy + 2), 12, IM_COL32(0, 0, 0, 60));
        const int N = 20;
        ImVec2 pts[N];
        for (int i = 0; i < N; i++) {
            float angle = 2.0f * M_PI * i / N;
            pts[i] = ImVec2(sx + cosf(angle) * 10, sy - 2 + sinf(angle) * 10);
        }
        draw_list->AddConvexPolyFilled(pts, N, IM_COL32(220, 50, 50, 255));
        draw_list->AddCircleFilled(ImVec2(sx, sy - 2), 5, IM_COL32(255, 255, 255, 255));
        draw_list->AddText(ImVec2(sx - 30, sy + 14), IM_COL32(255, 255, 255, 255), "You are here");
    }

    void init() {
        start_time = std::chrono::steady_clock::now();
        logo_tex = create_logo();
        worker = std::thread(&MapApp::worker_thread, this);
        start_location_request();
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
            center_lat = loc_lat;
            center_lon = loc_lon;
            zoom = 14;
            my_lat = loc_lat;
            my_lon = loc_lon;
            has_location = true;
            clear_tiles();
        }
    }

    void update() {
        process_results();

        ImGuiIO& io = ImGui::GetIO();
        ImVec2 display_size = io.DisplaySize;

        if (!splash_done) {
            float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= SPLASH_DURATION) {
                splash_done = true;
            } else {
                ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
                draw_list->AddRectFilled(ImVec2(0, 0), display_size, IM_COL32(20, 25, 40, 255));

                ImVec2 logo_pos(display_size.x / 2 - 60, display_size.y / 2 - 120);
                draw_list->AddImage((ImTextureID)(intptr_t)logo_tex, logo_pos, ImVec2(logo_pos.x + 120, logo_pos.y + 120));

                const char* title = "OpenStreetMap Viewer";
                ImVec2 title_size = ImGui::CalcTextSize(title);
                draw_list->AddText(ImVec2(display_size.x / 2 - title_size.x / 2, display_size.y / 2 + 20), IM_COL32(255, 255, 255, 255), title);

                const char* subtitle = "powered by OpenStreetMap";
                ImVec2 sub_size = ImGui::CalcTextSize(subtitle);
                draw_list->AddText(ImVec2(display_size.x / 2 - sub_size.x / 2, display_size.y / 2 + 55), IM_COL32(150, 150, 150, 255), subtitle);

                return;
            }
        }

        int center_tx, center_ty;
        tile_coords(center_lat, center_lon, zoom, center_tx, center_ty);

        int tiles_x = (int)ceil(display_size.x / 2.0f / TILE_SIZE) + 1;
        int tiles_y = (int)ceil(display_size.y / 2.0f / TILE_SIZE) + 1;

        ImVec2 center_screen(display_size.x / 2, display_size.y / 2);

        for (int dy = -tiles_y; dy <= tiles_y; dy++) {
            for (int dx = -tiles_x; dx <= tiles_x; dx++) {
                int tx = center_tx + dx;
                int ty = center_ty + dy;
                request_tile(tx, ty, zoom);
            }
        }

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        for (int dy = -tiles_y; dy <= tiles_y; dy++) {
            for (int dx = -tiles_x; dx <= tiles_x; dx++) {
                int tx = center_tx + dx;
                int ty = center_ty + dy;
                float sx = center_screen.x + dx * TILE_SIZE;
                float sy = center_screen.y + dy * TILE_SIZE;
                ImVec2 p_min(sx, sy);
                ImVec2 p_max(sx + TILE_SIZE, sy + TILE_SIZE);

                TileKey key{tx, ty, zoom};
                auto it = tiles.find(key);
                if (it != tiles.end()) {
                    draw_list->AddImage((ImTextureID)(intptr_t)it->second.tex, p_min, p_max);
                } else {
                    draw_list->AddRectFilled(p_min, p_max, IM_COL32(230, 230, 230, 255));
                }
            }
        }

        if (has_location) {
            draw_marker(draw_list, center_screen, (float)TILE_SIZE);
        }

        float btn_size = 36;
        float margin = 10;
        float spacing = 8;
        float bx = display_size.x - margin - btn_size;
        float by = display_size.y - margin - btn_size;

        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
            ImVec2 mp = io.MousePos;
            if (mp.x > 0 && mp.x < display_size.x && mp.y > 0 && mp.y < display_size.y) {
                bool on_button = (mp.x > bx - margin && mp.y > by - btn_size * 3 - spacing * 2 - margin);
                if (!on_button) {
                    dragging = true;
                    drag_start = mp;
                }
            }
        }
        if (ImGui::IsMouseReleased(0)) {
            dragging = false;
        }
        if (dragging && ImGui::IsMouseDragging(0)) {
            ImVec2 mp = io.MousePos;
            float dx = mp.x - drag_start.x;
            float dy = mp.y - drag_start.y;
            double lat_rad = center_lat * M_PI / 180.0;
            double cos_lat = cos(lat_rad);
            if (fabs(cos_lat) < 0.01) cos_lat = 0.01;
            double mpp = 156543.03 * cos_lat / pow(2.0, zoom);
            center_lon -= dx * mpp / 111320.0;
            center_lat += dy * mpp / 110540.0;
            if (center_lat > 85.0) center_lat = 85.0;
            if (center_lat < -85.0) center_lat = -85.0;
            while (center_lon > 180.0) center_lon -= 360.0;
            while (center_lon < -180.0) center_lon += 360.0;
            drag_start = mp;
        }

        float scroll = io.MouseWheel;
        if (scroll > 0 && zoom < 19) {
            zoom++;
            clear_tiles();
        } else if (scroll < 0 && zoom > 1) {
            zoom--;
            clear_tiles();
        }

        ImGui::SetCursorScreenPos(ImVec2(bx, by - btn_size * 2 - spacing * 2));
        if (ImGui::Button("-", ImVec2(btn_size, btn_size)) && zoom > 1) {
            zoom--;
            clear_tiles();
        }
        ImGui::SetCursorScreenPos(ImVec2(bx, by - btn_size - spacing));
        if (ImGui::Button("+", ImVec2(btn_size, btn_size)) && zoom < 19) {
            zoom++;
            clear_tiles();
        }
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (location_loading) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::Button("...", ImVec2(btn_size, btn_size));
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("\xE2\x8C\x97", ImVec2(btn_size, btn_size))) {
                start_location_request();
            }
        }

        char coord_text[256];
        snprintf(coord_text, sizeof(coord_text), "Zoom: %d | Lat: %.4f | Lon: %.4f", zoom, center_lat, center_lon);
        draw_list->AddText(ImVec2(margin, display_size.y - margin - 20), IM_COL32(255, 255, 255, 255), coord_text);
    }

    void shutdown() {
        worker_running = false;
        if (worker.joinable()) worker.join();
        if (location_thread.joinable()) location_thread.join();
        for (auto& [key, tex] : tiles) {
            glDeleteTextures(1, &tex.tex);
        }
        if (logo_tex) glDeleteTextures(1, &logo_tex);
    }
};

int main() {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);

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

    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        fprintf(stderr, "[GLFW] window close requested\n");
    });

    glfwSetErrorCallback([](int error, const char* desc) {
        fprintf(stderr, "[GLFW] error %d: %s\n", error, desc);
    });

#ifdef __APPLE__
    {
        Class NSApplication = objc_getClass("NSApplication");
        SEL sel_shared = sel_registerName("sharedApplication");
        id nsApp = ((id(*)(Class, SEL))objc_msgSend)(NSApplication, sel_shared);
        SEL sel_activate = sel_registerName("activateIgnoringOtherApps:");
        ((void(*)(id, SEL, BOOL))objc_msgSend)(nsApp, sel_activate, YES);
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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

        try {
            app.update();
        } catch (const std::exception& e) {
            fprintf(stderr, "[ERROR] update: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[ERROR] update: unknown exception\n");
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
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
