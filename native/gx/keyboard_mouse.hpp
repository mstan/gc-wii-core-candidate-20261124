// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace gcnllr::keyboard_mouse {

enum Action {
    Forward, Back, StrafeLeft, StrafeRight, Jump, Morph, SpiderBall,
    Fire, Missile, LockOn, PreviousVisor, NextVisor,
    PowerBeam, DarkBeam, LightBeam, AnnihilatorBeam, Map, Pause,
    MenuConfirm, MenuBack, BoostBall, PowerBomb, ActionCount,
};

struct Binding {
    enum Device { None, Key, Mouse } device = None;
    int code = 0;
};
using BindingSet = std::array<Binding, 3>;
using Bindings = std::array<BindingSet, ActionCount>;

constexpr Binding key(SDL_Scancode code) { return {Binding::Key, code}; }
constexpr Binding mouse(int button) { return {Binding::Mouse, button}; }

struct ActionInfo {
    const char* id;
    const char* label;
    const char* group;
    BindingSet initial;
};

inline constexpr std::array<ActionInfo, ActionCount> actions{{
    {"forward", "Move forward", "Movement", {key(SDL_SCANCODE_W)}},
    {"back", "Move back", "Movement", {key(SDL_SCANCODE_S)}},
    {"strafe-left", "Strafe left", "Movement", {key(SDL_SCANCODE_A)}},
    {"strafe-right", "Strafe right", "Movement", {key(SDL_SCANCODE_D)}},
    {"jump", "Jump", "Movement", {key(SDL_SCANCODE_SPACE)}},
    {"morph", "Morph Ball", "Movement", {key(SDL_SCANCODE_LCTRL), key(SDL_SCANCODE_RCTRL)}},
    {"spider-ball", "Spider Ball", "Morph Ball", {mouse(SDL_BUTTON_RIGHT)}},
    {"beam", "Fire beam / Lay bomb", "Combat", {mouse(SDL_BUTTON_LEFT)}},
    {"missile", "Fire missile", "Combat", {mouse(SDL_BUTTON_RIGHT)}},
    {"lock-on", "Lock on / Scan / Grapple", "Combat", {key(SDL_SCANCODE_F), key(SDL_SCANCODE_LSHIFT), key(SDL_SCANCODE_RSHIFT)}},
    {"visor-previous", "Previous visor", "Visors", {mouse(SDL_BUTTON_X1), key(SDL_SCANCODE_Q)}},
    {"visor-next", "Next visor", "Visors", {mouse(SDL_BUTTON_X2), key(SDL_SCANCODE_R)}},
    {"power-beam", "Power Beam", "Beams", {key(SDL_SCANCODE_1)}},
    {"dark-beam", "Dark Beam", "Beams", {key(SDL_SCANCODE_2)}},
    {"light-beam", "Light Beam", "Beams", {key(SDL_SCANCODE_3)}},
    {"annihilator-beam", "Annihilator Beam", "Beams", {key(SDL_SCANCODE_4)}},
    {"map", "Map", "Menus", {key(SDL_SCANCODE_TAB)}},
    {"pause", "Pause", "Menus", {key(SDL_SCANCODE_RETURN)}},
    {"menu-confirm", "Confirm in game menus", "Menus", {key(SDL_SCANCODE_SPACE), mouse(SDL_BUTTON_LEFT)}},
    {"menu-back", "Back in game menus", "Menus", {key(SDL_SCANCODE_BACKSPACE), mouse(SDL_BUTTON_RIGHT)}},
    {"boost-ball", "Charge Boost Ball", "Morph Ball", {key(SDL_SCANCODE_LSHIFT), key(SDL_SCANCODE_RSHIFT)}},
    {"power-bomb", "Lay Power Bomb", "Morph Ball", {key(SDL_SCANCODE_E)}},
}};

inline constexpr int min_sensitivity = 25, max_sensitivity = 400, default_sensitivity = 100;
inline constexpr char sensitivity_file[] = "mouse-sensitivity.cfg";

inline bool valid_sensitivity(int value) {
    return value >= min_sensitivity && value <= max_sensitivity;
}

inline int load_sensitivity(const std::filesystem::path& path, int fallback) {
    std::ifstream file(path);
    int value;
    return file >> value && valid_sensitivity(value) ? value : fallback;
}

inline bool save_sensitivity(const std::filesystem::path& path, int value) {
    if (path.empty() || !valid_sensitivity(value))
        return false;
    auto pending = path;
    pending += "." + std::to_string(SDL_GetPerformanceCounter()) + ".tmp";
    std::ofstream file(pending);
    file << value << '\n';
    file.close();
    const auto from = pending.u8string();
    const auto to = path.u8string();
    if (file && SDL_RenamePath(reinterpret_cast<const char*>(from.c_str()),
                               reinterpret_cast<const char*>(to.c_str())))
        return true;
    std::error_code error;
    std::filesystem::remove(pending, error);
    return false;
}

