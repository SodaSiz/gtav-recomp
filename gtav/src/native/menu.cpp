#include "menu.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "touch_controls.h"
#include "controls.h"
#include "../bug_report.h"
#include "shader_compiler.h"

#include "paths.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define GTAV_MENU_PHONE 1
#else
#define GTAV_MENU_PHONE 0
#endif

namespace gtav::native::menu {

namespace {

struct Input {
  enum Type { kMove, kButton, kWheel, kKey } type;
  float x = 0, y = 0;
  int code = 0;
  bool down = false;
};

std::mutex g_input_mutex;
std::vector<Input> g_input;
std::atomic<bool> g_open{false};
bool g_controls_open = false;
std::mutex g_notice_mutex;
std::string g_notice;
std::chrono::steady_clock::time_point g_notice_until{};
double g_notice_seconds = 0;  // counted from the first frame that shows it (boots take a while)
std::string g_last_report;

ImGuiContext* g_context = nullptr;
bool g_font_uploaded = false;
float g_scale = 1.0f;

// Every setting the menu shows, by cvar name. Saved to gtav_settings.ini as name=value.
constexpr const char* kSettings[] = {
    "gtav_fps_limit",
    "gtav_vehicle_30fps",
    "gtav_native_vsync",
    "gtav_native_fps",
    "gtav_native_tile_penumbra_all",
    "gtav_native_async_pipelines",
    "gpu_skip_rendering_lag_ms",
    "gtav_native_exact_cache",
    "gtav_native_texture_recheck_frames",
    "gtav_native_record_workers",
    "gtav_touch_controls",
    "gtav_touch_scale",
#if GTAV_MENU_PHONE
    "gtav_native_cube_every",
#endif
#if !GTAV_MENU_PHONE
    "gtav_fullscreen",
    "gtav_native_render_scale",
    "mnk_mouse",
    "mnk_sensitivity",
    "audio_force_stereo",
#endif
    "user_language",
};

std::filesystem::path SettingsPath() { return ExeDir() / "gtav_settings.ini"; }

// Written into gtav_settings.ini; a file from an older version gets that version's corrections
// once when it is loaded (LoadSettings).
constexpr int kSettingsVersion = 2;

void SaveSettings() {
  std::ofstream out(SettingsPath());
  out << "# GTA V native renderer settings (Esc menu). name=value, applied over the command line.\n";
  out << "settings_version=" << kSettingsVersion << '\n';
  for (const char* name : kSettings) out << name << '=' << rex::cvar::GetFlagByName(name) << '\n';
}

bool GetBool(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
void SetBool(const char* name, bool value) { rex::cvar::SetFlagByName(name, value ? "true" : "false"); }

// Presets: the two ends of the trade-off. "Best image" draws everything exactly as soon as it
// appears (new pipelines are built on the spot, a short hitch the first time an effect shows up)
// with the per-pixel sun shadows; "Max performance" keeps the frame moving (new effects are
// skipped for a frame or two while they compile), lets the game skip shadow work per tile and
// lets the emulated GPU trail the game less.
// How far the emulated GPU trails the game's kicks (ms): 6 is the tested value (foliage stopped
// flickering while driving); 2 flickered. 4 is ~10% faster on Windows (88 -> 98 fps running
// around, 10-29) and no worse than 6 in a headless flicker-catch A/B; not yet checked driving.
constexpr const char* kSafeGpuLag = "6";
constexpr const char* kFastGpuLag = "4";

void ApplyPreset(bool best) {
#if GTAV_MENU_PHONE
  // Phones: the per-pixel sun tiles cost no measurable fps (same-run A/B on an Adreno 730), and
  // without them the game's per-tile shortcut shows gray 16x16 blocks at object edges. (The
  // iPhone took the desktop branch until 11-27: "Max performance" turned them off there.)
  SetBool("gtav_native_tile_penumbra_all", true);
#else
  SetBool("gtav_native_tile_penumbra_all", best);
#endif
  SetBool("gtav_native_async_pipelines", !best);
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // iPhone 13 mini, 11-27, same-run A/B standing: 6 | 4 ms = 21.8 | 22.3 fps, b ahead in 1 of 3
  // report lines. Nothing to gain there, so the tested value stays in both presets.
  rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", kSafeGpuLag);
#else
  rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", best ? kSafeGpuLag : kFastGpuLag);
#endif
#if GTAV_MENU_PHONE
  // Phones: "Best image" is also the steady one, locked at 30 fps with vsync (sustained play heats
  // the phone less and throttles later); "Max performance" runs uncapped.
  rex::cvar::SetFlagByName("gtav_fps_limit", best ? "30" : "0");
  SetBool("gtav_native_vsync", best);
#else
  if (!best) {
    rex::cvar::SetFlagByName("gtav_fps_limit", "0");
    SetBool("gtav_native_vsync", false);
  }
#endif
  SaveSettings();
}

void EnsureContext(uint32_t h) {
  if (g_context) return;
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  g_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  g_scale = std::max(1.0f, float(h) / 720.0f);
#if GTAV_MENU_PHONE
  g_scale *= 1.6f;  // a 5.4 to 6.7-inch screen driven by fingers: larger text and controls
#endif
  ImFontConfig font;
  font.SizePixels = 15.0f * g_scale;
  io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
#if GTAV_MENU_PHONE
  // Rows a finger can hit: taller controls with space between them, a slider grab and a scroll
  // bar wide enough to drag, and a margin around every control that still counts as a hit.
  style.FramePadding = ImVec2(8.0f, 10.0f);
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.GrabMinSize = 26.0f;
  style.ScrollbarSize = 26.0f;
  style.TouchExtraPadding = ImVec2(3.0f, 3.0f);
#endif
  style.ScaleAllSizes(g_scale);
  if (previous) ImGui::SetCurrentContext(previous);
}

void Push(const Input& in) {
  if (!g_open) return;
  std::lock_guard lock(g_input_mutex);
  if (g_input.size() < 1024) g_input.push_back(in);
}

ImGuiKey ToImGui(Key key) {
  switch (key) {
    case Key::kUp: return ImGuiKey_UpArrow;
    case Key::kDown: return ImGuiKey_DownArrow;
    case Key::kLeft: return ImGuiKey_LeftArrow;
    case Key::kRight: return ImGuiKey_RightArrow;
    case Key::kEnter: return ImGuiKey_Enter;
    case Key::kSpace: return ImGuiKey_Space;
    case Key::kTab: return ImGuiKey_Tab;
  }
  return ImGuiKey_None;
}

void Build(uint32_t fps, double frame_ms) {
  bool open = true;
#if GTAV_MENU_PHONE
  // Phones: one sheet under the overlay's Menu button, as wide as the screen allows, two columns
  // of finger-sized rows with the rarely used groups folded away, and a Close button instead of
  // the title bar's 3 mm box. (At the desktop layout a checkbox was 2 mm across on a 6.4-inch
  // screen.) It does not move: a finger on its background is not a drag.
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float top = touch::MenuButtonBottom(display.y) + display.y * 0.015f;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, top), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(display.x, display.y - top - 4.0f * g_scale));
  ImGui::SetNextWindowSize(ImVec2(std::min(display.x * 0.94f, 800.0f * g_scale), 0.0f), ImGuiCond_Always);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::Begin("Settings", &open, flags)) {
    ImGui::End();
    return;
  }
  {
    const float close_w = 110.0f * g_scale;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Settings      %u fps  (%.1f ms per frame)", fps, frame_ms);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - close_w);  // left of the scroll bar, if any
    if (ImGui::Button("Close", ImVec2(close_w, 0.0f))) open = false;
  }
#else
  ImGui::SetNextWindowPos(ImVec2(40.0f * g_scale, 40.0f * g_scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(520.0f * g_scale, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings (F1 / Esc / B to close)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ImGui::Text("%u fps  (%.1f ms per frame)", fps, frame_ms);
#endif
  ImGui::Separator();

  bool changed = false;
  auto check = [&](const char* label, const char* name, const char* help) {
    bool v = GetBool(name);
    if (ImGui::Checkbox(label, &v)) {
      SetBool(name, v);
      changed = true;
    }
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  };
  // Combos and sliders. Desktop: the label to the right of a control `width` wide. Phones: the
  // label on its own line and the control as wide as its column, so the whole row is the target.
  std::string wide_id;
  auto wide = [&](const char* label, float width) {
#if GTAV_MENU_PHONE
    (void)width;
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    wide_id = std::string("##") + label;
#else
    ImGui::SetNextItemWidth(width * g_scale);
    wide_id = label;
#endif
    return wide_id.c_str();
  };

  auto presets = [&] {
#if GTAV_MENU_PHONE
    // Two buttons as wide as the column: the presets are what a phone player changes most.
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Best image", ImVec2(half, 0.0f))) ApplyPreset(true);
    ImGui::SameLine();
    if (ImGui::Button("Max performance", ImVec2(half, 0.0f))) ApplyPreset(false);
#else
    ImGui::TextUnformatted("Preset");
    if (ImGui::Button("Best image")) ApplyPreset(true);
    ImGui::SameLine();
    if (ImGui::Button("Max performance")) ApplyPreset(false);
    ImGui::TextDisabled("Compare the fps above after switching.");
#endif
  };
  auto frame_rate = [&] {
    int limit = std::atoi(rex::cvar::GetFlagByName("gtav_fps_limit").c_str());
    int choice = limit == 30 ? 0 : limit == 60 ? 1 : limit <= 0 ? 2 : 3;
    const char* limits[] = {"30 (the game's own limiter)", "60", "Uncapped", "Custom (command line)"};
    if (ImGui::Combo(wide("Frame rate limit", 260.0f), &choice, limits, choice == 3 ? 4 : 3) && choice < 3) {
      rex::cvar::SetFlagByName("gtav_fps_limit", choice == 0 ? "30" : choice == 1 ? "60" : "0");
      changed = true;
    }
  };
#if !GTAV_MENU_PHONE
  auto display_settings = [&] {
    check("Fullscreen", "gtav_fullscreen", "Borderless fullscreen at the desktop resolution. F11 or Alt+Enter also toggle it.");
    // The game renders at 1280x720 on the Xbox; the native renderer can draw its 3D passes at a
    // multiple of that. Read once at start (render targets are made at that size).
    static const double kScales[] = {0.75, 1.0, 1.5, 2.0};
    static const char* kNames[] = {"960x540 (faster)", "1280x720 (Xbox)", "1920x1080", "2560x1440"};
    static const double started = std::atof(rex::cvar::GetFlagByName("gtav_native_render_scale").c_str());
    const double now = std::atof(rex::cvar::GetFlagByName("gtav_native_render_scale").c_str());
    int choice = 1;
    for (int i = 0; i < 4; i++)
      if (std::abs(now - kScales[i]) < 0.01) choice = i;
    if (ImGui::Combo(wide("Resolution", 260.0f), &choice, kNames, 4)) {
      rex::cvar::SetFlagByName("gtav_native_render_scale", std::to_string(kScales[choice]));
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("3D rendering resolution (the HUD and menus stay at the game's own size).\n"
                        "Takes effect the next time the game starts.");
    if (std::abs(now - started) > 0.01) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Restart the game to apply the new resolution.");
  };
#endif
  auto pacing = [&] {
    check(GTAV_MENU_PHONE ? "30 fps in vehicles" : "30 fps in vehicles (Xbox handling)", "gtav_vehicle_30fps",
          "Driving and flying are tuned for the Xbox's 30 fps: above it steering and handling get too fast.\n"
          "On: vehicles run at 30 fps, on foot the limit above applies.");
    check("VSync", "gtav_native_vsync", "Wait for the display refresh: no tearing, fps capped at the refresh rate.");
    check("FPS counter", "gtav_native_fps", "Frames per second in the top-left corner.");
  };
  auto accuracy = [&] {
    check("Per-pixel sun shadows", "gtav_native_tile_penumbra_all",
          "On: every screen tile computes sun shadows per pixel (no blocky shadows or car reflections).\n"
          "Off: the game's own per-tile shortcut, which goes blocky on this renderer (gray 16x16 blocks).");
    check("Compile new effects in the background", "gtav_native_async_pipelines",
          "On: no hitch when a new effect first appears; it is skipped for a frame or two instead.\n"
          "Off: it is drawn from its first frame, with a short hitch while it is built.");
    {
      bool fast = std::atoi(rex::cvar::GetFlagByName("gpu_skip_rendering_lag_ms").c_str()) < std::atoi(kSafeGpuLag);
      if (ImGui::Checkbox("Low GPU latency", &fast)) {
        rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", fast ? kFastGpuLag : kSafeGpuLag);
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the emulated GPU trails the game by 2 ms instead of 6 (about 20%% more fps).\n"
                          "Off: the tested setting; with On, watch foliage and distant meshes while driving.");
    }
#if GTAV_MENU_PHONE
    {
      // --gtav_native_cube_every: the reflection cube (6 faces, ~345 draws a frame) redrawn every
      // second frame. S23 FE, heat-capped, same-run A/B: +2-5% fps, GPU -1.0 to -1.7 ms a frame.
      bool half = std::atoi(rex::cvar::GetFlagByName("gtav_native_cube_every").c_str()) >= 2;
      if (ImGui::Checkbox("Reflections every other frame", &half)) {
        rex::cvar::SetFlagByName("gtav_native_cube_every", half ? "2" : "1");
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: reflections on cars and wet roads update at half the frame rate (2-5%% more fps).");
    }
#endif
    {
      // --gtav_native_record_workers (docs/HANDOFF_MT_RENDERER_2026-11-22.md). Live.
      int workers = std::atoi(rex::cvar::GetFlagByName("gtav_native_record_workers").c_str());
      workers = workers < 0 ? 0 : workers > 3 ? 3 : workers;
      const char* names[] = {"Off", "1", "2", "3"};
      if (ImGui::Combo(wide("Record threads (experimental)", 120.0f), &workers, names, 4)) {
        rex::cvar::SetFlagByName("gtav_native_record_workers", std::to_string(workers));
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Extra threads that turn each frame into GPU commands next to the renderer's own.\n"
                          "More fps where the renderer limits the frame rate (many draws, fast CPU);\n"
                          "none, or slightly less, where the game itself is the limit (phones).");
    }
  };
  auto textures = [&] {
    check("Exact texture checks", "gtav_native_exact_cache",
          "On: every texture is fully re-checked each frame: no stale gray blocks, but more CPU time.\n"
          "Off: large textures are spot-checked between full checks (faster).");
    int frames = std::atoi(rex::cvar::GetFlagByName("gtav_native_texture_recheck_frames").c_str());
    if (ImGui::SliderInt(wide("Full re-check every N frames", 200.0f), &frames, 1, 30)) {
      rex::cvar::SetFlagByName("gtav_native_texture_recheck_frames", std::to_string(frames));
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lower: textures the game streams in show up sooner (fewer gray blocks), more CPU.");
  };
  auto language = [&] {
    // The console language (XGetLanguage): the game's text and menus. Read when the game starts.
    static const char* kLanguages[] = {"English", "Japanese", "German", "French", "Spanish", "Italian", "Korean",
                                       "Chinese (Traditional)", "Portuguese", "Chinese (Simplified)", "Polish",
                                       "Russian"};
    static const int started = std::atoi(rex::cvar::GetFlagByName("user_language").c_str());
    const int now = std::atoi(rex::cvar::GetFlagByName("user_language").c_str());
    int choice = now >= 1 && now <= 12 ? now - 1 : 0;
    if (ImGui::Combo(wide("Language", 260.0f), &choice, kLanguages, 12)) {
      rex::cvar::SetFlagByName("user_language", std::to_string(choice + 1));
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Text and menus (speech stays English). Applied at the next start.");
    if (now != started) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Restart the game to apply the new language.");
  };
  auto shaders = [&] {
    // What the renderer has: shaders the game made, which have SPIR-V, which failed or are missing.
    static ShaderStats stats;
    static int refresh = 0;
    if (refresh-- <= 0) stats = GetShaderStats(), refresh = 30;
    const size_t bad = stats.failed.size() + stats.missing_at_draw.size();
    if (ImGui::CollapsingHeader(bad ? "Shaders (some missing)###shaders" : "Shaders###shaders")) {
      ImGui::Text("%u of %u game shaders ready", stats.ready, stats.created);
      if (stats.pending) ImGui::Text("%u being prepared now", stats.pending);
      ImGui::Text("%u translated on this computer this run", stats.compiled_this_run);
      if (!stats.can_compile)
        ImGui::TextWrapped("This device cannot translate shaders: missing ones are saved for the PC "
                           "(rebuild the phone package there).");
      auto list = [&](const char* title, const std::vector<std::string>& names, const char* help) {
        if (names.empty()) return;
        if (ImGui::TreeNode(title, "%s: %zu", title, names.size())) {
          ImGui::TextDisabled("%s", help);
          for (const auto& n : names) ImGui::BulletText("%s", n.c_str());
          ImGui::TreePop();
        }
      };
      list("Failed to translate", stats.failed, "Draws using these are skipped. Please send a bug report.");
      list("Drawn without a shader", stats.missing_at_draw,
           "The game drew with these before they were ready (or they failed): objects can be missing.");
      if (!bad) ImGui::TextDisabled("Nothing missing.");
    }
  };
  auto bug_report = [&] {
    // Phones: as wide as its column.
    if (ImGui::Button("Create bug report", ImVec2(GTAV_MENU_PHONE ? -FLT_MIN : 0.0f, 0.0f))) {
      g_last_report = gtav::bug_report::Create("settings menu");
      if (!g_last_report.empty()) gtav::bug_report::ShowInFolder(g_last_report);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Zips the game log, crash info, your settings and system info into one file.\n"
                        "Attach it to an issue on github.com/andrewnakas/gtav-recomp/issues.");
    if (!g_last_report.empty()) ImGui::TextWrapped("Saved: %s", g_last_report.c_str());
  };

#if GTAV_MENU_PHONE
  ImGui::Columns(2, "phone", false);
  presets();
  frame_rate();
  pacing();
  ImGui::NextColumn();
  check("Touch controls", "gtav_touch_controls", "The on-screen gamepad (hidden while a controller is connected).");
  {
    // --gtav_touch_scale: the on-screen gamepad's buttons and sticks. Live.
    float size = float(std::atof(rex::cvar::GetFlagByName("gtav_touch_scale").c_str()));
    if (ImGui::SliderFloat(wide("Touch button size", 200.0f), &size, 0.7f, 1.4f, "%.2f")) {
      rex::cvar::SetFlagByName("gtav_touch_scale", std::to_string(size));
      changed = true;
    }
  }
  language();
  bug_report();
  ImGui::Columns(1);
  ImGui::Separator();
  if (ImGui::CollapsingHeader("Advanced")) {
    accuracy();
    textures();
    shaders();
    ImGui::TextDisabled("Controller: RB + Back opens and closes this menu; D-pad + A to change, B to close.");
  }
#else
  presets();
  ImGui::Separator();
  frame_rate();
  display_settings();
  pacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Accuracy");
  accuracy();
  ImGui::Separator();
  ImGui::TextUnformatted("Textures");
  textures();
  ImGui::Separator();
  ImGui::TextUnformatted("Keyboard and mouse");
  if (ImGui::Button("Controls (keys, mouse, controller)...")) g_controls_open = true;
  check("Mouse look", "mnk_mouse",
        "The mouse turns the camera (right stick); the pointer is locked to the game window.\n"
        "F1 (this menu) releases it.");
  {
    float sens = float(std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str()));
    ImGui::SetNextItemWidth(200.0f * g_scale);
    if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.1f, 5.0f, "%.2f")) {
      rex::cvar::SetFlagByName("mnk_sensitivity", std::to_string(sens));
      changed = true;
    }
  }
  ImGui::TextDisabled("GTA V PC default keys: they switch with what you do (on foot, driving,\n"
                      "flying, parachute, pause menu). Mouse wheel / 1-9: weapons, F5-F7: characters.\n"
                      "F1 opens this menu, Esc is the game's pause.");
  ImGui::Separator();
  ImGui::TextUnformatted("Sound");
  check("Stereo mix", "audio_force_stereo",
        "On: the game's 5.1 mix is folded to stereo here (dialogue from the centre channel kept).\n"
        "Off: 5.1 goes to the device when it reports more than 2 channels (real surround systems).\n"
        "Takes effect the next time the game starts.");
  ImGui::Separator();
  language();
  ImGui::Separator();
  shaders();
  ImGui::Separator();
  ImGui::TextUnformatted("Problems?");
  bug_report();
  ImGui::Separator();
  check("Touch controls", "gtav_touch_controls", "The on-screen gamepad (hidden while a controller is connected).");
  ImGui::TextDisabled("Controller: RB + Back opens/closes this menu; D-pad + A to change, B to close.");
#endif
  if (changed) SaveSettings();
  ImGui::End();
#if !GTAV_MENU_PHONE
  if (g_controls_open) controls::DrawWindow(&g_controls_open, g_scale);
#endif
  if (!open) g_open = false;
}

}  // namespace

void OnEscape() {
  bool was_open = g_open.exchange(!g_open.load());
  if (!was_open) {
    std::lock_guard lock(g_input_mutex);
    g_input.clear();
  }
}
bool IsOpen() { return g_open.load(); }
void ToggleFullscreen() {
  SetBool("gtav_fullscreen", !GetBool("gtav_fullscreen"));
  if (!std::getenv("GTAV_IGNORE_SETTINGS")) SaveSettings();
}
void OnMouseMove(float x, float y) { Push({Input::kMove, x, y}); }
void OnMouseButton(int button, bool down) { Push({Input::kButton, 0, 0, button, down}); }
void OnMouseWheel(float dy) { Push({Input::kWheel, 0, dy}); }
void OnKey(Key key, bool down) { Push({Input::kKey, 0, 0, int(key), down}); }

void TestSet(const std::string& name, const std::string& value) {
  REXLOG_INFO("[native] menu test: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
  if (std::getenv("GTAV_IGNORE_SETTINGS")) return;  // benchmark runs leave gtav_settings.ini alone
  SaveSettings();
  REXLOG_INFO("[native] menu test: saved");
}

// One line with the values the run actually uses (command line + ini), so a log is reproducible.
static void LogEffectiveSettings(bool ini_ignored) {
  std::string all;
  for (const char* name : kSettings) all += fmt::format(" {}={}", name, rex::cvar::GetFlagByName(name));
  REXLOG_INFO("[native] settings{}:{}", ini_ignored ? " (gtav_settings.ini ignored)" : "", all);
}

void LoadSettings() {
  // GTAV_IGNORE_SETTINGS=1: command-line settings only (benchmark runs).
  if (std::getenv("GTAV_IGNORE_SETTINGS")) {
    LogEffectiveSettings(true);
    return;
  }
  std::ifstream in(SettingsPath());
  int version = 0;
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string name = line.substr(0, eq), value = line.substr(eq + 1);
      if (name == "settings_version") {
        version = std::atoi(value.c_str());
        continue;
      }
      if (std::find_if(std::begin(kSettings), std::end(kSettings), [&](const char* s) { return name == s; }) ==
          std::end(kSettings))
        continue;
      if (rex::cvar::SetFlagByName(name, value)) REXLOG_INFO("[native] setting {} = {} (gtav_settings.ini)", name, value);
    }
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // Version 2 (11-27): the iPhone's "Max performance" button used to turn the per-pixel sun
    // shadows off (the desktop branch of ApplyPreset); on this renderer that is the gray 16x16
    // blocks at object edges, flickering as the camera moves. Saved files from then get them back
    // once; the checkbox still turns them off for whoever wants that.
    if (version < 2 && !GetBool("gtav_native_tile_penumbra_all")) {
      SetBool("gtav_native_tile_penumbra_all", true);
      REXLOG_INFO("[native] settings from before version 2: per-pixel sun shadows turned back on");
    }
    // The same button set the emulated GPU's lag to 4 ms: no faster on the iPhone (ApplyPreset),
    // and 6 is the value the flicker tests were made with.
    if (version < 2 && rex::cvar::GetFlagByName("gpu_skip_rendering_lag_ms") == kFastGpuLag) {
      rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", kSafeGpuLag);
      REXLOG_INFO("[native] settings from before version 2: GPU lag back to {} ms", kSafeGpuLag);
    }
#endif
    if (version < kSettingsVersion) {
      in.close();
      SaveSettings();
    }
  }
  LogEffectiveSettings(false);
}

void SetNotice(const std::string& text, double seconds) {
  std::lock_guard lock(g_notice_mutex);
  g_notice = text;
  g_notice_seconds = seconds;
  g_notice_until = {};
}

ImDrawData* Frame(uint32_t w, uint32_t h, uint32_t fps, double frame_ms, uint32_t pending_shaders) {
  const bool touch_overlay = touch::Visible();
  std::string notice;
  {
    std::lock_guard lock(g_notice_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_notice_seconds > 0) {
      g_notice_until = now + std::chrono::milliseconds(int64_t(g_notice_seconds * 1000));
      g_notice_seconds = 0;
      REXLOG_INFO("[native] on-screen notice shown: {}", g_notice);
    }
    if (now < g_notice_until) notice = g_notice;
  }
  if (!g_open && !touch_overlay && !pending_shaders && notice.empty()) return nullptr;
  EnsureContext(h);
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(w), float(h));
  io.DeltaTime = 1.0f / 60.0f;
  {
    std::lock_guard lock(g_input_mutex);
    for (const auto& in : g_input) {
      switch (in.type) {
        case Input::kMove: io.AddMousePosEvent(in.x, in.y); break;
        case Input::kButton: io.AddMouseButtonEvent(in.code, in.down); break;
        case Input::kWheel: io.AddMouseWheelEvent(0.0f, in.y); break;
        case Input::kKey: io.AddKeyEvent(ToImGui(Key(in.code)), in.down); break;
      }
    }
    g_input.clear();
  }
  if (!g_font_uploaded) {  // legacy atlas: the backend uploads it before the first draw
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
  }
  ImGui::NewFrame();
  if (g_open) Build(fps, frame_ms);
  if (touch_overlay) touch::Draw(ImGui::GetForegroundDrawList(), float(w), float(h));
  if (!notice.empty()) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float pad = 10.0f * g_scale;
    const ImVec2 size = ImGui::CalcTextSize(notice.c_str());
    const ImVec2 at((float(w) - size.x) * 0.5f, float(h) - size.y - 6 * pad);
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + size.x + pad, at.y + size.y + pad),
                      IM_COL32(0, 0, 0, 200), 6.0f * g_scale);
    dl->AddText(at, IM_COL32(255, 220, 120, 255), notice.c_str());
  }
  if (pending_shaders) {
    // First launch: the game's shaders are translated on this machine while it boots; the picture
    // can stay dark until they are done. Later launches reuse them. Also shown while the picture
    // waits for pipelines (after an update the graphics driver compiles them all again).
    char text[128];
    std::snprintf(text, sizeof(text), "Preparing shaders: %u left", pending_shaders);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 8.0f * g_scale;
    const ImVec2 at(float(w) - size.x - 3 * pad, 3 * pad);
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + size.x + pad, at.y + size.y + pad),
                      IM_COL32(0, 0, 0, 170), 4.0f * g_scale);
    dl->AddText(at, IM_COL32(255, 255, 255, 255), text);
  }
  ImGui::Render();
  ImDrawData* data = ImGui::GetDrawData();
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return data;
}

bool TakeFontAtlas(const uint8_t** pixels, int* width, int* height) {
  if (g_font_uploaded || !g_context) return false;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  unsigned char* p;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&p, width, height);
  *pixels = p;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return true;
}

void SetFontTexture(uint32_t descriptor) {
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGui::GetIO().Fonts->SetTexID(ImTextureID(descriptor));
  g_font_uploaded = true;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
}

}  // namespace gtav::native::menu
