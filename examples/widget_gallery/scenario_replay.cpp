#include "scenario_replay.hpp"

#include "gallery.hpp"

#include <wui/wui.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace wui;

Error failure(std::string message) {
    return Error{ErrorCode::invalid_frame, std::move(message)};
}

Status expect(bool condition, const std::string& message) {
    if (condition) return std::nullopt;
    return failure(message);
}

bool inside(Rect item, Rect bounds) {
    constexpr float slack = 0.5f;
    return item.x >= bounds.x - slack && item.y >= bounds.y - slack &&
        item.x + item.w <= bounds.x + bounds.w + slack &&
        item.y + item.h <= bounds.y + bounds.h + slack;
}

// Integer client pixels, as a native host would deliver them.
struct Point {
    long x = 0, y = 0;
};

constexpr std::array<const char*, 3> kSections{"Appearance", "Controls", "Diagnostics"};
constexpr std::array<const char*, 3> kMenu{"Low", "Balanced", "High"};
constexpr std::array<float, 4> kScales{1.0f, 1.25f, 1.5f, 2.0f};
constexpr std::array<const char*, 4> kScaleNames{"scale-100", "scale-125", "scale-150", "scale-200"};

class Replay {
public:
    explicit Replay(const ReplayServices& services) : services_(services) {}

    Status run() {
        auto icon = make_gallery_icon(*services_.textures);
        if (auto* error = std::get_if<Error>(&icon)) return *error;
        icon_ = std::get<TextureHandle>(icon);
        if (auto error = scale_set()) return error;
        if (auto error = scenario_tabs()) return error;
        if (auto error = scenario_choices()) return error;
        if (auto error = scenario_dropdown()) return error;
        if (auto error = scenario_slider()) return error;
        if (auto error = scenario_keyboard()) return error;
        if (auto error = scenario_text()) return error;
        if (auto error = scenario_matrix()) return error;
        if (auto error = scenario_states()) return error;
        if (auto error = scenario_atlas()) return error;
        if (auto error = scenario_packets()) return error;
        return extras();
    }

    ReplayResult take() { return std::move(result_); }

private:
    // ---- host ------------------------------------------------------------
    Rect viewport() const {
        return {0, 0, static_cast<float>(width_) / scale_, static_cast<float>(height_) / scale_};
    }

    void resize(unsigned width, unsigned height) {
        width_ = width;
        height_ = height;
    }

    Status reset() {
        ContextDesc desc;
        desc.theme = Theme::dark();
        desc.textures = services_.textures;
        desc.text = services_.text;
        last_.reset();
        events_.clear();
        context_.reset();
        auto created = Context::create(std::move(desc));
        if (auto* error = std::get_if<Error>(&created)) return *error;
        context_ = std::get<std::unique_ptr<Context>>(std::move(created));
        state_ = GalleryState{};
        prepare_gallery_model(state_);
        state_.icon = icon_;
        applied_theme_.reset();
        return std::nullopt;
    }

    Status apply_theme() {
        const std::array<int, 3> key{state_.density, state_.accent_choice, state_.reduced_motion ? 1 : 0};
        if (applied_theme_ == key) return std::nullopt;
        auto theme = gallery_theme(state_);
        if (auto* error = std::get_if<Error>(&theme)) return *error;
        if (auto error = context_->set_theme(std::get<Theme>(theme))) return error;
        applied_theme_ = key;
        return std::nullopt;
    }

    Status frames(int count) {
        for (int i = 0; i < count; ++i) {
            if (auto error = apply_theme()) return error;
            FrameInput input{viewport(), scale_, 1.0 / 60.0, {}};
            input.events.swap(events_);
            state_.viewport_height = input.viewport.h;
            auto begun = context_->begin_frame(input);
            if (auto* error = std::get_if<Error>(&begun)) return *error;
            Frame frame = std::get<Frame>(std::move(begun));
            WidgetOptions options;
            options.enabled = !disabled_;
            build_gallery(frame, state_, options);
            auto finished = frame.finish();
            if (auto* error = std::get_if<Error>(&finished)) return *error;
            last_ = std::get<DrawPacket>(std::move(finished));
            if (!last_) return failure("frame produced a null packet");
            observe_gallery_frame(state_, *last_, context_->diagnostics());
        }
        return std::nullopt;
    }

    Status settle(int count = 12) { return frames(count); }

    Vec2 logical(Point point) const {
        return {static_cast<float>(point.x) / scale_, static_cast<float>(point.y) / scale_};
    }
    void pointer(EventKind kind, Point point) {
        InputEvent event{kind};
        event.position = logical(point);
        events_.push_back(std::move(event));
    }
    void move(Point p) { pointer(EventKind::pointer_move, p); }
    void down(Point p) { pointer(EventKind::pointer_down, p); }
    void up(Point p) { pointer(EventKind::pointer_up, p); }
    void focus_lost() { events_.push_back(InputEvent{EventKind::focus_lost}); }
    void key(Key key, bool is_down, bool shift = false, bool control = false, int repeats = 1,
             bool previous_down = false) {
        for (int i = 0; i < repeats; ++i) {
            InputEvent event{is_down ? EventKind::key_down : EventKind::key_up};
            event.key = key;
            event.shift = shift;
            event.control = control;
            event.repeat = is_down && (i != 0 || previous_down);
            events_.push_back(std::move(event));
        }
    }
    Status key_frame(Key k, bool is_down, bool repeat = false) {
        key(k, is_down, false, false, repeat ? 2 : 1, repeat);
        return frames(1);
    }
    Status press(Key k) {
        if (auto error = key_frame(k, true)) return error;
        return key_frame(k, false);
    }
    Status chord(bool shift, bool control, Key k) {
        key(k, true, shift, control);
        key(k, false, shift, control);
        return frames(1);
    }
    // One text event per code point, as WM_CHAR delivers BMP characters.
    Status type(std::string_view utf8) {
        std::size_t i = 0;
        while (i < utf8.size()) {
            const auto lead = static_cast<unsigned char>(utf8[i]);
            const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            InputEvent event{EventKind::text};
            event.text = std::string(utf8.substr(i, length));
            events_.push_back(std::move(event));
            i += length;
        }
        return frames(1);
    }
    Status wheel(Point point, int notches) {
        InputEvent event{EventKind::wheel};
        event.position = logical(point);
        event.wheel = static_cast<float>(notches);
        events_.push_back(std::move(event));
        return frames(2);
    }

