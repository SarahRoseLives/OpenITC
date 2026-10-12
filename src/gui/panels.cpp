#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "app.h"
#include "theme.h"

namespace openitc::gui {
using namespace theme;

namespace {

const char* tname(int t) {
    const char* n = openitc::type_name(t);
    return n[0] ? n : "?";
}

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

void card_title(const char* title, const char* right = nullptr) {
    ImGui::TextColored(dim, "%s", title);
    if (right) {
        ImGui::SameLine();
        right_text(dim, right);
    }
}

void kv_cell(const char* k, const char* v, const ImVec4& vc) {
    ImGui::TableNextColumn();
    ImGui::TextColored(dim, "%s", k);
    ImGui::TextColored(vc, "%s", v);
}

ImVec4 tok_color(const std::string& tok) {
    if (starts_with(tok, "TID=")) return orange;
    if (starts_with(tok, "WIU=")) return cyan;
    if (starts_with(tok, "id=")) return purple;
    if (tok == "UTC") return yellow;
    if (tok.find(':') != std::string::npos) return ImVec4(0.85f, 0.90f, 0.96f, 1.0f);
    return text;
}

void seg(const ImVec4& c, const std::string& s) {
    if (s.empty()) return;
    ImGui::TextColored(c, "%s", s.c_str());
    ImGui::SameLine(0, 0);
}

void draw_log_line(const PacketEvent& e) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    char ts[16];
    local_hms(e.t_unix, ts, sizeof(ts));
    seg(dim, std::string("[") + ts + "] ");
    seg(purple, "ch" + std::to_string(e.channel) + " ");
    seg(dim, std::string(openitc::mode_name(e.mode)) + "  ");
    char tb[40];
    std::snprintf(tb, sizeof(tb), "type=%02x %-7s ", e.type, tname(e.type));
    seg(type_color(e.type), tb);
    seg(dim, "len=" + std::to_string(e.payload.size()) + "  ");

    const std::string sum = summarize(e);
    size_t i = 0;
    while (i < sum.size()) {
        size_t j = sum.find(' ', i);
        if (j == std::string::npos) j = sum.size();
        if (j > i) {
            const std::string tok = sum.substr(i, j - i);
            seg(tok_color(tok), tok + " ");
        }
        i = j + 1;
        while (i < sum.size() && sum[i] == ' ') i++;
    }
    ImGui::NewLine();
    const ImVec2 end = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x - 6, start.y),
                                              ImVec2(start.x - 3, end.y - 3),
                                              u(type_color(e.type)), 1.0f);
}

std::string itc_hms(const openitc::TimeRecord& r) {
    const double itc = openitc::itc_time_to_unix(r.info);
    if (itc <= 0.0) return "--:--:--";
    const double now = (double)std::time(nullptr);
    std::time_t tt = (std::time_t)(itc + (now - r.last_seen));
    std::tm* g = std::gmtime(&tt);
    char b[16] = "--:--:--";
    if (g) std::strftime(b, sizeof(b), "%H:%M:%S", g);
    return b;
}

const openitc::BeaconRecord* pick_beacon(const AppState& s, uint32_t want) {
    const std::vector<openitc::BeaconRecord> bs = s.repo.beacons();
    if (bs.empty()) return nullptr;
    const openitc::BeaconRecord* chosen = &bs[0];
    if (want) {
        for (const auto& b : bs)
            if (b.info.radio_id == want) {
                chosen = &b;
                break;
            }
    } else {
        for (const auto& b : bs)
            if (b.last_seen > chosen->last_seen) chosen = &b;
    }
    static openitc::BeaconRecord best;
    best = *chosen;
    return &best;
}

}  // namespace