inline Bindings default_bindings() {
    Bindings defaults;
    for (size_t i = 0; i < defaults.size(); ++i)
        defaults[i] = actions[i].initial;
    return defaults;
}

inline std::string encode_bindings(const BindingSet& bindings) {
    std::string result;
    for (const auto& binding : bindings) {
        if (!result.empty())
            result += ',';
        if (binding.device == Binding::None)
            result += "none";
        else
            result += (binding.device == Binding::Key ? "key:" : "mouse:") + std::to_string(binding.code);
    }
    return result;
}

inline bool parse_binding(std::string_view token, Binding& binding) {
    if (token == "none") {
        binding = {};
        return true;
    }
    const auto split = token.find(':');
    if (split == token.npos)
        return false;
    const auto type = token.substr(0, split);
    const char* end = token.data() + token.size();
    int code{};
    const auto parsed = std::from_chars(token.data() + split + 1, end, code);
    if (parsed.ec != std::errc{} || parsed.ptr != end || code <= 0)
        return false;
    if (type == "key" && code < SDL_SCANCODE_COUNT)
        binding = {Binding::Key, code};
    else if (type == "mouse" && code <= SDL_BUTTON_X2)
        binding = {Binding::Mouse, code};
    else
        return false;
    return true;
}

inline bool parse_bindings(std::string_view text, BindingSet& result) {
    BindingSet parsed{};
    for (auto& binding : parsed) {
        const auto comma = text.find(',');
        if (!parse_binding(text.substr(0, comma), binding))
            return false;
        if (comma == text.npos) {
            result = parsed;
            return true;
        }
        text.remove_prefix(comma + 1);
    }
    return false;
}

inline bool held(const BindingSet& bindings, const bool* keys, SDL_MouseButtonFlags buttons) {
    for (const auto& binding : bindings) {
        if (binding.device == Binding::Key && keys && keys[binding.code])
            return true;
        if (binding.device == Binding::Mouse && (buttons & SDL_BUTTON_MASK(binding.code)))
            return true;
    }
    return false;
}

struct Settings {
    int sensitivity = default_sensitivity;
    bool invert_y = false;
    Bindings bindings = default_bindings();

    bool load(std::istream& stream) {
        Settings parsed;
        if (!(stream >> parsed.sensitivity >> parsed.invert_y) ||
            !valid_sensitivity(parsed.sensitivity))
            return false;
        bool legacy = false;
        bool had_menu_version = false;
        bool add_grapple_default = true;
        for (size_t i = 0; i < parsed.bindings.size(); ++i) {
            std::string text;
            if (!(stream >> text)) {
                if (stream.eof() && (i == MenuConfirm || i == BoostBall)) {
                    legacy = true;
                    break;
                }
                return false;
            }
            if (i == BoostBall && text == "bindings-v2") {
                legacy = had_menu_version = true;
                break;
            }
            if (!parse_bindings(text, parsed.bindings[i]))
                return false;
        }
        if (legacy) {
            if (!had_menu_version &&
                encode_bindings(parsed.bindings[MenuBack]) == encode_bindings({key(SDL_SCANCODE_BACKSPACE)}))
                parsed.bindings[MenuBack] = actions[MenuBack].initial;
            if (encode_bindings(parsed.bindings[SpiderBall]) == encode_bindings({key(SDL_SCANCODE_E)}))
                parsed.bindings[SpiderBall] = actions[SpiderBall].initial;
        } else {
            std::string version;
            if (!(stream >> version) || (version != "bindings-v3" && version != "bindings-v4"))
                return false;
            add_grapple_default = version == "bindings-v3";
        }
        if (add_grapple_default &&
            encode_bindings(parsed.bindings[LockOn]) ==
                encode_bindings({key(SDL_SCANCODE_LSHIFT), key(SDL_SCANCODE_RSHIFT)})) {
            const bool f_bound = std::any_of(parsed.bindings.begin(), parsed.bindings.end(), [](const auto& set) {
                return std::any_of(set.begin(), set.end(), [](const Binding& binding) {
                    return binding.device == Binding::Key && binding.code == SDL_SCANCODE_F;
                });
            });
            if (!f_bound)
                parsed.bindings[LockOn] = actions[LockOn].initial;
        }
        *this = parsed;
        return true;
    }

    void write(std::ostream& stream) const {
        stream << sensitivity << ' ' << invert_y;
        for (const auto& binding : bindings)
            stream << ' ' << encode_bindings(binding);
        stream << " bindings-v4";
    }
};
}  // namespace gcnllr::keyboard_mouse