    // ---- semantics -------------------------------------------------------
    const std::vector<Semantic>& semantics() const { return last_->semantics; }

    const Semantic* named(Role role, std::string_view label, Status& error) const {
        const Semantic* found = nullptr;
        unsigned matches = 0;
        for (const auto& item : semantics()) {
            if (item.role == role && item.label == label) {
                ++matches;
                found = &item;
            }
        }
        if (matches != 1) {
            error = failure("expected exactly one role=" + std::to_string(static_cast<int>(role)) +
                " label='" + std::string(label) + "', found " + std::to_string(matches));
            return nullptr;
        }
        return found;
    }
    std::size_t count(Role role, std::string_view label) const {
        return static_cast<std::size_t>(std::count_if(semantics().begin(), semantics().end(),
            [&](const Semantic& item) { return item.role == role && item.label == label; }));
    }
    const Semantic* focused() const {
        for (const auto& item : semantics()) {
            if (item.focused) return &item;
        }
        return nullptr;
    }
    std::size_t popup_options() const {
        std::size_t found = 0;
        for (const char* label : kMenu) found += count(Role::dropdown, label);
        return found;
    }
    Point center(const Semantic& item) const {
        return {std::lround((item.bounds.x + item.bounds.w * 0.5f) * scale_),
                std::lround((item.bounds.y + item.bounds.h * 0.5f) * scale_)};
    }

    Status reveal(Role role, std::string_view label) {
        for (int notch = 0; notch < 80; ++notch) {
            Status error;
            const auto* item = named(role, label, error);
            if (!item) return error;
            const Rect view = viewport();
            if (inside(item->bounds, view)) return std::nullopt;
            const Point middle{std::lround(view.w * 0.5f * scale_), std::lround(view.h * 0.5f * scale_)};
            if (auto failed = wheel(middle, item->bounds.y < view.y ? 1 : -1)) return failed;
        }
        return failure("could not scroll '" + std::string(label) + "' into view");
    }
    Status point_of(Role role, std::string_view label, Point& point) {
        if (auto error = reveal(role, label)) return error;
        Status error;
        const auto* item = named(role, label, error);
        if (!item) return error;
        point = center(*item);
        return std::nullopt;
    }
    Status click(Role role, std::string_view label) {
        Point point;
        if (auto error = point_of(role, label, point)) return error;
        move(point);
        down(point);
        if (auto error = frames(1)) return error;
        up(point);
        return frames(2);
    }
    Status tab_to(Role role, std::string_view label) {
        for (int attempt = 0; attempt < 64; ++attempt) {
            if (auto error = press(Key::tab)) return error;
            const auto* item = focused();
            if (item && item->role == role && item->label == label) return std::nullopt;
        }
        return failure("keyboard traversal never focused '" + std::string(label) + "'");
    }

    Status check_model(bool notifications, unsigned restores, const char* operation) const {
        return expect(state_.notifications == notifications && state_.restore_count == restores,
            std::string(operation) + ": expected notifications=" + std::to_string(notifications) +
            " restore_count=" + std::to_string(restores) + ", observed " +
            std::to_string(state_.notifications) + "/" + std::to_string(state_.restore_count));
    }
    Status check_controls(bool enabled, Role focus = Role::panel) const {
        Status error;
        const auto* box = named(Role::checkbox, "Notifications", error);
        if (!box) return error;
        const auto* button = named(Role::button, "Restore defaults", error);
        if (!button) return error;
        return expect(box->enabled == enabled && button->enabled == enabled &&
            box->focused == (focus == Role::checkbox) && button->focused == (focus == Role::button) &&
            box->selected == state_.notifications,
            "control enabled/focused/selected semantics disagree with scenario");
    }

    // ---- evidence --------------------------------------------------------
    Status begin(std::string name, float scale, unsigned width = 640, unsigned height = 400,
                 bool disabled = false, bool notifications = true) {
        scenario_ = std::move(name);
        scale_ = scale;
        resize(static_cast<unsigned>(std::lround(width * scale)),
               static_cast<unsigned>(std::lround(height * scale)));
        if (auto error = reset()) return error;
        disabled_ = disabled;
        state_.notifications = notifications;
        if (auto error = settle()) return error;
        return check_controls(!disabled);
    }
    Status pass(std::string_view note = {}) {
        result_.log += "PASS " + scenario_ + " notifications=" + std::to_string(state_.notifications) +
            " restore_count=" + std::to_string(state_.restore_count) + "\n";
        if (!note.empty()) result_.log += std::string(note) + "\n";
        return std::nullopt;
    }
    void keep(std::string name, const DrawPacket& packet) {
        ReplayCheckpoint checkpoint;
        checkpoint.name = std::move(name);
        checkpoint.scale = packet->scale;
        checkpoint.width = static_cast<std::uint32_t>(std::lround(packet->viewport.w * packet->scale));
        checkpoint.height = static_cast<std::uint32_t>(std::lround(packet->viewport.h * packet->scale));
        checkpoint.packet = std::make_shared<PacketData>(*packet);
        result_.checkpoints.push_back(std::move(checkpoint));
    }
    // Native captures settle (or hover) 12 frames and capture the next one.
    Status capture(std::string name, std::optional<Point> hover = std::nullopt) {
        if (hover) move(*hover);
        if (auto error = settle()) return error;
        if (hover) move(*hover);
        if (auto error = frames(1)) return error;
        keep(std::move(name), last_);
        return std::nullopt;
    }