void draw_header(AppState& s) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::BeginChild("header", ImVec2(0, 78), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("hleft", ImVec2(W - 400, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 7, p.y + 9), 6.0f, u(cyan));
        ImGui::Dummy(ImVec2(20, 20));
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(cyan, "OpenITC");
        ImGui::SameLine(0, 20);

        const std::string dev =
            s.engine ? s.engine->device_name() : openitc::rtl_device_name(s.ec.rtl.device_index);
        chip(dev.c_str(), text, slot);
        char f[40];
        std::snprintf(f, sizeof(f), "%.3f MHz", s.ec.rtl.center_freq / 1e6);
        chip(f, green, slot);
        std::snprintf(f, sizeof(f), "Gain %.1f dB", s.ec.rtl.gain_tenths_db / 10.0);
        chip(f, slate, slot);
        chip(s.ec.rtl.bias_tee ? "Bias-Tee ON" : "Bias-Tee off",
             s.ec.rtl.bias_tee ? amber : dim, slot);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("hright", ImVec2(390, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    {
        char loc[16] = "--:--:--";
        const std::time_t now = std::time(nullptr);
        std::strftime(loc, sizeof(loc), "%H:%M:%S", std::localtime(&now));
        right_text(dim, (std::string("LOCAL   ") + loc).c_str());

        const std::vector<openitc::TimeRecord> times = s.repo.times();
        std::string itc = "--:--:--";
        std::string extra = "waiting for type=06 TIME packet";
        bool fresh = false;
        if (!times.empty()) {
            openitc::TimeRecord best = times[0];
            for (const auto& r : times)
                if (r.last_seen > best.last_seen) best = r;
            itc = itc_hms(best);
            fresh = (now - best.last_seen) < 30.0;
            char e[64];
            std::snprintf(e, sizeof(e), "drift %+.1f s  (ITC - PC)", best.drift);
            extra = e;
        }
        right_text(fresh ? green : dim, "ITC (UTC)");
        right_text(cyan, itc.c_str());
        right_text(dim, extra.c_str());
    }
    ImGui::EndChild();

    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, wp.y + ws.y - 1),
                                        ImVec2(wp.x + ws.x, wp.y + ws.y - 1), u(border));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_sidebar(AppState& s) {
    ImGui::BeginChild("sidebar", ImVec2(380, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // Session state.
    if (begin_card("sess", ImVec2(0, 132))) {
        card_title("SESSION STATE");
        ImGui::Spacing();
        if (ImGui::BeginTable("sessgrid", 2, ImGuiTableFlags_SizingStretchSame)) {
            char v[40];
            kv_cell("Status", s.running ? "Scanning" : "Stopped", s.running ? green : dim);
            std::snprintf(v, sizeof(v), "%zu", s.ec.channels.size());
            kv_cell("Channels", v, text);
            ImGui::TableNextRow();
            double span = 0;
            if (!s.ec.channels.empty()) {
                int lo = *std::min_element(s.ec.channels.begin(), s.ec.channels.end());
                int hi = *std::max_element(s.ec.channels.begin(), s.ec.channels.end());
                span = (openitc::itc_frequency(hi) - openitc::itc_frequency(lo)) / 1e6;
            }
            std::snprintf(v, sizeof(v), "%.3f MHz", span);
            kv_cell("Span", v, text);
            std::snprintf(v, sizeof(v), "%.2f MSps", s.ec.rtl.sample_rate / 1e6);
            kv_cell("Rate", v, text);
            ImGui::EndTable();
        }
    }
    end_card();

    // Active WIUs (fills remaining space above the beacon card).
    const float beacon_h = 236.0f;
    float wiu_h = ImGui::GetContentRegionAvail().y - beacon_h - ImGui::GetStyle().ItemSpacing.y;
    if (wiu_h < 140.0f) wiu_h = 140.0f;
    if (begin_card("wiucard", ImVec2(0, wiu_h), true, true)) {
        const std::vector<openitc::WiuRecord> ws = s.repo.wius();
        char badge[32];
        std::snprintf(badge, sizeof(badge), "%zu tracking", ws.size());
        card_title("ACTIVE WIUs", badge);
        ImGui::Spacing();
        if (ws.empty()) {
            ImGui::TextDisabled("Awaiting WIU packets...");
        } else {
            std::vector<openitc::WiuRecord> sorted = ws;
            std::sort(sorted.begin(), sorted.end(),
                      [](const openitc::WiuRecord& a, const openitc::WiuRecord& b) {
                          return a.last_seen > b.last_seen;
                      });
            const double now = (double)std::time(nullptr);
            if (ImGui::BeginTable("wiutab", 3,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("WIU ID");
                ImGui::TableSetupColumn("Switch");
                ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
                for (const auto& w : sorted) {
                    const bool stale = (now - w.last_seen) > 60.0;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(stale ? dim : cyan, "%06x", w.info.wiu_id);
                    ImGui::TableNextColumn();
                    if (w.info.has_typical) {
                        const char swc = "INRX"[w.info.sw & 3];
                        const ImVec4 c = (swc == 'R')   ? red
                                         : (swc == 'X') ? amber
                                                        : text;
                        ImGui::TextColored(c, "%c", swc);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    if (w.info.has_typical) {
                        for (int i = 0; i < 3; ++i) {
                            if (i) ImGui::SameLine(0, 6);
                            ImGui::TextColored(w.info.sig[i] < 10 ? red : text, "%d",
                                               w.info.sig[i]);
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
    }
    end_card();

    // Latest beacon.
    if (begin_card("beacon", ImVec2(0, beacon_h))) {
        card_title("LATEST LOC BEACON");
        ImGui::Spacing();
        const openitc::BeaconRecord* b = pick_beacon(s, s.map_selected);
        if (!b) {
            ImGui::TextDisabled("No beacon (type=02) yet");
        } else {
            ImGui::TextColored(dim, "ID");
            ImGui::SameLine(0, 8);
            ImGui::TextColored(purple, "%06x", b->info.radio_id);
            ImGui::TextColored(dim, "Lat");
            ImGui::SameLine(0, 8);
            ImGui::TextColored(text, "%.6f", b->info.lat);
            ImGui::TextColored(dim, "Lon");
            ImGui::SameLine(0, 8);
            ImGui::TextColored(text, "%.6f", b->info.lon);
            ImGui::TextColored(dim, "Util / Rate");
            ImGui::SameLine(0, 8);
            ImGui::TextColored(text, "%d / %d", b->info.util, b->info.rate);
            ImGui::Spacing();
            ImGui::TextColored(dim, "Ch List");
            ImGui::Spacing();
            for (int i = 0; i < 6; ++i) {
                char c[8];
                std::snprintf(c, sizeof(c), "%d", b->info.ch_list[i]);
                chip(c, text, slot);
            }
        }
    }
    end_card();

    ImGui::EndChild();
}

void draw_map(AppState& s, const ImVec2& size) {
    if (begin_card("mapcard", size)) {
        const std::vector<openitc::BeaconRecord> bs = s.repo.beacons();
        if (s.map_autofit && !bs.empty()) {
            s.map.fit(bs);
            s.map_autofit = false;
        }
        card_title("BEACON MAP");
        ImGui::SameLine(0, 16);
        if (ImGui::SmallButton("+")) s.map.zoom_in();
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("-")) s.map.zoom_out();
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("fit")) s.map.fit(bs);
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("%zu beacons   z=%d   tiles %d ready / %d pending / %d failed",
                            bs.size(), s.map.zoom(), s.tiles.ready(), s.tiles.pending(),
                            s.tiles.failed());
        ImGui::Spacing();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        uint32_t clicked = 0;
        if (s.map.draw(s.tiles, avail, bs, clicked)) {
            s.map_selected = clicked;
        }
    }
    end_card();
}

void draw_terminal(AppState& s, const ImVec2& size) {
    if (begin_card("termcard", size)) {
        card_title("RAW FRAME LOG");
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(150);
        const char* cur = s.filters.type < 0 ? "all types" : tname(s.filters.type);
        if (ImGui::BeginCombo("##ftype", cur)) {
            static const int kTypes[] = {-1, 0x01, 0x02, 0x03, 0x04, 0x05,
                                         0x06, 0x07, 0x31, 0x33, 0x34, 0x70};
            for (int id : kTypes) {
                char label[32];
                if (id < 0)
                    std::snprintf(label, sizeof(label), "all types");
                else
                    std::snprintf(label, sizeof(label), "%02x %s", id, tname(id));
                if (ImGui::Selectable(label, s.filters.type == id)) s.filters.type = id;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(150);
        ImGui::InputTextWithHint("##ff", "filter text", s.filters.text,
                                 sizeof(s.filters.text));
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##fch", &s.filters.channel);
        ImGui::SameLine(0, 10);
        if (ImGui::SmallButton("clear")) {
            s.log.clear();
            s.frozen.clear();
        }
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton(s.paused ? "play" : "pause")) {
            s.paused = !s.paused;
            if (s.paused) s.frozen = s.log.snapshot(6000);
        }
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("export")) {
            const std::vector<PacketEvent> v =
                s.paused ? s.frozen : s.log.snapshot(5000);
            char name[64];
            std::snprintf(name, sizeof(name), "openitc_export_%lld.log",
                          (long long)std::time(nullptr));
            std::ofstream out(name);
            size_t n = 0;
            for (const auto& e : v) {
                if (!s.filters.match(e)) continue;
                char ts[16];
                local_hms(e.t_unix, ts, sizeof(ts));
                out << "[" << ts << "] ch" << e.channel << " " << openitc::mode_name(e.mode)
                    << "  type=" << std::hex << e.type << std::dec << " " << tname(e.type)
                    << "  " << summarize(e) << "\n";
                ++n;
            }
            s.status = "exported " + std::to_string(n) + " lines to " + name;
        }
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, term);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::BeginChild("termout", ImVec2(0, 0), ImGuiChildFlags_None, 0);
        {
            const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
            const std::vector<PacketEvent> view =
                s.paused ? s.frozen : s.log.snapshot(6000);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
            for (const auto& e : view) {
                if (!s.filters.match(e)) continue;
                draw_log_line(e);
            }
            ImGui::PopStyleVar();
            ImGui::TextColored(dim, ">");
            ImGui::SameLine(0, 4);
            ImVec4 blk = cyan;
            blk.w = 0.35f + 0.65f * std::fabs(std::sinf((float)ImGui::GetTime() * 4.0f));
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                cp, ImVec2(cp.x + 8, cp.y + ImGui::GetTextLineHeight()), u(blk));
            if (!s.paused && at_bottom) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    end_card();
}

}  // namespace openitc::gui
