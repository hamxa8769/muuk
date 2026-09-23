#include "glass_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace muuk::desktop::glass {

namespace {

bool g_dark = true;

Palette g_palette;

const Palette kDarkPalette{
    ImVec4(0.035f, 0.043f, 0.080f, 1.0f),  // bgTop
    ImVec4(0.018f, 0.022f, 0.045f, 1.0f),  // bgBottom
    ImVec4(0.42f, 0.30f, 0.98f, 0.17f),    // glowA (violet)
    ImVec4(0.05f, 0.72f, 0.78f, 0.12f),    // glowB (teal)
    ImVec4(0.90f, 0.26f, 0.66f, 0.10f),    // glowC (magenta)
    ImVec4(1.0f, 1.0f, 1.0f, 0.045f),      // cardFill
    ImVec4(1.0f, 1.0f, 1.0f, 0.10f),       // cardBorder
    ImVec4(1.0f, 1.0f, 1.0f, 0.24f),       // cardHighlight
    ImVec4(0.0f, 0.0f, 0.0f, 0.12f),       // shadow
    ImVec4(0.93f, 0.94f, 0.97f, 1.0f),     // text
    ImVec4(0.58f, 0.62f, 0.71f, 1.0f),     // textDim
    ImVec4(0.75f, 0.80f, 0.95f, 1.0f),     // neutral
};

const Palette kLightPalette{
    ImVec4(0.86f, 0.89f, 0.98f, 1.0f),
    ImVec4(0.95f, 0.93f, 0.99f, 1.0f),
    ImVec4(0.52f, 0.56f, 1.00f, 0.30f),
    ImVec4(0.32f, 0.84f, 0.90f, 0.26f),
    ImVec4(1.00f, 0.60f, 0.82f, 0.22f),
    ImVec4(1.0f, 1.0f, 1.0f, 0.52f),
    ImVec4(0.25f, 0.30f, 0.55f, 0.13f),
    ImVec4(1.0f, 1.0f, 1.0f, 0.95f),
    ImVec4(0.25f, 0.30f, 0.55f, 0.06f),
    ImVec4(0.09f, 0.11f, 0.18f, 1.0f),
    ImVec4(0.38f, 0.42f, 0.52f, 1.0f),
    ImVec4(0.30f, 0.36f, 0.58f, 1.0f),
};

constexpr float kCardRounding = 14.0f;
constexpr float kButtonRounding = 9.0f;

ImVec4 alpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

ImU32 col(ImVec4 c) { return ImGui::GetColorU32(c); }  // honours style alpha (BeginDisabled)

std::unordered_map<ImGuiID, float>& animState() {
    static std::unordered_map<ImGuiID, float> state;
    return state;
}

float animate(ImGuiID id, float target, float speed) {
    auto& state = animState();
    const auto it = state.find(id);
    if (it == state.end()) {
        state.emplace(id, target);
        return target;
    }
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    it->second += (target - it->second) * (1.0f - std::exp(-speed * dt));
    return it->second;
}

const char* visibleEnd(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? hash : label + std::strlen(label);
}

void softGlow(ImDrawList* dl, ImVec2 center, float radius, ImVec4 color) {
    constexpr int kLayers = 28;
    ImVec4 layer = color;
    layer.w = color.w * 2.0f / kLayers;
    const ImU32 c = ImGui::ColorConvertFloat4ToU32(layer);
    for (int i = 0; i < kLayers; ++i) {
        const float f = 1.0f - static_cast<float>(i) / kLayers;
        dl->AddCircleFilled(center, radius * f, c, 64);
    }
}

void drawCardBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const Palette& p = g_palette;
    const float r = kCardRounding;

    // Soft shadow as fading outlines, so the translucent interior isn't darkened.
    for (int i = 1; i <= 6; ++i) {
        const float g = static_cast<float>(i) * 2.0f;
        ImVec4 s = p.shadow;
        s.w *= 1.0f - static_cast<float>(i) / 7.0f;
        dl->AddRect(ImVec2(a.x - g, a.y - g + 3.0f), ImVec2(b.x + g, b.y + g + 3.0f), col(s), r + g, 0, 2.0f);
    }

    dl->AddRectFilled(a, b, col(p.cardFill), r);