    // ---- scenarios -------------------------------------------------------
    Status scale_set() {
        for (std::size_t si = 0; si < kScales.size(); ++si) {
            const float scale = kScales[si];
            const std::string prefix = std::string(kScaleNames[si]) + "/";
            Status lookup;

            if (auto error = begin(prefix + "normal", scale)) return error;
            if (auto error = expect(state_.section == 1, "gallery did not start on Controls")) return error;
            if (auto error = capture(prefix + "normal-checked")) return error;
            if (auto error = check_controls(true)) return error;
            if (auto error = check_model(true, 0, "baseline")) return error;
            if (auto error = pass()) return error;

            if (auto error = begin(prefix + "mouse-checkbox", scale)) return error;
            const auto* box = named(Role::checkbox, "Notifications", lookup);
            if (!box) return lookup;
            const Point checkbox = center(*box);
            if (auto error = capture(prefix + "checkbox-hover", checkbox)) return error;
            down(checkbox);
            if (auto error = frames(1)) return error;
            if (auto error = check_model(true, 0, "checkbox down")) return error;
            if (auto error = capture(prefix + "checkbox-pressed")) return error;
            if (auto error = check_model(true, 0, "checkbox held")) return error;
            up(checkbox);
            if (auto error = frames(2)) return error;
            if (auto error = check_model(false, 0, "checkbox release")) return error;
            move({4, 4});
            focus_lost();
            if (auto error = capture(prefix + "unchecked")) return error;
            if (auto error = check_controls(true)) return error;
            if (auto error = pass()) return error;

            if (auto error = begin(prefix + "mouse-button", scale)) return error;
            if (auto error = click(Role::checkbox, "Notifications")) return error;
            if (auto error = check_model(false, 0, "button prerequisite checkbox toggle")) return error;
            focus_lost();
            const auto* restore = named(Role::button, "Restore defaults", lookup);
            if (!restore) return lookup;
            const Point button = center(*restore);
            if (auto error = capture(prefix + "button-hover", button)) return error;
            down(button);
            if (auto error = frames(1)) return error;
            if (auto error = check_model(false, 0, "button down")) return error;
            if (auto error = capture(prefix + "button-pressed")) return error;
            up(button);
            if (auto error = frames(2)) return error;
            if (auto error = check_model(true, 1, "button matching release")) return error;
            if (auto error = check_controls(true, Role::button)) return error;
            down(button);
            if (auto error = settle()) return error;
            move({4, 4});
            if (auto error = settle()) return error;
            up({4, 4});
            if (auto error = settle()) return error;
            if (auto error = check_model(true, 1, "button outside release")) return error;
            if (auto error = pass()) return error;

            if (auto error = begin(prefix + "keyboard", scale)) return error;
            if (auto error = frames(1)) return error;
            if (auto error = tab_to(Role::checkbox, "Notifications")) return error;
            if (auto error = check_controls(true, Role::checkbox)) return error;
            if (auto error = capture(prefix + "checkbox-focus")) return error;
            if (auto error = key_frame(Key::space, true)) return error;
            for (int repeat = 0; repeat < 3; ++repeat) {
                if (auto error = key_frame(Key::space, true, true)) return error;
                if (auto error = check_model(true, 0, "Space repeated down")) return error;
            }
            if (auto error = settle()) return error;
            if (auto error = key_frame(Key::space, false)) return error;
            if (auto error = settle()) return error;
            if (auto error = check_model(false, 0, "Space release")) return error;
            if (auto error = press(Key::tab)) return error;
            if (auto error = check_controls(true, Role::button)) return error;
            if (auto error = capture(prefix + "button-focus")) return error;
            if (auto error = key_frame(Key::enter, true)) return error;
            for (int repeat = 0; repeat < 3; ++repeat) {
                if (auto error = key_frame(Key::enter, true, true)) return error;
                if (auto error = check_model(false, 0, "Enter repeated down")) return error;
            }
            if (auto error = settle()) return error;
            if (auto error = key_frame(Key::enter, false)) return error;
            if (auto error = settle()) return error;
            if (auto error = check_model(true, 1, "Enter release")) return error;
            if (auto error = check_controls(true, Role::button)) return error;
            if (auto error = pass()) return error;

            for (bool checked : {true, false}) {
                const std::string image = checked ? "disabled-checked" : "disabled-unchecked";
                if (auto error = begin(prefix + image, scale, 640, 400, true, checked)) return error;
                for (Role role : {Role::checkbox, Role::button}) {
                    const auto* item = named(role, role == Role::checkbox ? "Notifications" : "Restore defaults", lookup);
                    if (!item) return lookup;
                    const Point point = center(*item);
                    move(point);
                    down(point);
                    if (auto error = settle()) return error;
                    up(point);
                    if (auto error = frames(2)) return error;
                    if (auto error = check_model(checked, 0, "disabled pointer release")) return error;
                    if (auto error = check_controls(false)) return error;
                }
                for (int traversal = 0; traversal < 3; ++traversal) {
                    if (auto error = press(Key::tab)) return error;
                    for (Key k : {Key::space, Key::enter}) {
                        if (auto error = key_frame(k, true)) return error;
                        if (auto error = key_frame(k, true, true)) return error;
                        if (auto error = key_frame(k, false)) return error;
                        if (auto error = check_model(checked, 0, "disabled key release")) return error;
                        if (auto error = check_controls(false)) return error;
                    }
                }
                move({4, 4});
                if (auto error = capture(prefix + image)) return error;
                if (auto error = pass()) return error;
            }
        }
        return std::nullopt;
    }

