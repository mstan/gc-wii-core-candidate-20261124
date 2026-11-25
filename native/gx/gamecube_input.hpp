// SPDX-License-Identifier: MIT
#pragma once

#include "gcnllr/native_gx.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace gcnllr::gamecube_input {

enum Control {
    MainLeft, MainRight, MainUp, MainDown, CLeft, CRight, CUp, CDown,
    LAnalog, RAnalog, LClick, RClick, A, B, X, Y, Z, Start,
    DLeft, DRight, DUp, DDown, ControlCount
};

inline constexpr std::array<const char*, ControlCount> labels{
    "Control Stick Left", "Control Stick Right", "Control Stick Up", "Control Stick Down",
    "C Stick Left", "C Stick Right", "C Stick Up", "C Stick Down",
    "L Analog", "R Analog", "L Click", "R Click", "A", "B", "X", "Y", "Z", "Start",
    "D-pad Left", "D-pad Right", "D-pad Up", "D-pad Down",
};

enum class Kind { None, Key, Button, PositiveAxis, NegativeAxis };
struct Binding {
    Kind kind = Kind::None;
    int code = 0;
};
using Mapping = std::array<Binding, ControlCount>;

inline Mapping keyboard_defaults() {
    constexpr std::array<int, ControlCount> keys{
        SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_K,
        SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_D,
        SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_A,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_8,
        SDL_SCANCODE_KP_2,
    };
    Mapping result;
    for (size_t i = 0; i < keys.size(); ++i)
        result[i] = {Kind::Key, keys[i]};
    return result;
}

