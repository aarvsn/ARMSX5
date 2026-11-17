#pragma once
// Guest controller input.
//
// Physical gamepads arrive through the Android GameController APIs
// (MotionEvent/KeyEvent); the JNI layer forwards events into this pure
// module so mapping logic is unit-testable on hosts. Button names use the
// DualSense layout; Android button codes follow developer.android.com
// (KEYCODE_BUTTON_A=96, etc.). No DualSense-specific features (touchpad,
// haptics, adaptive triggers) are claimed — those are unimplemented.
#include "armsx5/core/result.h"

#include <cstdint>
#include <map>

namespace armsx5::input {

enum class Button : uint32_t {
    Cross = 0, Circle, Square, Triangle,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    L1, R1, L3, R3,
    Options, Share,
    Count,
};

enum class Axis : uint32_t {
    LeftX = 0, LeftY, RightX, RightY,
    L2, R2, // analog triggers
    Count,
};

struct ControllerState {
    uint32_t buttons = 0; // bitmask of Button
    float axes[static_cast<size_t>(Axis::Count)] = {};
    bool connected = false;
};

// Android keycode -> guest button defaults.
class MappingConfig {
public:
    MappingConfig(); // installs the documented defaults

    // remap: android_keycode (e.g. 96 = KEYCODE_BUTTON_A) -> Button
    void set_button_mapping(uint32_t android_keycode, Button b);
    void clear_button_mapping(uint32_t android_keycode);
    std::map<uint32_t, Button>& button_map() { return key_to_button_; }
    const std::map<uint32_t, Button>& button_map() const { return key_to_button_; }

    float deadzone() const { return deadzone_; }
    void set_deadzone(float dz);

    // Applies a raw axis value [-1..1] with deadzone applied and rescaled.
    static float apply_deadzone(float value, float deadzone);

private:
    std::map<uint32_t, Button> key_to_button_;
    float deadzone_ = 0.12f;
};

// Aggregates raw Android events into a guest-facing ControllerState.
class StateAggregator {
public:
    explicit StateAggregator(MappingConfig& mapping) : mapping_(mapping) {}

    void on_button(uint32_t android_keycode, bool pressed);
    void on_axis(uint32_t android_axis_index, float value);
    void on_disconnect() { state_.connected = false; }
    void on_connect() { state_.connected = true; }

    const ControllerState& state() const { return state_; }

private:
    MappingConfig& mapping_;
    ControllerState state_;
};

} // namespace armsx5::input
