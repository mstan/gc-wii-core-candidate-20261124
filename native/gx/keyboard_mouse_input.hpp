// SPDX-License-Identifier: MIT
#pragma once

#include "keyboard_mouse.hpp"
#include "gcnllr/native_gx.h"
#include <dolphin/pad.h>
#include <algorithm>
#include <cmath>

namespace gcnllr::keyboard_mouse {

class Input {
public:
    bool start(std::istream& settings, const char* sensitivity_path = nullptr) {
        enabled_ = settings_.load(settings);
        if (enabled_ && sensitivity_path && *sensitivity_path) {
            sensitivity_path_ = std::filesystem::u8path(sensitivity_path);
            settings_.sensitivity = load_sensitivity(sensitivity_path_, settings_.sensitivity);
        }
        return enabled_;
    }

    void stop() { *this = {}; }

    bool enabled() const { return enabled_; }
    bool settings_open() const { return settings_open_; }
    int sensitivity() const { return settings_.sensitivity; }
    bool save_failed() const { return save_failed_; }
    void set_sensitivity(int percent) {
        settings_.sensitivity = std::clamp(percent, min_sensitivity, max_sensitivity);
        save_failed_ = !sensitivity_path_.empty() && !save_sensitivity(sensitivity_path_, settings_.sensitivity);
    }
    void close_settings() { settings_open_ = false; }

    bool poll(SDL_Window* window, const bool* keys, GcnllrPad& pad) {
        if (!enabled_)
            return false;
        const auto buttons = window ? SDL_GetMouseState(nullptr, nullptr) : 0;
        const bool click = (buttons & SDL_BUTTON_LMASK) != 0;
        const bool escape = keys && keys[SDL_SCANCODE_ESCAPE];
        const bool menu_toggle = escape && !escape_down_ && (remaining_polls_ || settings_open_);
        escape_down_ = escape;
        if (menu_toggle)
            settings_open_ = !settings_open_;
        if (settings_open_ || menu_toggle) {
            if (captured_ && window)
                SDL_SetWindowRelativeMouseMode(window, false);
            captured_ = capturing_click_ = false;
            previous_visor_ = next_visor_ = false;
            menu_confirm_blocked_ = true;
            mouse_was_down_ = click;
            state_ = {};
            pad = {};
            pad.stick_x = pad.stick_y = pad.substick_x = pad.substick_y = 128;
            if (remaining_polls_)
                --remaining_polls_;
            return true;
        }
        const bool confirm = held(settings_.bindings[MenuConfirm], keys, buttons);
        const bool can_capture = window && keys && remaining_polls_ && !keys[SDL_SCANCODE_ESCAPE];
        if (!can_capture) {
            const bool in_menu = remaining_polls_ == 0;
            if (captured_ && window)
                SDL_SetWindowRelativeMouseMode(window, false);
            captured_ = false;
            state_ = {};
            previous_visor_ = next_visor_ = false;
            mouse_was_down_ = click;
            remaining_polls_ = 0;
            if (in_menu && window && keys) {
                menu_confirm_blocked_ &= confirm;
                menu_input(keys, buttons, confirm && !menu_confirm_blocked_, pad);
                return true;
            }
            return false;
        }
        --remaining_polls_;
        menu_confirm_blocked_ = confirm;
        if (!captured_ && click && !mouse_was_down_) {
            captured_ = SDL_SetWindowRelativeMouseMode(window, true);
            capturing_click_ = captured_;
            SDL_GetRelativeMouseState(nullptr, nullptr);
        }
        mouse_was_down_ = click;
        if (!click)
            capturing_click_ = false;

        const auto active_buttons = captured_ ? buttons & ~(capturing_click_ ? SDL_BUTTON_LMASK : 0u) : 0u;
        const auto pressed = [&](Action action) { return held(settings_.bindings[action], keys, active_buttons); };
        const int forward = int(pressed(Forward)) - int(pressed(Back));
        const int strafe = int(pressed(StrafeRight)) - int(pressed(StrafeLeft));
        const float scale = forward && strafe ? .70710678f : 1.f;
        pad = {};
        pad.stick_x = static_cast<uint8_t>(128 + std::lround(127.f * scale * strafe));
        pad.stick_y = static_cast<uint8_t>(128 + std::lround(127.f * scale * forward));
        pad.substick_x = pad.substick_y = 128;
        state_.strafe = scale * strafe;
        if (pressed(morph_ball_ ? BoostBall : Jump)) pad.buttons |= PAD_BUTTON_B;
        if (pressed(Fire)) pad.buttons |= PAD_BUTTON_A;
        if (pressed(morph_ball_ ? PowerBomb : Missile)) pad.buttons |= PAD_BUTTON_Y;
        if (pressed(Morph)) pad.buttons |= PAD_BUTTON_X;
        state_.lock_on = !morph_ball_ && pressed(LockOn);
        if (state_.lock_on) {
            pad.buttons |= PAD_TRIGGER_L;
            pad.trigger_l = 255;
        }
        if (morph_ball_ && pressed(SpiderBall)) {
            pad.buttons |= PAD_TRIGGER_R;
            pad.trigger_r = 255;
        }
        // The game's C-stick beam handler enforces inventory and transition rules.
        if (pressed(PowerBeam)) pad.substick_y = 255;
        else if (pressed(DarkBeam)) pad.substick_x = 255;
        else if (pressed(LightBeam)) pad.substick_x = 1;
        else if (pressed(AnnihilatorBeam)) pad.substick_y = 1;
        if (pressed(Map)) pad.buttons |= PAD_TRIGGER_Z;
        if (pressed(Pause)) pad.buttons |= PAD_BUTTON_START;

        const bool previous = !morph_ball_ && pressed(PreviousVisor);
        const bool next = !morph_ball_ && pressed(NextVisor);
        const int step = int(next && !next_visor_) - int(previous && !previous_visor_);
        if (step)
            state_.visor_step = step;
        previous_visor_ = previous;
        next_visor_ = next;
        if (captured_) {
            float dx, dy;
            SDL_GetRelativeMouseState(&dx, &dy);
            if (morph_ball_)
                return true;
            constexpr float radians_per_count_at_100 = 0.00130899694f;  // 0.075 degrees.
            const float radians_per_count = (settings_.sensitivity / 100.f) * radians_per_count_at_100;
            state_.yaw -= dx * radians_per_count;
            state_.pitch += dy * radians_per_count * (settings_.invert_y ? 1.f : -1.f);
        }
        return true;
    }

