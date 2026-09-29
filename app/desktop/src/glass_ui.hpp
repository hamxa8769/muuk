#pragma once

#include <imgui.h>

// Small "glass" widget layer on top of Dear ImGui: an animated gradient
// backdrop, frosted translucent cards, animated buttons, pills and a
// progress bar. Everything is drawn with ImGui's own draw lists -- no extra
// dependencies, no OS-level blur -- so it stays fast on any machine.
namespace muuk::desktop::glass {

struct Palette {
    ImVec4 bgTop, bgBottom;
    ImVec4 glowA, glowB, glowC;  // soft coloured light behind the cards
    ImVec4 cardFill;
    ImVec4 cardBorder;
    ImVec4 cardHighlight;        // bright top edge of each card
    ImVec4 shadow;
    ImVec4 text, textDim;
    ImVec4 neutral;              // tint for plain (non-coloured) buttons
};

// Accent colours. applyTheme() sets them: bright on the dark theme, deeper
// on the light one so coloured text stays readable on a pale background.
inline ImVec4 kGreen(0.20f, 0.84f, 0.56f, 1.0f);
inline ImVec4 kRed(1.00f, 0.39f, 0.43f, 1.0f);
inline ImVec4 kBlue(0.40f, 0.62f, 1.00f, 1.0f);
inline ImVec4 kAmber(1.00f, 0.73f, 0.30f, 1.0f);
inline ImVec4 kViolet(0.64f, 0.52f, 1.00f, 1.0f);
inline ImVec4 kGrey(0.58f, 0.62f, 0.70f, 1.0f);
inline ImVec4 kOrange(1.00f, 0.56f, 0.18f, 1.0f);

// Selects the palette and applies matching ImGui style colours/metrics.
void applyTheme(bool dark);
[[nodiscard]] const Palette& palette();
[[nodiscard]] bool isDark();

// Full-window animated gradient + soft glows, on the background draw list.
// Call once per frame before any window.
void drawBackground();

// Lite mode, for drawing without a GPU (software renderer on a VPS): a
// still, flat look -- no glows, shadows, sheen, pulses or animations -- so
// a frame is cheap and nothing on screen asks for another one. Same colours.
void setLowPower(bool on);
[[nodiscard]] bool lowPower();

// A frosted glass card. Everything between beginCard/endCard is laid out
// inside it with normal ImGui calls. size.y == 0 with autoHeight grows to
// fit the contents; size (0,0) without autoHeight fills the remaining space.
void beginCard(const char* id, ImVec2 size, bool autoHeight = false, ImGuiWindowFlags extraFlags = 0);
void endCard();

enum class ButtonKind {
    Primary,  // strong fill in the tint colour, white label
    Soft,     // faint tinted glass, normal label
};

// An animated glass button (hover fades in, glow on hover, press dips).
// size.x: 0 = fit the label, <0 = fill the available width. Honours
// ImGui::BeginDisabled(). Returns true when clicked.
bool button(const char* label, ImVec4 tint, ImVec2 size = ImVec2(0, 0), ButtonKind kind = ButtonKind::Soft);

// Rounded status pill with a leading dot; `pulse` animates the dot.
void pill(const char* text, ImVec4 color, bool pulse = false);

// Rounded, animated progress bar (the fill eases towards `fraction`).
void progress(const char* id, float fraction, ImVec4 color, const char* overlay, float height = 0.0f);

// A bar centred on zero: a positive `value` fills green to the right (full
// at `posScale`), a negative one fills red to the left (full at
// `negScale`). Eased, so it glides as P/L moves.
void balanceBar(const char* id, float value, float negScale, float posScale, const char* overlay,
                float height = 0.0f);

// The net P/L bar: centred on zero, green grows right towards `posScale`
// (the target), red grows left towards `negScale` (the loss limit). It
// glides to each new value, its colour deepens with the size of the net,
// and every move gives a short "beat" -- a glow at the moving edge, green
// when the net rose, red when it fell, stronger for a bigger jump. No text
// inside: labels go around it so they're always readable.
void pnlBar(const char* id, float value, float negScale, float posScale, float height = 0.0f);

// One row of a side menu (the left rail): the label on the left, an optional
// badge on the right (a count, or a short word) in `badgeColor`, a soft glass
// highlight on hover and an accent bar + tint when `selected`. Fills the
// available width. Returns true when clicked.
bool railItem(const char* label, const char* badge, ImVec4 badgeColor, bool selected, ImVec4 accent,
              float height = 0.0f);

// The menu button: a rounded glass square with three bars (a "hamburger").
// `open` (0..1) morphs the bars as the menu slides out; `alert` adds a red
// dot. Returns true while the mouse is over it.
bool menuIcon(const char* id, float size, float open, bool alert);

// Eases a displayed value towards `target` (per-id), for smooth numbers.
[[nodiscard]] float ease(const char* id, float target, float speed = 10.0f);

}  // namespace muuk::desktop::glass
