// OpenITC GUI: Dear ImGui + GLFW dashboard (header + status chips + dual
// clocks; sidebar cards for session state, active WIUs and the latest beacon;
// a live beacon map; and a syntax-highlighted raw frame log).

#include <cmath>
#include <cstdio>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.h"
#include "theme.h"

namespace {
using openitc::gui::AppState;

void draw_toolbar(AppState& s) {
    using namespace openitc::gui::theme;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
    ImGui::BeginChild("toolbar", ImVec2(0, 38), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    if (s.running) {
        if (ImGui::Button("Stop")) s.stop();
    } else if (ImGui::Button("Start")) {
        s.start();
    }
    ImGui::SameLine();
    ImGui::Checkbox("record", &s.recording);

    ImGui::BeginDisabled(s.running);
    ImGui::SameLine();
    ImGui::TextColored(dim, "SDR");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(210);
    const char* cur =
        (s.ec.rtl.device_index >= 0 && s.ec.rtl.device_index < (int)s.sdr_devices.size())
            ? s.sdr_devices[s.ec.rtl.device_index].c_str()
            : "(no device)";
    if (ImGui::BeginCombo("##sdr", cur)) {
        for (int d = 0; d < (int)s.sdr_devices.size(); ++d) {
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "%d: %s", d, s.sdr_devices[d].c_str());
            if (ImGui::Selectable(lbl, d == s.ec.rtl.device_index))
                s.ec.rtl.device_index = d;
        }
        if (s.sdr_devices.empty()) ImGui::TextDisabled("no RTL-SDR devices found");
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextColored(dim, "gain");
    ImGui::SameLine();
    float gain_db = s.ec.rtl.gain_tenths_db / 10.0f;
    ImGui::BeginDisabled(s.ec.rtl.agc);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("##gain", &gain_db, 0.0f, 49.6f, "%.1f dB",
                           ImGuiSliderFlags_AlwaysClamp))
        s.ec.rtl.gain_tenths_db = (int)std::lround(gain_db * 10.0f);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("AGC", &s.ec.rtl.agc);

    ImGui::SameLine();
    ImGui::TextColored(dim, "ppm");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##ppm", &s.ec.rtl.ppm);
    ImGui::SameLine();
    ImGui::Checkbox("bias tee", &s.ec.rtl.bias_tee);

    ImGui::SameLine();
    ImGui::TextColored(dim, "chunk ms");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##chunk", &s.ec.chunk_ms);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("(restart to apply)");
    if (!s.status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(amber, "  %s", s.status.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
    GLFWwindow* window = glfwCreateWindow(1440, 900, "OpenITC", nullptr, nullptr);
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
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    openitc::gui::theme::apply();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlsl);

    AppState s;
    s.ec.rtl.sample_rate = 2304000;
    s.ec.rtl.center_freq = 221050000;
    s.ec.rtl.bias_tee = true;
    s.ec.rtl.gain_tenths_db = 420;
    s.ec.chunk_ms = 250;
    s.ec.channels = openitc::default_channels();
    const int ndev = openitc::rtl_device_count();
    for (int d = 0; d < ndev; ++d) s.sdr_devices.push_back(openitc::rtl_device_name(d));
    s.db_path = openitc::gui::default_db_path();
    if (!s.store.open(s.db_path)) s.status = "db not open: " + s.db_path;
    s.tiles.init(openitc::gui::tile_cache_dir(), "");

    const ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::Begin("OpenITC##root", nullptr, kRootFlags);
        {
            openitc::gui::draw_header(s);
            ImGui::Dummy(ImVec2(0, 2));
            draw_toolbar(s);

            ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            openitc::gui::draw_sidebar(s);
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            {
                const float H = ImGui::GetContentRegionAvail().y;
                const float mapH = H * 0.56f;
                openitc::gui::draw_map(s, ImVec2(0, mapH));
                openitc::gui::draw_terminal(s, ImVec2(0, 0));
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar();

        s.tiles.pump();

        ImGui::Render();
        int dw = 0, dh = 0;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        const ImVec4 bg = openitc::gui::theme::bg;
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (s.engine) s.engine->stop();
    s.store.close();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#if defined(_WIN32)
// Built as a Windows-subsystem app (no console window). The MinGW entry point
// is WinMain; forward to our main().
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(); }
#endif