    Status scenario_tabs() {
        const std::string scenes = "scenario-01-tabs-hide-show/";
        if (auto error = begin("scenario-01-tabs-hide-show", 1.0f)) return error;
        Status lookup;
        const auto* box = named(Role::checkbox, "Notifications", lookup);
        if (!box) return lookup;
        const auto box_id = box->id.value;
        const auto* restore = named(Role::button, "Restore defaults", lookup);
        if (!restore) return lookup;
        const auto restore_id = restore->id.value;
        std::array<std::uint64_t, 3> tab_ids{};
        for (std::size_t i = 0; i < kSections.size(); ++i) {
            const auto* tab = named(Role::tab, kSections[i], lookup);
            if (!tab) return lookup;
            tab_ids[i] = tab->id.value;
        }
        if (auto error = click(Role::checkbox, "Notifications")) return error;
        if (auto error = check_model(false, 0, "tabs model edit")) return error;
        if (auto error = click(Role::tab, "Appearance")) return error;
        if (auto error = expect(state_.section == 0 && count(Role::checkbox, "Notifications") == 0 &&
                count(Role::button, "Restore defaults") == 0,
                "Appearance not selected or hidden Controls still emitted semantics")) return error;
        if (auto error = capture(scenes + "section-appearance")) return error;
        if (auto error = click(Role::tab, "Diagnostics")) return error;
        std::size_t rows = 0;
        for (const auto& item : semantics()) rows += item.role == Role::button && item.label.rfind("Row ", 0) == 0;
        if (auto error = expect(state_.section == 2 && count(Role::button, "Row 1") == 1 &&
                count(Role::button, "Row 10000") == 0 && rows > 0 && rows < 100,
                "Diagnostics not selected or virtual list emitted invisible rows")) return error;
        if (auto error = capture(scenes + "section-diagnostics")) return error;
        if (auto error = click(Role::tab, "Controls")) return error;
        if (auto error = check_model(false, 0, "tabs model after switching")) return error;
        box = named(Role::checkbox, "Notifications", lookup);
        if (!box) return lookup;
        restore = named(Role::button, "Restore defaults", lookup);
        if (!restore) return lookup;
        bool stable = box->id.value == box_id && restore->id.value == restore_id && !box->selected;
        for (std::size_t i = 0; i < kSections.size(); ++i) {
            const auto* tab = named(Role::tab, kSections[i], lookup);
            if (!tab) return lookup;
            stable = stable && tab->id.value == tab_ids[i];
        }
        if (auto error = expect(stable, "IDs or model changed across tab hide/show")) return error;
        return pass("tabs: keyed IDs and model survive hide/show; virtual rows emitted=" + std::to_string(rows));
    }

    Status scenario_choices() {
        if (auto error = begin("scenario-02-choice-values", 1.0f)) return error;
        Status lookup;
        std::array<std::uint64_t, 3> radio_ids{};
        for (std::size_t i = 0; i < kMenu.size(); ++i) {
            const auto* radio = named(Role::radio, kMenu[i], lookup);
            if (!radio) return lookup;
            radio_ids[i] = radio->id.value;
        }
        for (const auto& [label, value] : {std::pair{"High", 2}, std::pair{"Low", 0}}) {
            if (auto error = click(Role::radio, label)) return error;
            const auto* menu = named(Role::dropdown, "Quality menu", lookup);
            if (!menu) return lookup;
            if (auto error = expect(state_.quality == value && menu->value == label && menu->selected,
                    std::string("reordered choice did not select ") + label)) return error;
        }
        state_.quality = 7;
        if (auto error = settle()) return error;
        bool none_selected = true;
        for (std::size_t i = 0; i < kMenu.size(); ++i) {
            const auto* radio = named(Role::radio, kMenu[i], lookup);
            if (!radio) return lookup;
            none_selected = none_selected && !radio->selected && radio->id.value == radio_ids[i];
        }
        const auto* menu = named(Role::dropdown, "Quality menu", lookup);
        if (!menu) return lookup;
        std::size_t missing = 0;
        for (const auto& diagnostic : context_->diagnostics()) {
            missing += diagnostic.message == "Selected value is missing from choices";
        }
        if (auto error = expect(none_selected && !menu->selected && missing == 2,
                "missing value selected an option or was not diagnosed by both groups")) return error;
        if (auto error = capture("scenario-02-choice-values/missing-value")) return error;
        state_.quality = 1;
        if (auto error = settle()) return error;
        if (auto error = expect(context_->diagnostics().empty(),
                "diagnostics persisted after the model value was restored")) return error;
        return pass();
    }

