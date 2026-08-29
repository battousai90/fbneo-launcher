// src/ControllerConfig.h
#pragma once
#include <string>
#include <map>
#include <vector>

// ── Generic arcade actions ─────────────────────────────────────────────────
enum class GameAction {
    UP = 0, DOWN, LEFT, RIGHT,
    BUTTON1, BUTTON2, BUTTON3, BUTTON4, BUTTON5, BUTTON6,
    START, COIN,
    COUNT
};

constexpr int GAME_ACTION_COUNT = static_cast<int>(GameAction::COUNT);

inline const char* game_action_name(GameAction a) {
    switch (a) {
        case GameAction::UP:      return "Up";
        case GameAction::DOWN:    return "Down";
        case GameAction::LEFT:    return "Left";
        case GameAction::RIGHT:   return "Right";
        case GameAction::BUTTON1: return "Button 1";
        case GameAction::BUTTON2: return "Button 2";
        case GameAction::BUTTON3: return "Button 3";
        case GameAction::BUTTON4: return "Button 4";
        case GameAction::BUTTON5: return "Button 5";
        case GameAction::BUTTON6: return "Button 6";
        case GameAction::START:   return "Start";
        case GameAction::COIN:    return "Coin / Select";
        default:                  return "Unknown";
    }
}

inline const char* game_action_key(GameAction a) {
    switch (a) {
        case GameAction::UP:      return "up";
        case GameAction::DOWN:    return "down";
        case GameAction::LEFT:    return "left";
        case GameAction::RIGHT:   return "right";
        case GameAction::BUTTON1: return "button1";
        case GameAction::BUTTON2: return "button2";
        case GameAction::BUTTON3: return "button3";
        case GameAction::BUTTON4: return "button4";
        case GameAction::BUTTON5: return "button5";
        case GameAction::BUTTON6: return "button6";
        case GameAction::START:   return "start";
        case GameAction::COIN:    return "coin";
        default:                  return "unknown";
    }
}

// ── Single input binding ───────────────────────────────────────────────────
struct InputBinding {
    bool valid    = false;
    bool is_axis  = false;
    int  button   = -1;   // button index (!is_axis)
    int  axis     = -1;   // axis index   (is_axis)
    int  axis_dir = 0;    // +1 or -1     (is_axis)

    std::string label() const {
        if (!valid) return "—";
        if (is_axis)
            return std::string("Axis ") + std::to_string(axis)
                   + (axis_dir > 0 ? " +" : " -");
        return std::string("Button ") + std::to_string(button);
    }
};

// ── Analog inputs ──────────────────────────────────────────────────────────
// A whole family of games has no D-pad at all: Out Run steers, Arkanoid uses a
// paddle, Tempest a dial, light-gun games a pair of pointer axes. FBNeo names
// these per game — "Steering", "Paddle", "Dial", "Gun X" — so the launcher
// cannot bind them by name the way it binds "P1 Left".
//
// Roles are the answer: what an input DOES, which is stable across the dozens
// of names games give it. A role is matched to a game's input name by pattern
// (see analog_role_for_input), and carries the binding the player chose.
enum class AnalogRole { STEERING = 0, THROTTLE, BRAKE, AIM_X, AIM_Y, COUNT };

constexpr int ANALOG_ROLE_COUNT = static_cast<int>(AnalogRole::COUNT);

inline const char* analog_role_name(AnalogRole r) {
    switch (r) {
        case AnalogRole::STEERING: return "Steering / paddle / dial";
        case AnalogRole::THROTTLE: return "Accelerate / throttle";
        case AnalogRole::BRAKE:    return "Brake";
        case AnalogRole::AIM_X:    return "Aim X (gun, trackball)";
        case AnalogRole::AIM_Y:    return "Aim Y (gun, trackball)";
        default:                   return "Unknown";
    }
}

inline const char* analog_role_key(AnalogRole r) {
    switch (r) {
        case AnalogRole::STEERING: return "steering";
        case AnalogRole::THROTTLE: return "throttle";
        case AnalogRole::BRAKE:    return "brake";
        case AnalogRole::AIM_X:    return "aim_x";
        case AnalogRole::AIM_Y:    return "aim_y";
        default:                   return "unknown";
    }
}

inline AnalogRole analog_role_from_key(const std::string& k) {
    for (int i = 0; i < ANALOG_ROLE_COUNT; ++i) {
        auto r = static_cast<AnalogRole>(i);
        if (k == analog_role_key(r)) return r;
    }
    return AnalogRole::COUNT;
}

// Where an analog input reads from. Only joystick axes are wired today; the
// enum exists so a mouse or a light gun can be added later without changing
// the saved file format or the dialog's shape.
enum class AnalogSource { NONE = 0, JOY_AXIS, MOUSE_AXIS };

struct AnalogBinding {
    AnalogSource source = AnalogSource::NONE;
    int  index    = -1;      // axis number on the device
    bool invert   = false;
    // FBNeo drives an analog input in one of two ways, and the right one
    // depends on the control being emulated:
    //   absolute — the stick's position IS the wheel's position (a real wheel)
    //   relative — the stick turns a wheel that drifts back to centre, which
    //              is how a keyboard or a digital pad has to fake one
    bool relative = false;
    int  speed    = 0x800;   // relative only: how fast the axis moves it
    int  center   = 10;      // relative only: how fast it recentres
    bool is_set() const { return source != AnalogSource::NONE && index >= 0; }
};

// Sensible starting point for a standard twin-stick pad: steering and aiming
// on the left stick, pedals on the triggers. A player who disagrees changes it
// in the dialog; a player who never opens it still gets a playable Out Run.
inline AnalogBinding default_analog_binding(AnalogRole r) {
    AnalogBinding b;
    b.source = AnalogSource::JOY_AXIS;
    switch (r) {
        case AnalogRole::STEERING: b.index = 0; break;   // left stick X
        case AnalogRole::AIM_X:    b.index = 0; break;
        case AnalogRole::AIM_Y:    b.index = 1; break;   // left stick Y
        case AnalogRole::THROTTLE: b.index = 5; break;   // right trigger
        case AnalogRole::BRAKE:    b.index = 2; break;   // left trigger
        default:                   b.source = AnalogSource::NONE; break;
    }
    return b;
}

// ── Per-player configuration ───────────────────────────────────────────────
struct PlayerConfig {
    std::string device_path;   // "" = unset, "/dev/input/js0" etc.
    std::string device_name;
    std::map<GameAction, InputBinding> bindings;
    std::map<AnalogRole, AnalogBinding> analog;
};

// ── Full controller configuration (2 players) ─────────────────────────────
struct ControllerConfig {
    std::vector<PlayerConfig> players;
    ControllerConfig() { players.resize(2); }
};
