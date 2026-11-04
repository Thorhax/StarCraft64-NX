#pragma once
#include "input/devices.hpp"
#include "input/gestures.hpp"
#include "json/json.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Host input routing, controller merge, recipes and the overlay model. These
// classes are engine-independent: the SDL shell (app.cpp) feeds them events
// and draws their overlay; synthetic tests drive them through NativeApi mocks.
//
// Coordinates follow the former presentation contract: pointer `x`/`y` are in
// the 960x720 design space (stretched over the window), `screen_x`/`screen_y`
// are physical window pixels; both have a bottom-left origin.
namespace sc64::shell {
constexpr double design_width = 960, design_height = 720;

struct Touch {
    std::string id;
    bool pressed = false, released = false;
    double x = 0, y = 0;
    std::optional<double> screen_x, screen_y;
};
// A bound action event. `id` is empty for plain pointer motion.
struct InputEvent {
    std::optional<std::string> id;
    bool pressed = false, released = false;
    std::optional<double> value;
    std::optional<int> gamepad;
    std::optional<double> x, y, screen_x, screen_y;
    std::vector<Touch> touch;
    // A mouse kept inside the screen (the Switch's USB mouse): not a touch alias, and
    // its position at the image edge scrolls the map as on PC.
    bool confined_mouse = false;
};
enum class WindowEvent { focus_lost, focus_gained, iconified, restored, resized };

// Configuration values: command-line `--config=sc64.key=value` pairs or
// Android asset/intent arguments, all keyed "sc64.<name>".
class Config {
public:
    void set(std::string key, std::string value) { values_[std::move(key)] = std::move(value); }
    std::string string(const std::string& key, const std::string& fallback = "") const;
    double number(const std::string& key, double fallback = 0) const;
    // Accepts `--config=key=value` arguments; other arguments are ignored.
    void parse_arguments(int argc, const char* const* argv);
    // Accepts `key = value` lines; `#` starts a comment.
    void parse_text(const std::string& text);
private:
    std::map<std::string, std::string> values_;
};

// The native session surface the scripts call (formerly the `sc64` Lua module).
class NativeApi {
public:
    virtual ~NativeApi() = default;
    virtual input::Info info() = 0;
    // The complete telemetry snapshot (formerly the whole get_info table).
    virtual nlohmann::json telemetry() = 0;
    virtual nlohmann::json take_audio_timing() = 0;
    // Returns an error message, or nothing on success.
    virtual std::optional<std::string> load_rom(const std::string& path) = 0;
    virtual std::optional<std::string> load_bundled_rom() = 0;
    // Starts an asynchronous ROM choice; its result arrives through poll_rom().
    virtual std::optional<std::string> choose_rom() = 0;
    // Nothing pending, or a completed import: an empty string means success.
    virtual std::optional<std::string> poll_rom() = 0;
    virtual std::optional<std::string> stored_rom() = 0;
    virtual void pause(bool paused) = 0;
    virtual void set_pad(int player, unsigned buttons, double x, double y) = 0;
    virtual bool navigate_minimap(double x, double y, double context, double field) = 0;
    virtual void set_cursor_hidden(const std::optional<std::array<double, 4>>& region) = 0;
    virtual void vibrate(int milliseconds, int amplitude) = 0;
    virtual void set_haptics(bool enabled) = 0;
    virtual void exit(int code) = 0;
    virtual std::pair<double, double> window_size() = 0;
    virtual void log(const std::string& line) = 0;
};

// Durable storage of the diagnostic touch-pad layout.
class LayoutStore {
public:
    virtual ~LayoutStore() = default;
    // Throws when a present file cannot be read.
    virtual std::optional<nlohmann::json> load() = 0;
    // Returns an error message, or nothing once the layout is published.
    virtual std::optional<std::string> save(const nlohmann::json& layout) = 0;
};
class FileLayoutStore final : public LayoutStore {
public:
    explicit FileLayoutStore(std::filesystem::path path) : path_(std::move(path)) {}
    std::optional<nlohmann::json> load() override;
    std::optional<std::string> save(const nlohmann::json& layout) override;
private:
    std::filesystem::path path_;
};

// State shared by the host and overlay scripts.
struct Shared {
    input::GestureState gestures;
    input::touch_pad::State pad;
    std::string message = "StarCraft 64\n\nClick / tap or press O to choose your NTSC-U ROM dump.\n\n"
        "Enter: Start   X: A   C: B   Z: trigger\nArrows: D-pad   WASD: analog stick\n"
        "IJKL: C-buttons   Q/E: L/R\nP: pause";
    bool user_paused = false;
};

nlohmann::json to_json(const input::UiItem& item);
nlohmann::json to_json(const input::Info& info);
// Numbers print as integers when integral, like the former Lua encoder.
nlohmann::json number(double value);
nlohmann::json touch_layout_json(const input::touch_pad::Layout& layout);
input::touch_pad::Layout touch_layout_from(const std::optional<nlohmann::json>& saved);

// Controller merge, ROM startup, recipes, telemetry (former host.script).
class HostScript {
public:
    HostScript(Shared& shared, NativeApi& native, const Config& config);
    void init();
    void update(double dt);
    void on_input(const std::string& id, const InputEvent& action);
    void window_event(WindowEvent event);
    void final();
    struct RecipeItem {
        double field = 0;
        std::optional<double> duration;
        std::optional<std::string> key, when_ui;
        std::optional<double> when_token;
        std::optional<std::array<nlohmann::json, 4>> pointer;
        bool touch = false, right = false, auto_pick = false;
        std::optional<std::array<double, 2>> hover, ui_click, point;
        std::optional<std::array<double, 4>> ui_swipe;
        std::optional<std::tuple<std::string, double, double>> pan;
        std::optional<double> buttons, x, y;
        int player = 1;
        bool sent = false, released = false;
        std::optional<double> release_field;
    };
    std::vector<RecipeItem> recipe;
    double elapsed = 0;
    bool paused = false;
private:
    void set_user_pause(bool value);
    bool debug() const;
    Shared& shared_;
    NativeApi& native_;
    const Config& config_;
    input::pad_state::State pads_;
    input::haptics::State haptics_;
    double next_log_ = 0;
    bool test_paused_ = false, test_resumed_ = false;
    bool ui_target_log_ = false;
    std::optional<std::string> ui_selection_log_;
    std::optional<std::pair<double, double>> minimap_log_;
    std::map<int, input::PadOutput> pad_log_;
};

// One drawable overlay element in design coordinates (centre, size).
struct OverlayNode {
    enum class Kind { box, text } kind = Kind::box;
    bool enabled = true;
    double x = 0, y = 0, width = 1, height = 1, rotation = 0, scale = 1;
    std::array<double, 4> color{1, 1, 1, 1};
    std::string text;
    bool line_break = false;
};

// Diagnostic pad, hint outline, fallback panel, corner buttons (former
// touch.gui_script). Nodes are created once; creation order is draw order.
class GuiScript {
public:
    GuiScript(Shared& shared, NativeApi& native, const Config& config, LayoutStore& store);
    void init();
    void update(double dt);
    bool on_input(const std::optional<std::string>& id, const InputEvent& action);
    void final();
    std::vector<OverlayNode> nodes;
    std::string notice;
    double notice_time = 0;
    static constexpr std::size_t max_nodes = 128;
private:
    bool save();
    bool contact(const std::optional<std::string>& id, const std::string& key, const InputEvent& a, bool is_touch,
                 const input::Info& info);
    bool pad_pointer(const std::string& key, const InputEvent& a, const input::Info& info);
    std::size_t node(OverlayNode value);
    void place(std::size_t index, double x, double y, double width, double height);
    void update_buttons(std::optional<std::string_view> active, const input::Info& info,
                        const std::optional<input::viewport::Rect>& rect, double w, double h, double sw, double sh);
    Shared& shared_;
    NativeApi& native_;
    const Config& config_;
    LayoutStore& store_;
    struct PadNodes { std::size_t box, text; };
    std::map<std::string, PadNodes, std::less<>> pad_nodes_;
    std::size_t hint_ = 0;
    std::array<std::size_t, 4> edges_{};
    std::vector<PadNodes> commands_;
    struct ButtonNodes { std::size_t frame, fill; std::vector<std::size_t> glyph; };
    std::map<std::string, ButtonNodes, std::less<>> buttons_;
};

// Status, error and pause text (former status.gui_script).
class StatusScript {
public:
    StatusScript(Shared& shared, NativeApi& native) : shared_(shared), native_(native) {}
    void update();
    OverlayNode node{OverlayNode::Kind::text, true, 480, 360, 840, 600, 0, 1, {1, 1, 1, 1}, "", true};
private:
    Shared& shared_;
    NativeApi& native_;
};
}