    // Frosted sheen: stacked translucent top bands give a vertical fade.
    const float height = b.y - a.y;
    const ImVec4 sheen = g_dark ? ImVec4(1, 1, 1, 0.007f) : ImVec4(1, 1, 1, 0.035f);
    for (int k = 1; k <= 8; ++k) {
        const float h = height * 0.07f * static_cast<float>(k);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + h), col(sheen), r, ImDrawFlags_RoundCornersTop);
    }

    dl->AddRect(a, b, col(p.cardBorder), r, 0, 1.0f);
    dl->AddLine(ImVec2(a.x + r, a.y + 0.5f), ImVec2(b.x - r, a.y + 0.5f), col(p.cardHighlight), 1.0f);
}

void setStyleColors(bool dark) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;
    const Palette& p = g_palette;

    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDim;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_CheckMark] = kBlue;
    c[ImGuiCol_SliderGrab] = kBlue;
    c[ImGuiCol_SliderGrabActive] = kBlue;
    c[ImGuiCol_PlotHistogram] = kGreen;
    c[ImGuiCol_TextSelectedBg] = alpha(kBlue, 0.35f);
    c[ImGuiCol_Header] = alpha(kBlue, 0.18f);
    c[ImGuiCol_HeaderHovered] = alpha(kBlue, 0.28f);
    c[ImGuiCol_HeaderActive] = alpha(kBlue, 0.36f);
    c[ImGuiCol_TabHovered] = alpha(kBlue, 0.30f);
    c[ImGuiCol_TabSelectedOverline] = kBlue;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);

    if (dark) {
        c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.085f, 0.135f, 0.96f);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.09f);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.055f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.085f);
        c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.11f);
        c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
        c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.10f);
        c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.14f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.28f);
        c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.035f);
        c[ImGuiCol_TabSelected] = alpha(kBlue, 0.22f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.045f);
        c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.05f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.022f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.03f, 0.06f, 0.55f);
    } else {
        c[ImGuiCol_PopupBg] = ImVec4(0.97f, 0.97f, 1.0f, 0.97f);
        c[ImGuiCol_Border] = ImVec4(0.10f, 0.12f, 0.25f, 0.12f);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.60f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.78f);
        c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.92f);
        c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.55f);
        c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.75f);
        c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.90f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0, 0, 0, 0.14f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0, 0, 0, 0.22f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0, 0, 0, 0.30f);
        c[ImGuiCol_Separator] = ImVec4(0, 0, 0, 0.08f);
        c[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.35f);
        c[ImGuiCol_TabSelected] = ImVec4(1, 1, 1, 0.85f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.45f);
        c[ImGuiCol_TableBorderStrong] = ImVec4(0, 0, 0, 0.08f);
        c[ImGuiCol_TableBorderLight] = ImVec4(0, 0, 0, 0.06f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0.025f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.30f, 0.35f, 0.50f, 0.35f);
    }
}

}  // namespace

void applyTheme(bool dark) {
    g_dark = dark;
    g_palette = dark ? kDarkPalette : kLightPalette;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = kCardRounding;
    st.FrameRounding = 8.0f;
    st.PopupRounding = 12.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.ScrollbarRounding = 8.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.TabBorderSize = 0.0f;
    st.TabBarBorderSize = 0.0f;
    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.CellPadding = ImVec2(8, 6);
    st.ScrollbarSize = 10.0f;
    st.DisabledAlpha = 0.42f;
    setStyleColors(dark);
}

const Palette& palette() { return g_palette; }
bool isDark() { return g_dark; }

void drawBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const Palette& p = g_palette;
    const ImU32 top = ImGui::ColorConvertFloat4ToU32(p.bgTop);
    const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(p.bgBottom);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, top, top, bottom, bottom);

    // Slow drift -- barely perceptible, just enough to feel alive.
    const float t = static_cast<float>(ImGui::GetTime());
    softGlow(dl, ImVec2(size.x * (0.16f + 0.04f * std::sin(t * 0.13f)), size.y * (0.18f + 0.05f * std::cos(t * 0.11f))),
             size.x * 0.40f, p.glowA);
    softGlow(dl, ImVec2(size.x * (0.86f + 0.03f * std::cos(t * 0.09f)), size.y * (0.32f + 0.06f * std::sin(t * 0.12f))),
             size.x * 0.34f, p.glowB);
    softGlow(dl, ImVec2(size.x * (0.52f + 0.05f * std::sin(t * 0.07f)), size.y * (0.98f + 0.04f * std::cos(t * 0.10f))),
             size.x * 0.44f, p.glowC);
}

void beginCard(const char* id, ImVec2 size, bool autoHeight, ImGuiWindowFlags extraFlags) {
    ImGuiChildFlags childFlags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoHeight) childFlags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, size, childFlags, extraFlags | ImGuiWindowFlags_NoBackground);
}

