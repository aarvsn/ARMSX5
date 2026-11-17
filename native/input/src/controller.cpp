#include "armsx5/input/controller.h"

#include <cmath>

namespace armsx5::input {

MappingConfig::MappingConfig() {
    // Defaults follow Android GameController conventions:
    // A->Cross, B->Circle, X->Square, Y->Triangle (western layout),
    // plus the standard shoulder/dpad/select/start set.
    key_to_button_[96] = Button::Cross;     // KEYCODE_BUTTON_A
    key_to_button_[97] = Button::Circle;    // KEYCODE_BUTTON_B
    key_to_button_[99] = Button::Square;    // KEYCODE_BUTTON_X
    key_to_button_[100] = Button::Triangle; // KEYCODE_BUTTON_Y
    key_to_button_[102] = Button::L1;       // KEYCODE_BUTTON_L1
    key_to_button_[103] = Button::R1;       // KEYCODE_BUTTON_R1
    key_to_button_[110] = Button::L3;       // KEYCODE_BUTTON_THUMBL
    key_to_button_[111] = Button::R3;       // KEYCODE_BUTTON_THUMBR
    key_to_button_[108] = Button::Options;  // KEYCODE_BUTTON_START
    key_to_button_[109] = Button::Share;    // KEYCODE_BUTTON_SELECT
    key_to_button_[19] = Button::DpadUp;    // KEYCODE_DPAD_UP
    key_to_button_[20] = Button::DpadDown;  // KEYCODE_DPAD_DOWN
    key_to_button_[21] = Button::DpadLeft;  // KEYCODE_DPAD_LEFT
    key_to_button_[22] = Button::DpadRight; // KEYCODE_DPAD_RIGHT
}

void MappingConfig::set_button_mapping(uint32_t android_keycode, Button b) {
    key_to_button_[android_keycode] = b;
}

void MappingConfig::clear_button_mapping(uint32_t android_keycode) {
    key_to_button_.erase(android_keycode);
}

void MappingConfig::set_deadzone(float dz) {
    deadzone_ = dz < 0.0f ? 0.0f : (dz > 0.9f ? 0.9f : dz);
}

float MappingConfig::apply_deadzone(float value, float deadzone) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    const float mag = std::fabs(value);
    if (mag <= deadzone) return 0.0f;
    // Rescale the remainder so the full range is preserved after the zone.
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    return sign * (mag - deadzone) / (1.0f - deadzone);
}

void StateAggregator::on_button(uint32_t android_keycode, bool pressed) {
    auto it = mapping_.button_map().find(android_keycode);
    if (it == mapping_.button_map().end()) return;
    const uint32_t bit = 1u << static_cast<uint32_t>(it->second);
    if (pressed)
        state_.buttons |= bit;
    else
        state_.buttons &= ~bit;
    state_.connected = true;
}

void StateAggregator::on_axis(uint32_t android_axis_index, float value) {
    // Android MotionEvent axis indexes: 0=X, 1=Y, 11=Z (right stick X),
    // 14=RX (right stick Y on most pads), 23=BRAKE (L2), 22=GAS (R2).
    switch (android_axis_index) {
    case 0: state_.axes[static_cast<size_t>(Axis::LeftX)] =
                MappingConfig::apply_deadzone(value, mapping_.deadzone()); break;
    case 1: state_.axes[static_cast<size_t>(Axis::LeftY)] =
                MappingConfig::apply_deadzone(value, mapping_.deadzone()); break;
    case 11: state_.axes[static_cast<size_t>(Axis::RightX)] =
                 MappingConfig::apply_deadzone(value, mapping_.deadzone()); break;
    case 14: state_.axes[static_cast<size_t>(Axis::RightY)] =
                 MappingConfig::apply_deadzone(value, mapping_.deadzone()); break;
    case 23: state_.axes[static_cast<size_t>(Axis::L2)] =
                 (value + 1.0f) * 0.5f; break; // BRAKE reports -1..1
    case 22: state_.axes[static_cast<size_t>(Axis::R2)] =
                 (value + 1.0f) * 0.5f; break; // GAS reports -1..1
    default: break;
    }
    state_.connected = true;
}

} // namespace armsx5::input