    Status open_menu(const std::string& name, bool above) {
        Status lookup;
        const auto* trigger = named(Role::dropdown, "Quality menu", lookup);
        if (!trigger) return lookup;
        const Rect anchor = trigger->bounds;
        const Rect view = viewport();
        if (auto error = expect(inside(anchor, view), "dropdown trigger not fully visible")) return error;
        if (auto error = click(Role::dropdown, "Quality menu")) return error;
        if (auto error = expect(popup_options() == 3, "dropdown did not open three options")) return error;
        for (const char* label : kMenu) {
            const auto* option = named(Role::dropdown, label, lookup);
            if (!option) return lookup;
            const bool placed = above ? option->bounds.y + option->bounds.h <= anchor.y + 0.5f
                                      : option->bounds.y >= anchor.y + anchor.h - 0.5f;
            if (auto error = expect(inside(option->bounds, view) && placed,
                    std::string("popup option outside viewport or on wrong side: ") + label)) return error;
        }
        return capture(name);
    }

    Status scenario_dropdown() {
        const std::string scenes = "scenario-03-dropdown-edges/";
        if (auto error = begin("scenario-03-dropdown-edges", 1.0f)) return error;
        if (auto error = open_menu(scenes + "popup-bottom-edge", true)) return error;
        const auto quality = state_.quality;
        if (auto error = click(Role::checkbox, "Notifications")) return error;
        if (auto error = expect(popup_options() == 0 && state_.quality == quality,
                "outside click did not close popup or changed selection")) return error;
        if (auto error = check_model(true, 0, "outside click consumed")) return error;
        resize(640, 240);
        if (auto error = settle()) return error;
        for (int notch = 0; notch < 40; ++notch) {
            Status lookup;
            const auto* trigger = named(Role::dropdown, "Quality menu", lookup);
            if (!trigger) return lookup;
            if (trigger->bounds.y <= 40) break;
            if (auto error = wheel({320, 120}, -1)) return error;
        }
        if (auto error = open_menu(scenes + "popup-top-edge", false)) return error;
        if (auto error = click(Role::dropdown, "High")) return error;
        if (auto error = expect(state_.quality == 2 && popup_options() == 0,
                "choosing a popup option did not select and close")) return error;
        resize(200, 400);
        if (auto error = settle()) return error;
        for (int notch = 0; notch < 60; ++notch) {
            Status lookup;
            const auto* trigger = named(Role::dropdown, "Quality menu", lookup);
            if (!trigger) return lookup;
            if (trigger->bounds.y >= 0 && trigger->bounds.y + trigger->bounds.h <= 360) break;
            if (auto error = wheel({100, 200}, trigger->bounds.y < 0 ? 1 : -1)) return error;
        }
        Status lookup;
        const auto* trigger = named(Role::dropdown, "Quality menu", lookup);
        if (!trigger) return lookup;
        const float below = 400 - (trigger->bounds.y + trigger->bounds.h);
        if (auto error = open_menu(scenes + "popup-narrow", below < 124)) return error;
        if (auto error = press(Key::escape)) return error;
        if (auto error = expect(popup_options() == 0, "Escape did not close popup")) return error;
        resize(640, 400);
        if (auto error = settle()) return error;
        return pass();
    }

    Status scenario_slider() {
        if (auto error = begin("scenario-04-slider-capture", 1.0f)) return error;
        Point slider;
        if (auto error = point_of(Role::slider, "Volume", slider)) return error;
        const long right = static_cast<long>(width_) + 200;
        move(slider);
        down(slider);
        if (auto error = frames(1)) return error;
        const float pressed = state_.volume;
        if (auto error = expect(pressed > 0.3f && pressed < 0.7f,
                "slider press did not move value toward pointer")) return error;
        if (auto error = capture("scenario-04-slider-capture/slider-held")) return error;
        move({right, slider.y});
        if (auto error = frames(2)) return error;
        if (auto error = expect(state_.volume == 1.0f, "drag right did not clamp to max")) return error;
        move({-200, slider.y});
        if (auto error = frames(2)) return error;
        if (auto error = expect(state_.volume == 0.0f, "drag left did not clamp to min")) return error;
        up({-200, slider.y});
        if (auto error = frames(2)) return error;
        move(slider);
        if (auto error = frames(2)) return error;
        if (auto error = expect(state_.volume == 0.0f, "released slider still dragged")) return error;
        // Focus loss while held cancels the drag.
        down(slider);
        if (auto error = frames(1)) return error;
        const float before_loss = state_.volume;
        focus_lost();
        if (auto error = frames(2)) return error;
        move({right, slider.y});
        if (auto error = frames(2)) return error;
        if (auto error = expect(state_.volume == before_loss, "slider moved after focus loss")) return error;
        up({right, slider.y});
        if (auto error = frames(2)) return error;
        // Removing the held slider ends the drag.
        move(slider);
        down(slider);
        if (auto error = frames(1)) return error;
        const float before_removal = state_.volume;
        state_.section = 0;
        if (auto error = frames(2)) return error;
        move({right, slider.y});
        if (auto error = frames(2)) return error;
        state_.section = 1;
        if (auto error = frames(2)) return error;
        if (auto error = expect(state_.volume == before_removal, "removed slider kept dragging")) return error;
        up({right, slider.y});
        if (auto error = settle()) return error;
        if (auto error = point_of(Role::slider, "Volume", slider)) return error;
        disabled_ = true;
        if (auto error = settle()) return error;
        const float disabled_value = state_.volume;
        move(slider);
        down(slider);
        if (auto error = frames(1)) return error;
        move({right, slider.y});
        if (auto error = frames(2)) return error;
        up({right, slider.y});
        if (auto error = frames(2)) return error;
        Status lookup;
        const auto* item = named(Role::slider, "Volume", lookup);
        if (!item) return lookup;
        if (auto error = expect(state_.volume == disabled_value && !item->enabled,
                "disabled slider changed")) return error;
        disabled_ = false;
        return pass();
    }