void endCard() {
    ImGui::EndChild();
    // Drawn on the parent's draw list *after* the child: child windows are
    // always rendered after their parent, so this still lands behind the
    // card's contents -- and by now the card's final size is known.
    drawCardBackdrop(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

bool button(const char* label, ImVec4 tint, ImVec2 size, ButtonKind kind) {
    const char* end = visibleEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, end);
    const ImGuiStyle& st = ImGui::GetStyle();

    ImVec2 sz = size;
    if (sz.x == 0.0f) {
        sz.x = textSize.x + st.FramePadding.x * 2.0f + 10.0f;
    } else if (sz.x < 0.0f) {
        sz.x = std::max(4.0f, ImGui::GetContentRegionAvail().x);
    }
    if (sz.y == 0.0f) sz.y = ImGui::GetFrameHeight();

    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + sz.x, a.y + sz.y);
    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const float h = animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 14.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = kButtonRounding;

    if (h > 0.01f) {
        for (int i = 1; i <= 4; ++i) {
            const float g = static_cast<float>(i) * 1.6f;
            dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g),
                        col(alpha(tint, 0.13f * h * (1.0f - static_cast<float>(i) / 5.0f))), r + g, 0, 1.6f);
        }
    }

    const bool primary = kind == ButtonKind::Primary;
    ImVec4 fill = alpha(tint, primary ? 0.60f + 0.22f * h : 0.10f + 0.12f * h);
    if (held) fill = ImVec4(fill.x * 0.82f, fill.y * 0.82f, fill.z * 0.82f, fill.w + 0.08f);
    dl->AddRectFilled(a, b, col(fill), r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + sz.y * 0.5f), col(ImVec4(1, 1, 1, primary ? 0.14f : 0.05f)), r,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, col(alpha(tint, primary ? 0.95f : 0.30f + 0.40f * h)), r, 0, 1.0f);

    const ImVec4 textColor = primary ? ImVec4(1, 1, 1, 1) : g_palette.text;
    const ImVec2 textPos(a.x + (sz.x - textSize.x) * 0.5f, a.y + (sz.y - textSize.y) * 0.5f + (held ? 1.0f : 0.0f));
    dl->AddText(textPos, col(textColor), label, end);
    return pressed;
}

void pill(const char* text, ImVec4 color, bool pulse) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = ImGui::GetFrameHeight();
    const float dotR = 4.0f;
    const float padX = 11.0f;
    const float gap = 8.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + padX + dotR * 2 + gap + textSize.x + padX, a.y + height);

    dl->AddRectFilled(a, b, col(alpha(color, 0.14f)), height * 0.5f);
    dl->AddRect(a, b, col(alpha(color, 0.45f)), height * 0.5f, 0, 1.0f);

    const ImVec2 dot(a.x + padX + dotR, a.y + height * 0.5f);
    if (pulse) {
        const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 1.1f, 1.0f);
        dl->AddCircleFilled(dot, dotR + 7.0f * phase, col(alpha(color, 0.45f * (1.0f - phase))), 24);
    }
    dl->AddCircleFilled(dot, dotR, col(color), 16);
    dl->AddText(ImVec2(dot.x + dotR + gap, a.y + (height - textSize.y) * 0.5f), col(g_palette.text), text);
    ImGui::Dummy(ImVec2(b.x - a.x, height));
}

void progress(const char* id, float fraction, ImVec4 color, const char* overlay, float height) {
    const float shown = animate(ImGui::GetID(id), std::clamp(fraction, 0.0f, 1.0f), 8.0f);
    const float h = height > 0.0f ? height : ImGui::GetFrameHeight() * 0.95f;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = h * 0.5f;

    dl->AddRectFilled(a, b, col(g_dark ? ImVec4(1, 1, 1, 0.06f) : ImVec4(0, 0, 0, 0.06f)), r);
    if (shown > 0.002f) {
        const ImVec2 fillEnd(a.x + std::max(h, w * shown), b.y);
        dl->AddRectFilled(a, fillEnd, col(alpha(color, 0.80f)), r);
        dl->AddRectFilled(a, ImVec2(fillEnd.x, a.y + h * 0.5f), col(ImVec4(1, 1, 1, 0.16f)), r,
                          ImDrawFlags_RoundCornersTop);
    }
    dl->AddRect(a, b, col(g_palette.cardBorder), r, 0, 1.0f);
    if (overlay) {
        const ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), col(g_palette.text), overlay);
    }
    ImGui::Dummy(ImVec2(w, h));
}

float ease(const char* id, float target, float speed) { return animate(ImGui::GetID(id), target, speed); }

}  // namespace muuk::desktop::glass
