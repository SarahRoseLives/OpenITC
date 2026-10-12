#include "tiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "theme.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace openitc::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTile = 256.0;

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

#ifdef _WIN32
bool http_get(const std::string& url, std::vector<uint8_t>& out) {
    const std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hs = WinHttpOpen(L"OpenITC/0.1 (+https://sarahsforge.dev)",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return false;
    WinHttpSetTimeouts(hs, 8000, 8000, 8000, 8000);

    bool ok = false;
    HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
    if (hc) {
        HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          https ? WINHTTP_FLAG_SECURE : 0);
        if (hr) {
            const wchar_t* hdr = L"User-Agent: OpenITC/0.1\r\nAccept: image/png,*/*\r\n";
            WinHttpAddRequestHeaders(hr, hdr, (DWORD)-1L,
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hr, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    for (;;) {
                        if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) {
                            ok = !out.empty();
                            break;
                        }
                        std::vector<uint8_t> buf(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hr, buf.data(), avail, &read)) break;
                        out.insert(out.end(), buf.data(), buf.data() + read);
                    }
                }
            }
            WinHttpCloseHandle(hr);
        }
        WinHttpCloseHandle(hc);
    }
    WinHttpCloseHandle(hs);
    return ok;
}
#else
bool http_get(const std::string&, std::vector<uint8_t>&) { return false; }
#endif

double merc_v(double lat_deg) {
    const double lat = lat_deg * kPi / 180.0;
    return (1.0 - std::asinh(std::tan(lat)) / kPi) / 2.0;
}
double inv_merc_lat(double v) {
    const double n = kPi * (1.0 - 2.0 * v);
    return std::atan(std::sinh(n)) * 180.0 / kPi;
}

}  // namespace

TileCache::~TileCache() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void TileCache::init(const std::string& cache_dir, const std::string& url_template) {
    dir_ = cache_dir;
    url_ = url_template.empty() ? "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
                                : url_template;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    stop_ = false;
    th_ = std::thread([this] { worker(); });
}

std::string TileCache::path_for(const Key& k) const {
    return dir_ + "/" + std::to_string(k.z) + "/" + std::to_string(k.x) + "/" +
           std::to_string(k.y) + ".png";
}

std::string TileCache::url_for(const Key& k) const {
    std::string s = replace_all(url_, "{z}", std::to_string(k.z));
    s = replace_all(s, "{x}", std::to_string(k.x));
    s = replace_all(s, "{y}", std::to_string(k.y));
    return s;
}

bool TileCache::load_disk(const Key& k, std::vector<uint8_t>& png) {
    FILE* f = std::fopen(path_for(k).c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    png.resize((size_t)n);
    const size_t r = std::fread(png.data(), 1, png.size(), f);
    std::fclose(f);
    png.resize(r);
    return r > 0;
}

void TileCache::save_disk(const Key& k, const std::vector<uint8_t>& png) {
    const std::string p = path_for(k);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
}

void TileCache::enqueue(const Key& k) {
    q_.push(k);
    cv_.notify_one();
}

void TileCache::worker() {
    for (;;) {
        Key k;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) break;
            k = q_.front();
            q_.pop();
            auto it = tiles_.find(k);
            if (it == tiles_.end()) continue;
            it->second.state = Loading;
        }

        std::vector<uint8_t> png;
        bool have = load_disk(k, png);
        if (!have && online_) have = http_get(url_for(k), png);
        if (have) save_disk(k, png);

        bool decoded = false;
        if (have) {
            int w = 0, h = 0, ch = 0;
            unsigned char* px = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &ch, 4);
            if (px) {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = tiles_.find(k);
                if (it != tiles_.end()) {
                    it->second.rgba.assign(px, px + (size_t)w * h * 4);
                    it->second.w = w;
                    it->second.h = h;
                    it->second.state = Ready;
                }
                stbi_image_free(px);
                decoded = true;
            }
        }
        if (!decoded) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = tiles_.find(k);
            if (it != tiles_.end()) it->second.state = Failed;
            failed_count_.fetch_add(1);
        }
    }
}

ImTextureID TileCache::texture(int z, int x, int y) {
    const Key k{z, x, y};
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tiles_.find(k);
    if (it == tiles_.end()) {
        tiles_[k] = Tile{};
        pending_count_.fetch_add(1);
        enqueue(k);
        return 0;
    }
    Tile& t = it->second;
    if (t.state == Ready && t.gl != 0) return (ImTextureID)(uintptr_t)t.gl;
    if (t.state == Failed) {
        t.state = Pending;
        enqueue(k);
    }
    return 0;
}

void TileCache::pump() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : tiles_) {
        Tile& t = kv.second;
        if (t.gl != 0 || t.rgba.empty()) continue;
#ifdef _WIN32
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     t.rgba.data());
        t.gl = (unsigned)tex;
        pending_count_.fetch_sub(1);
        ready_count_.fetch_add(1);
#endif
        t.rgba.clear();
        t.rgba.shrink_to_fit();
    }
}