    Status scenario_keyboard() {
        if (auto error = begin("scenario-05-keyboard", 1.0f)) return error;
        if (auto error = frames(1)) return error;
        if (auto error = tab_to(Role::checkbox, "Notifications")) return error;
        if (auto error = press(Key::tab)) return error;
        if (auto error = check_controls(true, Role::button)) return error;
        if (auto error = chord(true, false, Key::tab)) return error;
        if (auto error = check_controls(true, Role::checkbox)) return error;
        if (auto error = press(Key::space)) return error;
        if (auto error = check_model(false, 0, "keyboard Space")) return error;
        if (auto error = press(Key::tab)) return error;
        if (auto error = press(Key::enter)) return error;
        if (auto error = check_model(true, 1, "keyboard Enter")) return error;
        if (auto error = tab_to(Role::dropdown, "Quality menu")) return error;
        if (auto error = press(Key::enter)) return error;
        if (auto error = expect(popup_options() == 3, "Enter did not open focused dropdown")) return error;
        if (auto error = capture("scenario-05-keyboard/popup-keyboard")) return error;
        if (auto error = press(Key::escape)) return error;
        const auto* item = focused();
        if (auto error = expect(popup_options() == 0 && item && item->role == Role::dropdown &&
                item->label == "Quality menu",
                "Escape did not close popup and restore trigger focus")) return error;
        if (auto error = click(Role::tab, "Appearance")) return error;
        bool prominent = false, quiet = false, disabled = false;
        for (int step = 0; step < 40; ++step) {
            if (auto error = press(Key::tab)) return error;
            if (const auto* now = focused()) {
                prominent |= now->label == "Prominent";
                quiet |= now->label == "Quiet";
                disabled |= now->label == "Disabled";
            }
        }
        if (auto error = expect(prominent && quiet && !disabled,
                "Tab traversal did not skip the disabled button")) return error;
        if (auto error = tab_to(Role::button, "Quiet")) return error;
        state_.section = 1;
        if (auto error = settle()) return error;
        if (auto error = expect(count(Role::button, "Quiet") == 0 && focused() == nullptr,
                "missing focused widget still reported focus")) return error;
        if (auto error = press(Key::tab)) return error;
        if (auto error = expect(focused() != nullptr, "Tab after missing focus did not recover")) return error;
        return pass();
    }

    Status name_is(const std::string& expected, const char* step) const {
        Status missing;
        const auto* input = named(Role::text_input, "Name", missing);
        if (!input) return missing;
        return expect(state_.name == expected && input->value == expected && input->focused,
            std::string("text editing step failed: ") + step);
    }