    bool read(uint32_t consume, GcnllrKeyboardMouseInput& sample) {
        if (!enabled_)
            return false;
        // Allow several VI fields between game input updates, including in
        // 30 Hz modes. Menus stop calling the player input routines.
        if (!(consume & GCNLLR_MOUSE_NO_ACTIVATE))
            remaining_polls_ = 6;
        if (consume & (GCNLLR_MOUSE_COMBAT | GCNLLR_MOUSE_MORPH_BALL))
            morph_ball_ = (consume & GCNLLR_MOUSE_MORPH_BALL) != 0;
        sample = state_;
        if (consume & GCNLLR_MOUSE_TAKE_AIM)
            state_.yaw = state_.pitch = 0;
        if (consume & GCNLLR_MOUSE_TAKE_VISOR)
            state_.visor_step = 0;
        return true;
    }

private:
    void menu_input(const bool* keys, SDL_MouseButtonFlags buttons, bool confirm, GcnllrPad& pad) const {
        const auto pressed = [&](Action action) { return held(settings_.bindings[action], keys, buttons); };
        const int vertical = int(pressed(Forward)) - int(pressed(Back));
        const int horizontal = int(pressed(StrafeRight)) - int(pressed(StrafeLeft));
        if (vertical)
            pad.stick_y = static_cast<uint8_t>(128 + 100 * vertical);
        if (horizontal)
            pad.stick_x = static_cast<uint8_t>(128 + 100 * horizontal);
        if (confirm)
            pad.buttons |= PAD_BUTTON_A;
        if (pressed(MenuBack))
            pad.buttons |= PAD_BUTTON_B;
    }

    Settings settings_;
    std::filesystem::path sensitivity_path_;
    GcnllrKeyboardMouseInput state_{};
    unsigned remaining_polls_ = 0;
    bool enabled_ = false;
    bool captured_ = false;
    bool capturing_click_ = false;
    bool mouse_was_down_ = false;
    bool previous_visor_ = false;
    bool next_visor_ = false;
    bool menu_confirm_blocked_ = false;
    bool morph_ball_ = false;
    bool escape_down_ = false;
    bool settings_open_ = false;
    bool save_failed_ = false;
};
}  // namespace gcnllr::keyboard_mouse