bool MapView::draw(TileCache& cache, const ImVec2& size,
                   const std::vector<openitc::BeaconRecord>& beacons,
                   uint32_t& clicked_radio_id) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##map", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) drag_accum_ = 0.0f;

    double S = kTile * std::pow(2.0, zoom_);
    const ImGuiIO& io = ImGui::GetIO();
    if (active) {
        drag_accum_ += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
        u_ -= io.MouseDelta.x / S;
        v_ -= io.MouseDelta.y / S;
        u_ = std::min(1.0, std::max(0.0, u_));
        v_ = std::min(1.0, std::max(0.0, v_));
    }

    bool clicked = false;
    if (hovered && io.MouseWheel != 0.0f) {
        const double mx = io.MousePos.x - p0.x;
        const double my = io.MousePos.y - p0.y;
        const double wu = u_ + (mx - size.x / 2) / S;
        const double wv = v_ + (my - size.y / 2) / S;
        zoom_ = std::min(17, std::max(3, zoom_ + (io.MouseWheel > 0 ? 1 : -1)));
        S = kTile * std::pow(2.0, zoom_);
        u_ = wu - (mx - size.x / 2) / S;
        v_ = wv - (my - size.y / 2) / S;
    }

    clicked = hovered && ImGui::IsItemDeactivated() && drag_accum_ < 4.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, theme::u(theme::bg));

    const double cx = u_ * S;
    const double cy = v_ * S;
    const int n = 1 << zoom_;
    const int tx0 = (int)std::floor((cx - size.x / 2) / kTile);
    const int tx1 = (int)std::floor((cx + size.x / 2) / kTile);
    const int ty0 = (int)std::floor((cy - size.y / 2) / kTile);
    const int ty1 = (int)std::floor((cy + size.y / 2) / kTile);
    for (int ty = ty0; ty <= ty1; ++ty) {
        if (ty < 0 || ty >= n) continue;
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tx < 0 || tx >= n) continue;
            const float sx = p0.x + (float)(tx * kTile - (cx - size.x / 2));
            const float sy = p0.y + (float)(ty * kTile - (cy - size.y / 2));
            ImTextureID t = cache.texture(zoom_, tx, ty);
            if (t)
                dl->AddImage(t, ImVec2(sx, sy), ImVec2(sx + kTile, sy + kTile));
            else
                dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + kTile, sy + kTile),
                                  theme::u(theme::slot));
        }
    }

    const double now = (double)std::time(nullptr);
    for (const auto& b : beacons) {
        if (!b.info.valid) continue;
        const double bu = (b.info.lon + 180.0) / 360.0;
        const double bv = merc_v(b.info.lat);
        const float sx = p0.x + (float)(bu * S - (cx - size.x / 2));
        const float sy = p0.y + (float)(bv * S - (cy - size.y / 2));
        if (sx < p0.x - 20 || sx > p1.x + 20 || sy < p0.y - 20 || sy > p1.y + 20) continue;
        const bool fresh = (now - b.last_seen) < 120.0;
        const ImU32 col = fresh ? theme::u(theme::green) : theme::u(theme::amber);
        dl->AddCircleFilled(ImVec2(sx, sy), 5.0f, col);
        dl->AddCircle(ImVec2(sx, sy), 5.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
        char label[32];
        std::snprintf(label, sizeof(label), "%06x", b.info.radio_id);
        dl->AddText(ImVec2(sx + 7, sy - 7), IM_COL32(255, 255, 255, 230), label);
        if (clicked && io.MousePos.x > sx - 9 && io.MousePos.x < sx + 9 &&
            io.MousePos.y > sy - 9 && io.MousePos.y < sy + 9) {
            clicked_radio_id = b.info.radio_id;
        }
    }

    dl->PopClipRect();
    dl->AddRect(p0, p1, theme::u(theme::border));
    const char* attr = "c OpenStreetMap contributors";
    dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(attr).x - 6, p1.y - 16),
                IM_COL32(160, 160, 170, 220), attr);
    return clicked;
}

void MapView::fit(const std::vector<openitc::BeaconRecord>& beacons) {
    if (beacons.empty()) return;
    double minlat = 90, maxlat = -90, minlon = 180, maxlon = -180;
    for (const auto& b : beacons) {
        if (!b.info.valid) continue;
        minlat = std::min(minlat, b.info.lat);
        maxlat = std::max(maxlat, b.info.lat);
        minlon = std::min(minlon, b.info.lon);
        maxlon = std::max(maxlon, b.info.lon);
    }
    if (minlat > maxlat) return;
    const double clat = (minlat + maxlat) / 2.0;
    const double clon = (minlon + maxlon) / 2.0;
    u_ = (clon + 180.0) / 360.0;
    v_ = merc_v(clat);
    const double spanx = std::max(0.02, (maxlon - minlon) / 360.0);
    const double spany = std::max(0.02, std::fabs(merc_v(maxlat) - merc_v(minlat)));
    int z = 3;
    while (z < 15 && (spanx * 256.0 * std::pow(2.0, z) < 500.0) &&
           (spany * 256.0 * std::pow(2.0, z) < 400.0))
        ++z;
    zoom_ = z;
}

double MapView::center_lat() const { return inv_merc_lat(v_); }
double MapView::center_lon() const { return u_ * 360.0 - 180.0; }

}  // namespace openitc::gui