    Status scenario_text() {
        const std::string scenes = "scenario-06-text-editing/";
        if (auto error = begin("scenario-06-text-editing", 1.0f)) return error;
        if (auto error = click(Role::text_input, "Name")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        if (auto error = type("I\xC4\x9F" "d\xC4\xB1r")) return error;
        if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "replace selection")) return error;
        if (auto error = type("e\xCC\x81")) return error;
        if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1re\xCC\x81", "combining mark")) return error;
        if (auto error = press(Key::backspace)) return error;
        if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "grapheme backspace")) return error;
        if (auto error = chord(true, false, Key::left)) return error;
        if (auto error = chord(true, false, Key::left)) return error;
        if (auto error = capture(scenes + "text-selection")) return error;
        if (auto error = type("z")) return error;
        if (auto error = name_is("I\xC4\x9F" "dz", "type over selection")) return error;
        if (auto error = chord(false, true, Key::z)) return error;
        if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r", "undo")) return error;
        if (auto error = press(Key::end)) return error;
        if (auto error = type("\xC4\xB0")) return error;
        if (auto error = name_is("I\xC4\x9F" "d\xC4\xB1r\xC4\xB0", "dotted capital I")) return error;
        if (auto error = capture(scenes + "text-caret")) return error;
        if (auto error = click(Role::numeric_input, "Amount")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        if (auto error = type("42")) return error;
        if (auto error = press(Key::enter)) return error;
        if (auto error = expect(state_.amount == 42.0, "Enter did not commit 42")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        if (auto error = type("abc")) return error;
        if (auto error = press(Key::escape)) return error;
        Status lookup;
        const auto* amount = named(Role::numeric_input, "Amount", lookup);
        if (!amount) return lookup;
        if (auto error = expect(state_.amount == 42.0 && amount->value == "42",
                "Escape did not restore committed amount")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        if (auto error = type("5000")) return error;
        if (auto error = press(Key::tab)) return error;
        amount = named(Role::numeric_input, "Amount", lookup);
        if (!amount) return lookup;
        if (auto error = expect(state_.amount == 42.0 && amount->value == "42" && !amount->focused,
                "out-of-range draft was not restored on focus loss")) return error;
        return pass();
    }

    Status scenario_matrix() {
        constexpr std::array<std::pair<unsigned, unsigned>, 3> extents{
            std::pair{800u, 600u}, std::pair{1280u, 720u}, std::pair{1920u, 1080u}};
        scenario_ = "scenario-07-resize-matrix";
        for (std::size_t si = 0; si < kScales.size(); ++si) {
            for (std::size_t ei = 0; ei < extents.size(); ++ei) {
                const auto [w, h] = extents[ei];
                const auto [other_w, other_h] = extents[(ei + 1) % extents.size()];
                scale_ = kScales[si];
                resize(w, h);
                if (auto error = reset()) return error;
                if (auto error = settle()) return error;
                Status lookup;
                const Rect view = viewport();
                for (const auto& [role, label] : {std::pair{Role::checkbox, "Notifications"},
                                                  std::pair{Role::button, "Restore defaults"}}) {
                    const auto* item = named(role, label, lookup);
                    if (!item) return lookup;
                    if (auto error = expect(inside(item->bounds, view),
                            std::string(label) + " outside the logical viewport")) return error;
                }
                Point slider;
                if (auto error = point_of(Role::slider, "Volume", slider)) return error;
                move(slider);
                down(slider);
                if (auto error = frames(1)) return error;
                resize(other_w, other_h);
                if (auto error = settle()) return error;
                up({4, 4});
                if (auto error = settle()) return error;
                const float volume = state_.volume;
                if (auto error = expect(std::isfinite(volume) && volume >= 0 && volume <= 1,
                        "slider value invalid after resize")) return error;
                resize(w, h);
                char image[96]{};
                std::snprintf(image, sizeof(image), "scenario-07-resize-matrix/matrix-%s-%ux%u",
                    kScaleNames[si], w, h);
                if (auto error = capture(image)) return error;
            }
        }
        scale_ = 1.0f;
        resize(640, 400);
        return pass();
    }

    // Vertex positions/colors of the last packet; the hover fill animates in color.
    std::vector<Vertex> vertices() const { return last_->vertices; }

    static bool same(const std::vector<Vertex>& a, const std::vector<Vertex>& b) {
        return a.size() == b.size() &&
            (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vertex)) == 0);
    }

    Status scenario_states() {
        const std::string scenes = "scenario-08-visual-states/";
        if (auto error = begin("scenario-08-visual-states", 1.0f)) return error;
        Status lookup;
        if (auto error = capture(scenes + "state-normal")) return error;
        Point restore;
        if (auto error = point_of(Role::button, "Restore defaults", restore)) return error;
        if (auto error = capture(scenes + "state-hover", restore)) return error;
        move(restore);
        if (auto error = frames(30)) return error;
        const auto* tip = named(Role::tooltip, "Turns notifications back on", lookup);
        if (!tip) return lookup;
        if (auto error = expect(inside(tip->bounds, viewport()), "tooltip outside the viewport")) return error;
        if (auto error = capture(scenes + "state-tooltip", restore)) return error;
        down(restore);
        if (auto error = frames(1)) return error;
        if (auto error = capture(scenes + "state-pressed")) return error;
        move({4, 4});
        up({4, 4});
        if (auto error = settle()) return error;
        if (auto error = check_model(true, 0, "pressed state released outside")) return error;
        if (auto error = tab_to(Role::checkbox, "Notifications")) return error;
        const auto* balanced = named(Role::radio, "Balanced", lookup);
        if (!balanced) return lookup;
        const auto* controls = named(Role::tab, "Controls", lookup);
        if (!controls) return lookup;
        if (auto error = expect(balanced->selected && controls->selected,
                "selected radio/tab state not reported")) return error;
        if (auto error = capture(scenes + "state-focused-selected")) return error;
        focus_lost();
        if (auto error = settle()) return error;
        auto hover = [&](std::vector<Vertex>& first, std::vector<Vertex>& settled) -> Status {
            move({4, 4});
            if (auto error = settle()) return error;
            move(restore);
            if (auto error = frames(1)) return error;
            first = vertices();
            if (auto error = frames(12)) return error;
            settled = vertices();
            return std::nullopt;
        };
        std::vector<Vertex> animated_first, animated_settled, reduced_first, reduced_settled;
        if (auto error = hover(animated_first, animated_settled)) return error;
        state_.reduced_motion = true;
        if (auto error = hover(reduced_first, reduced_settled)) return error;
        if (auto error = expect(!same(animated_first, animated_settled),
                "hover transition not observable at normal motion")) return error;
        if (auto error = expect(same(reduced_first, reduced_settled),
                "reduced motion still animated hover")) return error;
        state_.reduced_motion = false;
        move({4, 4});
        disabled_ = true;
        if (auto error = settle()) return error;
        for (const auto& [role, label] : {std::pair{Role::checkbox, "Notifications"},
                                          std::pair{Role::button, "Restore defaults"},
                                          std::pair{Role::slider, "Volume"},
                                          std::pair{Role::dropdown, "Quality menu"}}) {
            const auto* item = named(role, label, lookup);
            if (!item) return lookup;
            if (auto error = expect(!item->enabled, std::string(label) + " not disabled")) return error;
        }
        if (auto error = capture(scenes + "state-disabled")) return error;
        disabled_ = false;
        if (auto error = settle()) return error;
        if (auto error = click(Role::text_input, "Name")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        if (auto error = press(Key::backspace)) return error;
        state_.quantity = 9;
        if (auto error = settle()) return error;
        const auto* name = named(Role::text_input, "Name", lookup);
        if (!name) return lookup;
        const auto* quantity = named(Role::slider, "Quantity", lookup);
        if (!quantity) return lookup;
        if (auto error = expect(state_.name.empty() && name->invalid && quantity->invalid &&
                count(Role::validation, "Name is required") == 1 &&
                count(Role::validation, "Quantities above 8 need approval") == 1,
                "invalid state or validation message missing")) return error;
        if (auto error = wheel({320, 200}, -10)) return error;
        if (auto error = capture(scenes + "state-invalid")) return error;
        return pass();
    }

    Status scenario_atlas() {
        const std::string scenes = "scenario-09-glyph-atlas-recreation/";
        if (auto error = begin("scenario-09-glyph-atlas-recreation", 1.0f)) return error;
        keep(scenes + "before-glyphs", last_);
        const DrawPacket before = result_.checkpoints.back().packet;
        if (auto error = click(Role::text_input, "Name")) return error;
        if (auto error = chord(false, true, Key::a)) return error;
        const std::string glyphs = "\xD0\x96\xD0\xAF\xCE\xA9\xCE\xA8\xC5\xA1\xC4\x91";
        if (auto error = type(glyphs)) return error;
        if (auto error = settle()) return error;
        bool new_snapshot = false;
        for (const auto& snapshot : last_->textures) {
            new_snapshot |= std::find(before->textures.begin(), before->textures.end(), snapshot) ==
                before->textures.end();
        }
        if (auto error = expect(state_.name == glyphs && new_snapshot,
                "new glyphs did not publish a new atlas snapshot")) return error;
        if (auto error = capture(scenes + "after-glyphs")) return error;
        return pass();
    }

    Status scenario_packets() {
        const std::string scenes = "scenario-10-packets-in-flight/";
        if (auto error = begin("scenario-10-packets-in-flight", 1.0f)) return error;
        DrawPacket held = last_;
        keep(scenes + "held", held);
        if (auto error = frames(6)) return error;
        std::vector<DrawPacket> direct;
        bool refused = false;
        for (int attempt = 0; attempt < 4 && !refused; ++attempt) {
            auto begun = context_->begin_frame(FrameInput{viewport(), scale_, 1.0 / 60.0, {}});
            if (const auto* error = std::get_if<Error>(&begun)) {
                if (error->code != ErrorCode::busy) return *error;
                refused = true;
                break;
            }
            Frame frame = std::get<Frame>(std::move(begun));
            build_gallery(frame, state_);
            auto finished = frame.finish();
            if (auto* error = std::get_if<Error>(&finished)) return *error;
            direct.push_back(std::get<DrawPacket>(std::move(finished)));
        }
        const std::size_t filled = direct.size();
        if (auto error = expect(refused && filled >= 1 && filled <= 3,
                "context did not refuse a frame once four packets were live")) return error;
        direct.clear();
        if (auto error = click(Role::text_input, "Name")) return error;
        if (auto error = type("\xD1\x89\xCE\xBB")) return error;
        if (auto error = settle()) return error;
        if (auto error = capture(scenes + "after-growth")) return error;
        held.reset();
        if (auto error = settle()) return error;
        return pass("packets: " + std::to_string(filled) + " direct frames filled the free slots, next begin busy");
    }

    Status extras() {
        if (auto error = begin("resize-during-checkbox-press", 1.0f)) return error;
        Status lookup;
        const auto* box = named(Role::checkbox, "Notifications", lookup);
        if (!box) return lookup;
        down(center(*box));
        if (auto error = frames(1)) return error;
        resize(320, 240);
        if (auto error = settle()) return error;
        if (auto error = check_model(true, 0, "resize while held")) return error;
        up({4, 4});
        if (auto error = settle()) return error;
        if (auto error = check_model(true, 0, "resized outside release")) return error;
        if (auto error = capture("resize-during-checkbox-press/320x240")) return error;
        if (auto error = pass()) return error;

        if (auto error = begin("narrow-checkbox-clipping", 1.0f, 180, 400)) return error;
        const auto* narrow = named(Role::checkbox, "Notifications", lookup);
        if (!narrow) return lookup;
        const auto* button = named(Role::button, "Restore defaults", lookup);
        if (!button) return lookup;
        if (auto error = expect(narrow->bounds.h >= Theme::dark().control_height && narrow->bounds.x >= 0 &&
                narrow->bounds.x + narrow->bounds.w <= 180 &&
                narrow->bounds.y + narrow->bounds.h <= button->bounds.y && button->bounds.x >= 0 &&
                button->bounds.x + button->bounds.w <= 180,
                "180px controls violate minimum height, viewport bounds or non-overlap")) return error;
        if (auto error = capture("narrow-checkbox-clipping/narrow-180")) return error;
        if (auto error = pass()) return error;

        if (auto error = begin("focus-loss-while-captured", 1.0f)) return error;
        box = named(Role::checkbox, "Notifications", lookup);
        if (!box) return lookup;
        const Point checkbox = center(*box);
        down(checkbox);
        if (auto error = frames(1)) return error;
        focus_lost();
        if (auto error = frames(2)) return error;
        up({4, 4});
        if (auto error = frames(2)) return error;
        if (auto error = check_model(true, 0, "release after focus loss")) return error;
        if (auto error = click(Role::checkbox, "Notifications")) return error;
        if (auto error = check_model(false, 0, "ordinary click after focus loss")) return error;
        return pass();
    }

    const ReplayServices& services_;
    TextureHandle icon_{};
    std::unique_ptr<Context> context_;
    GalleryState state_;
    std::optional<std::array<int, 3>> applied_theme_;
    std::vector<InputEvent> events_;
    DrawPacket last_;
    unsigned width_ = 640, height_ = 400;
    float scale_ = 1.0f;
    bool disabled_ = false;
    std::string scenario_ = "initialization";
    ReplayResult result_;
};

}  // namespace

wui::Result<ReplayResult> replay_gallery(const ReplayServices& services) {
    if (!services.textures || !services.text) {
        return wui::Error{wui::ErrorCode::invalid_argument, "replay requires texture and text services"};
    }
    Replay replay(services);
    if (auto error = replay.run()) {
        return wui::Error{error->code, "replay failed: " + error->message};
    }
    return replay.take();
}
