// OpenITC GUI shell (M8): Dear ImGui + GLFW. Starts/stops the monitor engine
// and shows a live status line, a clock preview, and a live packet log. The
// full dashboards (map, WIU board, filters) land in later milestones.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "engine.h"
#include "model.h"
#include "repository.h"

namespace {

using openitc::PacketEvent;

// Bounded, thread-safe recent-packet log fed by the engine thread.
class PacketLog {
public:
    void add(const PacketEvent& e) {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push_back(e);
        if (items_.size() > kCap) items_.pop_front();
    }
    std::vector<PacketEvent> snapshot(size_t max_items) const {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t n = items_.size() < max_items ? items_.size() : max_items;
        return std::vector<PacketEvent>(items_.end() - n, items_.end());
    }

private:
    static constexpr size_t kCap = 20000;
    mutable std::mutex mu_;
    std::deque<PacketEvent> items_;
};

std::string hexs(const std::vector<uint8_t>& d, size_t from, size_t maxn) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = from; i < d.size() && i < from + maxn; ++i) {
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 0xf]);
    }
    return s;
}

// One-line decoded summary for the packet-log column.
std::string summarize(const PacketEvent& e) {
    char buf[256];
    const std::vector<uint8_t>& p = e.payload;
    if (e.type == 0x02 && e.beacon.valid) {
        std::snprintf(buf, sizeof(buf),
                      "id=%06x lat=%.5f lon=%.5f ch=%d rate=%d util=%d", e.beacon.radio_id,
                      e.beacon.lat, e.beacon.lon, e.beacon.channel, e.beacon.rate,
                      e.beacon.util);
    } else if (e.type == 0x06 && e.time.valid) {
        std::snprintf(buf, sizeof(buf), "TID=%06x  %04d-%02d-%02d %02d:%02d:%02d UTC",
                      e.time.tid, e.time.year, e.time.month, e.time.day, e.time.hour,
                      e.time.minute, e.time.second);
    } else if (e.type == 0x05 && e.cntl.valid) {
        std::snprintf(buf, sizeof(buf), "TID=%06x seq=%d data=%s", e.cntl.tid, e.cntl.seq,
                      hexs(e.cntl.data, 0, 12).c_str());
    } else if ((e.type == 0x33 || e.type == 0x34) && e.wiu.valid) {
        std::snprintf(buf, sizeof(buf), "WIU=%06x data=%s", e.wiu.wiu_id,
                      hexs(e.wiu.indicators, 0, 16).c_str());
    } else if (p.size() >= 3) {
        std::snprintf(buf, sizeof(buf), "TID=%06x  %s", openitc::tid24(p),
                      hexs(p, 3, 16).c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%s", hexs(p, 0, 16).c_str());
    }
    return buf;
}

void local_hms(double t_unix, char* out, size_t n) {
    std::time_t t = (std::time_t)t_unix;
    std::strftime(out, n, "%H:%M:%S", std::localtime(&t));
}

}  // namespace

int main() {
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    const char* kGlsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1360, 860, "OpenITC", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlsl);

    openitc::EngineConfig ec;
    ec.rtl.sample_rate = 2304000;
    ec.rtl.center_freq = 221050000;
    ec.rtl.bias_tee = true;
    ec.rtl.gain_tenths_db = 420;
    ec.chunk_ms = 250;

    openitc::Repository repo;
    PacketLog log;
    std::unique_ptr<openitc::MonitorEngine> engine;
    bool running = false;
    char status_msg[256] = "";

    auto subscribe = [&](openitc::MonitorEngine& eng) {
        eng.set_callback([&](const PacketEvent& e) {
            repo.observe(e);
            log.add(e);
        });
    };
    auto do_start = [&]() {
        engine = std::make_unique<openitc::MonitorEngine>(ec);
        subscribe(*engine);
        if (engine->start()) {
            running = true;
            status_msg[0] = '\0';
        } else {
            std::snprintf(status_msg, sizeof(status_msg), "start failed: %s",
                          engine->last_error().c_str());
        }
    };
    auto do_stop = [&]() {
        if (engine) engine->stop();
        running = false;
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("OpenITC##root", nullptr,
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Device")) {
                if (!running) {
                    if (ImGui::MenuItem("Start")) do_start();
                } else if (ImGui::MenuItem("Stop")) {
                    do_stop();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ---- controls (editable only while stopped) ----
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("gain (dBx10)##gain", &ec.rtl.gain_tenths_db);
        ImGui::SameLine();
        ImGui::Checkbox("bias tee", &ec.rtl.bias_tee);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("chunk (ms)##chunk", &ec.chunk_ms);
        ImGui::SameLine();
        ImGui::TextDisabled("(restart to apply)");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (running) {
            if (ImGui::Button("Stop")) do_stop();
        } else if (ImGui::Button("Start")) {
            do_start();
        }

        const openitc::EngineStats st = engine ? engine->stats() : openitc::EngineStats{};
        const std::string devname =
            engine ? engine->device_name() : openitc::rtl_device_name(ec.rtl.device_index);
        ImGui::Separator();
        ImGui::Text("%s  |  device %d (%s)  |  %.3f MHz  rate %u  gain %s  bias_tee %s",
                    running ? "RUNNING" : "stopped", ec.rtl.device_index, devname.c_str(),
                    ec.rtl.center_freq / 1e6, ec.rtl.sample_rate,
                    ec.rtl.agc ? "AGC"
                               : (std::to_string(ec.rtl.gain_tenths_db / 10.0) + " dB").c_str(),
                    ec.rtl.bias_tee ? "on" : "off");
        ImGui::Text("packets %llu  (%.0f/min)  |  chunks %llu  |  sync hits %llu", st.packets,
                    repo.packets_per_min(), st.chunks, st.sync_hits);
        if (status_msg[0]) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", status_msg);

        // ---- clock preview (ITC vs local) ----
        {
            const std::vector<openitc::TimeRecord> times = repo.times();
            if (!times.empty()) {
                openitc::TimeRecord best = times[0];
                for (const auto& r : times)
                    if (r.last_seen > best.last_seen) best = r;
                char loc[16];
                local_hms((double)std::time(nullptr), loc, sizeof(loc));
                ImGui::Separator();
                ImGui::Text("ITC (TID %06x): %04d-%02d-%02d %02d:%02d:%02d UTC",
                            best.info.tid, best.info.year, best.info.month, best.info.day,
                            best.info.hour, best.info.minute, best.info.second);
                ImGui::SameLine();
                ImGui::Text(" | local %s | drift %+.1f s", loc, best.drift);
            }
        }

        ImGui::Separator();
        // ---- packet log ----
        const std::vector<PacketEvent> rows = log.snapshot(2000);
        if (ImGui::BeginTable("packets", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("time");
            ImGui::TableSetupColumn("ch");
            ImGui::TableSetupColumn("mode");
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("len");
            ImGui::TableSetupColumn("summary", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin((int)rows.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const PacketEvent& e = rows[(size_t)i];
                    char ts[16];
                    local_hms(e.t_unix, ts, sizeof(ts));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ts);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", e.channel);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(openitc::mode_name(e.mode));
                    ImGui::TableNextColumn();
                    ImGui::Text("%02x %s", e.type, openitc::type_name(e.type));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", (int)e.payload.size());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(summarize(e).c_str());
                }
            }
            ImGui::EndTable();
        }

        ImGui::End();

        ImGui::Render();
        int dw = 0, dh = 0;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (engine) engine->stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
