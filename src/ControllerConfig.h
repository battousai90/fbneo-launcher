// src/ControllerConfig.h
#pragma once
#include <cstdio>
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
// Nom de secours d'un code touche, quand on n'a pas releve le nom sur le
// clavier du joueur : ces codes sont des positions, pas des lettres, et la
// meme position porte A sur un clavier QWERTY et Q sur un AZERTY. Seules les
// touches dont le nom ne depend pas de la disposition sont nommees ici ; les
// autres s'affichent en hexadecimal plutot que d'annoncer la mauvaise lettre.
inline std::string key_label(int code) {
    switch (code) {
        case 0x01: return "Esc";
        case 0x0E: return "Backspace";
        case 0x0F: return "Tab";
        case 0x1C: return "Enter";
        case 0x1D: return "Ctrl gauche";
        case 0x2A: return "Maj gauche";
        case 0x36: return "Maj droite";
        case 0x38: return "Alt gauche";
        case 0x39: return "Espace";
        case 0x9C: return "Entree (pave)";
        case 0x9D: return "Ctrl droite";
        case 0xB8: return "Alt droite";
        case 0xC7: return "Debut";
        case 0xC8: return "Haut";
        case 0xC9: return "Page haut";
        case 0xCB: return "Gauche";
        case 0xCD: return "Droite";
        case 0xCF: return "Fin";
        case 0xD0: return "Bas";
        case 0xD1: return "Page bas";
        case 0xD2: return "Inser";
        case 0xD3: return "Suppr";
        default: break;
    }
    if (code >= 0x02 && code <= 0x0B) {                 // 1 a 9 puis 0
        const char* digits = "1234567890";
        return std::string(1, digits[code - 0x02]);
    }
    if (code >= 0x3B && code <= 0x44)                   // F1 a F10
        return "F" + std::to_string(code - 0x3B + 1);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Touche 0x%02X", code);
    return buf;
}

// D'ou vient une commande. Le clavier n'etait pas propose : un joueur sans
// manette ne pouvait rien configurer, et un joueur qui en a une ne pouvait pas
// garder deux ou trois touches au clavier a cote.
enum class InputSource { PAD = 0, KEY };

struct InputBinding {
    bool valid    = false;
    InputSource source = InputSource::PAD;
    bool is_axis  = false;
    int  button   = -1;   // button index (source PAD, !is_axis)
    int  axis     = -1;   // axis index   (source PAD, is_axis)
    int  axis_dir = 0;    // +1 or -1     (source PAD, is_axis)
    int  key      = -1;   // code touche de l'emulateur (source KEY)
    // Nom releve sur le clavier du joueur au moment de la liaison. Le code
    // seul ne suffit pas a l'ecrire : il designe une position, et la meme
    // position ne porte pas la meme lettre selon la disposition.
    std::string key_name;

    std::string label() const {
        if (!valid) return "";
        if (source == InputSource::KEY)
            return key_name.empty() ? key_label(key) : key_name;
        if (is_axis)
            return std::string("Axis ") + std::to_string(axis)
                   + (axis_dir > 0 ? " +" : " -");
        return std::string("Button ") + std::to_string(button);
    }
};

// ── Analog inputs ──────────────────────────────────────────────────────────
// A whole family of games has no D-pad at all: Out Run steers, Arkanoid uses a
// paddle, Tempest a dial, light-gun games a pair of pointer axes. FBNeo names
// these per game : "Steering", "Paddle", "Dial", "Gun X" : so the launcher
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
// MOUSE_AXIS est conserve pour ne pas invalider les configurations deja
// enregistrees, mais le pilote SDL de l'emulateur ne lit pas la souris : la
// valeur est ecrite et jamais appliquee. Le dialogue ne la propose plus.
//
// KEY_PAIR est la facon dont l'emulateur fait tourner un volant au clavier :
// deux touches, une par sens, avec une vitesse et un retour au centre. C'est
// exactement ce qu'After Burner utilise par defaut.
enum class AnalogSource { NONE = 0, JOY_AXIS, MOUSE_AXIS, KEY_PAIR };

struct AnalogBinding {
    AnalogSource source = AnalogSource::NONE;
    int  index    = -1;      // axis number on the device
    int  key_neg  = -1;      // KEY_PAIR : touche du sens negatif
    int  key_pos  = -1;      // KEY_PAIR : touche du sens positif
    std::string key_neg_name;   // releves sur le clavier du joueur, voir
    std::string key_pos_name;   // InputBinding::key_name
    bool invert   = false;
    // FBNeo drives an analog input in one of two ways, and the right one
    // depends on the control being emulated:
    //   absolute : the stick's position IS the wheel's position (a real wheel)
    //   relative : the stick turns a wheel that drifts back to centre, which
    //              is how a keyboard or a digital pad has to fake one
    bool relative = false;
    int  speed    = 0x800;   // relative only: how fast the axis moves it
    int  center   = 10;      // relative only: how fast it recentres
    bool is_set() const {
        if (source == AnalogSource::KEY_PAIR) return key_neg >= 0 && key_pos >= 0;
        return source != AnalogSource::NONE && index >= 0;
    }
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
