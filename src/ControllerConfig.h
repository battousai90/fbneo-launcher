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

// ── Per-player configuration ───────────────────────────────────────────────
struct PlayerConfig {
    std::string device_path;   // "" = unset, "/dev/input/js0" etc.
    std::string device_name;
    std::map<GameAction, InputBinding> bindings;
};

// ── Full controller configuration (2 players) ─────────────────────────────
struct ControllerConfig {
    std::vector<PlayerConfig> players;
    ControllerConfig() { players.resize(2); }
};