inline Mapping gamepad_defaults() {
    return {{
        {Kind::NegativeAxis, SDL_GAMEPAD_AXIS_LEFTX}, {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_LEFTX},
        {Kind::NegativeAxis, SDL_GAMEPAD_AXIS_LEFTY}, {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_LEFTY},
        {Kind::NegativeAxis, SDL_GAMEPAD_AXIS_RIGHTX}, {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHTX},
        {Kind::NegativeAxis, SDL_GAMEPAD_AXIS_RIGHTY}, {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHTY},
        {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
        {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
        {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
        {Kind::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
        {Kind::Button, SDL_GAMEPAD_BUTTON_SOUTH}, {Kind::Button, SDL_GAMEPAD_BUTTON_EAST},
        {Kind::Button, SDL_GAMEPAD_BUTTON_WEST}, {Kind::Button, SDL_GAMEPAD_BUTTON_NORTH},
        {Kind::Button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {Kind::Button, SDL_GAMEPAD_BUTTON_START},
        {Kind::Button, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {Kind::Button, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {Kind::Button, SDL_GAMEPAD_BUTTON_DPAD_UP}, {Kind::Button, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    }};
}

struct Port {
    std::string device;
    std::string name;
    Mapping keyboard = keyboard_defaults();
    Mapping gamepad = gamepad_defaults();
};
using Profile = std::array<Port, 4>;

inline Profile defaults() {
    Profile result;
    result.front().device = "keyboard";
    return result;
}

inline bool valid_binding(int kind, int code, bool keyboard) {
    if (code < 0)
        return false;
    switch (static_cast<Kind>(kind)) {
        case Kind::None: return code == 0;
        case Kind::Key: return keyboard && code > 0 && code < SDL_SCANCODE_COUNT;
        case Kind::Button: return !keyboard && code < SDL_GAMEPAD_BUTTON_COUNT;
        case Kind::PositiveAxis:
        case Kind::NegativeAxis: return !keyboard && code < SDL_GAMEPAD_AXIS_COUNT;
    }
    return false;
}

inline bool load(const std::filesystem::path& path, Profile& profile) {
    std::ifstream file(path);
    int version;
    if (!(file >> version) || version != 1)
        return false;
    Profile candidate;
    for (auto& port : candidate) {
        if (!(file >> std::quoted(port.device) >> std::quoted(port.name)))
            return false;
        for (Mapping* map : {&port.keyboard, &port.gamepad}) {
            for (auto& binding : *map) {
                int kind, code;
                if (!(file >> kind >> code) || !valid_binding(kind, code, map == &port.keyboard))
                    return false;
                binding = {static_cast<Kind>(kind), code};
            }
        }
    }
    profile = std::move(candidate);
    return true;
}

inline void serialize(std::ostream& output, const Profile& profile) {
    output << "1\n";
    for (const auto& port : profile) {
        output << std::quoted(port.device) << ' ' << std::quoted(port.name) << '\n';
        for (const Mapping* map : {&port.keyboard, &port.gamepad}) {
            for (const auto& binding : *map)
                output << static_cast<int>(binding.kind) << ' ' << binding.code << ' ';
            output << '\n';
        }
    }
}

inline bool save(const std::filesystem::path& path, const Profile& profile) {
    auto pending = path;
    pending += "." + std::to_string(SDL_GetPerformanceCounter()) + ".tmp";
    std::ofstream file(pending);
    serialize(file, profile);
    file.close();
    const auto from = pending.u8string();
    const auto to = path.u8string();
    const auto* from_bytes = reinterpret_cast<const char*>(from.c_str());
    const auto* to_bytes = reinterpret_cast<const char*>(to.c_str());
    if (file && SDL_RenamePath(from_bytes, to_bytes))
        return true;
    std::error_code error;
    std::filesystem::remove(pending, error);
    return false;
}

struct Device {
    SDL_JoystickID id;
    std::string key;
    std::string name;
};

inline std::vector<Device> connected_devices() {
    std::vector<Device> result;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
        if (!pad)
            continue;
        char guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof(guid));
        const char* serial = SDL_GetGamepadSerial(pad);
        const char* path = SDL_GetGamepadPath(pad);
        const char* name = SDL_GetGamepadName(pad);
        std::string key = guid;
        // Paths distinguish USB ports even when two pads report the same
        // model GUID and serial. This key is independent of connection order.
        if (path && *path)
            key += std::string(":path:") + path;
        else if (serial && *serial)
            key += std::string(":serial:") + serial;
        result.push_back({ids[i], std::move(key), name && *name ? name : "Gamepad"});
        SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
    return result;
}

inline int amount(Binding binding, const bool* keys, SDL_Gamepad* pad) {
    switch (binding.kind) {
        case Kind::None: return 0;
        case Kind::Key: return keys && keys[binding.code] ? 32767 : 0;
        case Kind::Button:
            return pad && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(binding.code)) ? 32767 : 0;
        case Kind::PositiveAxis:
        case Kind::NegativeAxis: {
            if (!pad)
                return 0;
            const int raw = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(binding.code));
            return std::clamp(binding.kind == Kind::NegativeAxis ? -raw : raw, 0, 32767);
        }
    }
    return 0;
}

inline GcnllrPad sample(const Mapping& map, const bool* keys, SDL_Gamepad* pad) {
    std::array<int, ControlCount> values;
    for (size_t i = 0; i < map.size(); ++i)
        values[i] = amount(map[i], keys, pad);
    const auto axis = [&](Control negative, Control positive) -> uint8_t {
        const int value = values[positive] - values[negative];
        const int deadzone = pad ? 4096 : 0;
        return std::abs(value) <= deadzone ? 128 : static_cast<uint8_t>(128 + value / 256);
    };
    constexpr std::array<std::pair<Control, uint16_t>, 12> buttons{{
        {A,0x100}, {B,0x200}, {X,0x400}, {Y,0x800}, {Start,0x1000}, {Z,0x10},
        {LClick,0x40}, {RClick,0x20}, {DLeft,1}, {DRight,2}, {DUp,8}, {DDown,4},
    }};
    GcnllrPad result{};
    for (const auto [control, mask] : buttons)
        if (values[control] >= (control == LClick || control == RClick ? 29440 : 16384))
            result.buttons |= mask;
    result.stick_x = axis(MainLeft, MainRight);
    result.stick_y = axis(MainDown, MainUp);
    result.substick_x = axis(CLeft, CRight);
    result.substick_y = axis(CDown, CUp);
    result.trigger_l = static_cast<uint8_t>(values[LAnalog] / 128);
    result.trigger_r = static_cast<uint8_t>(values[RAnalog] / 128);
    return result;
}

class ControllerPorts {
public:
    bool start(const char* path) {
        enabled_ = load(std::filesystem::u8path(path), profile_);
        return enabled_;
    }

    void stop() {
        for (auto*& pad : pads_)
            release(pad);
        enabled_ = false;
        next_probe_ = 0;
        input_ = {};
    }

    bool enabled() const { return enabled_; }
    const GcnllrGameCubeInput& input() const { return input_; }
    GcnllrPad& port_input(size_t port) { return input_.ports[port]; }

    int keyboard_port() const {
        for (size_t i = 0; i < profile_.size(); ++i)
            if (profile_[i].device == "keyboard")
                return static_cast<int>(i);
        return -1;
    }

    void poll(const bool* keys) {
        for (auto*& pad : pads_)
            if (pad && !SDL_GamepadConnected(pad))
                release(pad);
        const uint64_t now = SDL_GetTicks();
        if (now >= next_probe_) {
            next_probe_ = now + 250;
            reconnect();
        }
        input_.connected = 0;
        for (size_t i = 0; i < profile_.size(); ++i) {
            const Port& port = profile_[i];
            if (port.device == "keyboard" || pads_[i])
                input_.connected |= 1u << i;
            input_.ports[i] = port.device == "keyboard"
                ? sample(port.keyboard, keys, nullptr)
                : sample(port.gamepad, nullptr, pads_[i]);
        }
    }

private:
    static void release(SDL_Gamepad*& device) {
        if (device)
            SDL_CloseGamepad(device);
        device = nullptr;
    }

    void reconnect() {
        std::array<bool, 4> missing{};
        for (size_t i = 0; i < profile_.size(); ++i) {
            const std::string& key = profile_[i].device;
            missing[i] = !pads_[i] && !key.empty() && key != "keyboard";
        }
        if (std::none_of(missing.begin(), missing.end(), [](bool value) { return value; }))
            return;
        const auto devices = connected_devices();
        for (size_t i = 0; i < profile_.size(); ++i) {
            if (!missing[i])
                continue;
            const auto matches = [&](const Device& device) { return device.key == profile_[i].device; };
            auto candidate = std::find_if(devices.begin(), devices.end(), matches);
            if (candidate == devices.end() || std::find_if(candidate + 1, devices.end(), matches) != devices.end())
                continue;
            const auto assigned = [&](SDL_Gamepad* pad) {
                return pad && SDL_GetGamepadID(pad) == candidate->id;
            };
            if (std::none_of(pads_.begin(), pads_.end(), assigned))
                pads_[i] = SDL_OpenGamepad(candidate->id);
        }
    }

    Profile profile_;
    std::array<SDL_Gamepad*, 4> pads_{};
    uint64_t next_probe_ = 0;
    bool enabled_ = false;
    GcnllrGameCubeInput input_{};
};
}
