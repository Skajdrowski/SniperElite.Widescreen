#ifndef NOMINMAX
#define NOMINMAX
#undef min
#undef max
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <utility>
#include "injector/injector.hpp"
#include "injector/assembly.hpp"
#include "Patterns.h"

/*                        Sniper Elite Widescreen fix                           *
*                                                                               *
*                                                                               *
* The hooks below target the Asura engine. Layouts and function semantics were  *
* cross-referenced against AvP3_Retail.xex (same engine generation)             *
*                                                                               *
* Sniper Elite addresses in comments are from Steam executable.                 *
*                                                                               */

namespace Asura {
    // Asura_Bounding_Rectangle.
    struct Rectangle {
        float MinX, MinY;
        float MaxX, MaxY;
    };

    // Asura_ViewingArea, embedded in Asura_CameraData at +0x3C.
    struct ViewingArea {
        float Width, Height;
        float XOffset, YOffset;
    };

    // Asura_CameraData.
    struct CameraData {
        uint8_t Unknown[0x3C];
        Asura::ViewingArea ViewingArea;
        float HalfFieldOfViewX;
    };

    #pragma pack(push, 1)
    struct OverlayQuad {
        float X[4], Y[4];
        float U[4], V[4];
        uint32_t GouraudColour[4];
        int32_t TextureIndex;
        uint32_t TranslucencyMode;
        uint8_t GouraudShaded;
        uint8_t NonRotated;
    };
    #pragma pack(pop)

    // Screen-space HUD element written by the layout routine at 0x5020C0.
    // The first four fields are the element's rectangle; the last four mirror
    // the current camera ViewingArea.
    struct HudElement {
        uintptr_t* vTable;
        HudElement* Parent;
        HudElement** Children;
        int32_t ChildCount;
        uint8_t Unknown[0x08];
        float LocalX, LocalY;
        float LocalWidth, LocalHeight;
        float X, Y;
        float Width, Height;
        float XOffset, YOffset;
        float ViewingWidth, ViewingHeight;
    };

    // Minimap transform. The outer map rectangle is at +0x8C/+0x90 and the
    // inner icon rectangle is at +0x94/+0x98 (see 0x508E70).
    struct MinimapViewport {
        uint8_t Unknown[0x8C];
        float Width, Height;
        float InnerWidth, InnerHeight;
    };
}

struct Resolution {
    int32_t Width = 0, Width43 = 0;
    int32_t Height = 0;
    float fWidth = 0.0f, fWidth43 = 0.0f, fWidthRatio = 0.0f;
    float fHeight = 0.0f;
    float AspectRatio = 0.0f;
    float HudOffset = 0.0f, HudOffset43 = 0.0f, HudOffsetRatio = 0.0f;
} Res;
constexpr float Aspect43 = 4.0f / 3.0f;

// dword_7716C8 (Asura_Viewport::Width) referenced by HUD/font code.
uint32_t NativeViewportWidthAddress = 0;

// Rewrites references to the native viewport width global (dword_7716C8)
// inside a code range so the caller reads the 4:3 play-area width instead.
void RedirectViewportWidth(const uint8_t* begin, const uint8_t* end, uint32_t nativeViewportWidth, const int32_t* width) {
    uint8_t bytes[sizeof(nativeViewportWidth)];
    to_bytes(nativeViewportWidth, bytes);

    char pattern[sizeof(nativeViewportWidth) * 3 + 1]{};
    pattern_str<sizeof(nativeViewportWidth)>(bytes, pattern);

    hook::range_pattern(reinterpret_cast<uintptr_t>(begin), reinterpret_cast<uintptr_t>(end), pattern).for_each_result(
        [=](hook::pattern_match match) { injector::WriteMemory(match.get<uint32_t>(), width, true); });
}

enum class DrawMode { Game, Menu, Intel };
DrawMode Mode = DrawMode::Game;

inline bool Near(float a, float b) {
    return fabsf(a - b) < 2.0f;
}

inline std::tuple<int32_t, int32_t> GetDesktopRes() {
    // A DPI aware process gets the display mode in physical pixels; one that isn't gets the monitor's size in its own
    // (scaled) coordinates, the ones its windows use
    const auto monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info = { sizeof(info) };
    DEVMODEW mode = { .dmSize = sizeof(DEVMODEW) };
    if (GetMonitorInfoW(monitor, &info) && IsProcessDPIAware() && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0)
        return { static_cast<int32_t>(mode.dmPelsWidth), static_cast<int32_t>(mode.dmPelsHeight) };
    const int32_t width = info.rcMonitor.right - info.rcMonitor.left;
    const int32_t height = info.rcMonitor.bottom - info.rcMonitor.top;
    return width > 0 && height > 0 ? std::make_tuple(width, height) : std::make_tuple(1280, 720);
}

void SetRes(injector::reg_pack& regs) {
    *reinterpret_cast<int32_t*>(regs.esp + 4) = Res.Width;
    *reinterpret_cast<int32_t*>(regs.esp + 8) = Res.Height;
}
static SafetyHookMid SetResHook;

namespace FoV {
    constexpr auto PI = 3.14159265358979323846;
    constexpr auto RadiansPerDegree = static_cast<float>(PI / 180.0);

    // Keep the vertical FOV of the original 4:3 presentation. Asura_Camera
    // stores horizontal FOV, so widen it by the resolution/4:3 ratio.
    inline float Adjust(float fovDegrees, float aspectRatio) {
        const auto halfAngle = fovDegrees / 2.0f * RadiansPerDegree;
        const auto adjusted = 2.0f * atan((aspectRatio / Aspect43) * tan(halfAngle)) * static_cast<float>(180.0 / PI);
        return std::round(adjusted * 100.0f) / 100.0f;
    }

    void AdjustCutscene(injector::reg_pack& regs) {
        auto& fov = *reinterpret_cast<float*>(regs.esp);
        fov = Adjust(fov / RadiansPerDegree, Res.AspectRatio) * RadiansPerDegree;
    }

    void CaptureCutscene(injector::reg_pack& regs) {
        const auto* camera = reinterpret_cast<const Asura::CameraData*>(regs.eax);
        static Asura::CameraData camera43{};
        camera43.HalfFieldOfViewX = atan(tan(camera->HalfFieldOfViewX) * (Aspect43 / Res.AspectRatio));
        regs.eax = reinterpret_cast<uintptr_t>(&camera43);
    }

    static SafetyHookMid CutsceneStart, CutsceneEvent, CutsceneCapture;
}

namespace HUD {
    // Vertex used by the scope/binocular blur pass (0x49B05D); the second UV
    // set is the mask coordinate written by InitializeScope.
    struct BlurMaskVertex {
        float X, Y, Z;
        float BlurU, BlurV;
        float MaskU, MaskV;
    };

    void* (__cdecl* GetPlayerByIndex)(int32_t) = nullptr;
    uint8_t* localPlayer = nullptr;
    static SafetyHookInline PlayerArrayInit, PlayerArrayShutdown;

    int32_t __cdecl FetchPlayerPointer(int32_t playerCount) {
        const auto result = PlayerArrayInit.unsafe_ccall<int32_t>(playerCount);
        localPlayer = reinterpret_cast<uint8_t*>(GetPlayerByIndex(0));
        return result;
    }

    void __cdecl ClearPlayerPointer() {
        localPlayer = nullptr;
        PlayerArrayShutdown.unsafe_ccall<void>();
    }

    inline bool IsPlayerInSniperMode() {
        if (localPlayer == nullptr)
            return false;

        const auto playerCam = *reinterpret_cast<uint8_t**>(localPlayer + 0x64);

        // Camera mode. Null means Sniper mode
        return playerCam && *reinterpret_cast<uint32_t*>(playerCam + 4) == 0;
    }

    float TextShift = 0.0f;

    void ScaleAndAnchor(injector::reg_pack& regs) {
        auto* element = reinterpret_cast<Asura::HudElement*>(regs.esi);

        // Mirror of branch condition at 0x5020C6.
        uint8_t(__thiscall * AbsoluteLayout)(Asura::HudElement*);
        const bool absolute = !element->Parent || reinterpret_cast<decltype(AbsoluteLayout)>(element->vTable[6])(element) != 0;

        // Absolute/root layout: recentre it inside the 4:3 area
        // before the child loop below, so children inherit the result.
        if (absolute) {
            element->X += Res.HudOffset;
            element->Width -= Res.HudOffset * 2.0f;
        }

        // Relative child layout: anchor direct children of the root to the
        // display sides, while leaving centred elements alone.
        const auto* root = element->Parent;
        if (IsPlayerInSniperMode() || !root || root->Parent)
            return;

        if (fabsf(root->X - Res.HudOffset) > 1.0f || fabsf(root->Width - Res.fWidth43) > 1.0f)
            return;

        const float center = element->LocalX + element->LocalWidth / 2.0f;
        const float side = center < 0.45f ? -1.0f : center > 0.55f ? 1.0f : 0.0f;
        element->X += side * Res.HudOffset; // HUD pictures; text moves in UI::PrintLongString
    }
    static SafetyHookMid ScaleAndAnchorHook;

    inline float LineHeightRender(float x) {
        return x * Res.fWidthRatio + Res.HudOffsetRatio;
    }

    void AdjustHorizontalLineRender(injector::reg_pack& regs) {
        auto* args = reinterpret_cast<float*>(regs.esp);
        args[0] = LineHeightRender(args[0]);
        args[1] = LineHeightRender(args[1]);
    }

    void AdjustVerticalLineRender(injector::reg_pack& regs) {
        auto& x = *reinterpret_cast<float*>(regs.esp);
        x = LineHeightRender(x);
    }

    static SafetyHookMid HorizontalLineRender[3], VerticalLineRender[3];

    void AlignMinimapIcon(injector::reg_pack& regs) {
        if (Mode != DrawMode::Game)
            return;

        auto* position = reinterpret_cast<float*>(regs.eax);
        position[0] = (position[0] - Res.HudOffsetRatio) / Res.fWidthRatio;
    }

    void SquareMinimap(injector::reg_pack& regs) {
        if (Mode != DrawMode::Game)
            return;

        auto* viewport = reinterpret_cast<Asura::MinimapViewport*>(regs.esi);
        const float height = viewport->Height;
        viewport->InnerWidth += viewport->Width - height;
        viewport->Width = height;
    }

    void CenterMinimap(injector::reg_pack& regs) {
        if (Mode != DrawMode::Game)
            return;

        auto& delta = *reinterpret_cast<float*>(regs.esp + 0x14);
        delta *= Res.fWidth / Res.fWidth43;
    }

    static SafetyHookMid MinimapIcons[3], MinimapRect, MinimapCenter;
    static SafetyHookInline IntelScreen;
}

namespace Textures {
    enum Type : uint8_t { Other, Menu, Fullscreen, Scope, Binoculars };
    Type Types[250]{}; // Texture manager capacity

    static SafetyHookInline AddTextureWithResetGroupHook;

    // Asura_Texture_Management::AddTextureWithResetGroup (0x408EE0).
    int32_t __cdecl AddTextureWithResetGroup(const char* name, int32_t resetGroup, bool* created) {
        bool added = false;
        const int32_t id = AddTextureWithResetGroupHook.unsafe_ccall<int32_t>(name, resetGroup, &added);
        if (id == -1)
            return id;

        if (created)
            *created = added;

        if (added && id < _countof(Types)) {
            static constexpr struct { const char* name; Type type; } textures[] = {
                { "\\Splash\\legal.dds", Fullscreen },
                { "\\Splash\\mc2logo.dds", Fullscreen },
                { "\\Splash\\namcolog.dds", Fullscreen },
                { "\\Splash\\frontscr.dds", Menu },
                { "\\Splash\\backscr.dds", Fullscreen },
                { "\\Splash\\oldmenu1.dds", Fullscreen },
                { "\\Splash\\profskin.dds", Menu },
                { "\\Splash\\profedit.dds", Menu },
                { "\\Splash\\profsel.dds", Menu },
                { "\\Splash\\Menu25.dds", Menu },
                { "\\HUD\\mauserpc.tga", Scope },
                { "\\HUD\\naganpc.tga", Scope },
                { "\\HUD\\springpc.tga", Scope },
                { "\\HUD\\Binos.tga", Binoculars }
            };
            Types[id] = Other; // Handles can get reused after unloading
            for (const auto& texture : textures) {
                if (_stricmp(name, texture.name) == 0) {
                    Types[id] = texture.type;
                    break;
                }
            }
        }
        return id;
    }
}

namespace UI {
    bool CenteredText = false;
    float ActionManagerTextScale = 0.5f, GameplayTextObjectScale = 0.65f;
    int32_t FontWidth = 0;
    int32_t* LoadingTexture = nullptr;
    const uint8_t* LoadingImage = nullptr;

    static SafetyHookInline LoadingFrame, LoadingText, LoadingProgress, LoadingPercent;
    static SafetyHookInline MenuDraw, MenuDrawOverlay, PrintLongStringHook, GetPixelClipHook;
    static SafetyHookMid MouseUpdate;

    // Asura_Camera::SetViewingArea equivalent (0x415480): stores four floats
    // into Asura_CameraData::ViewingArea.
    void(__cdecl* SetViewingArea)(const Asura::ViewingArea*);
    // 0x4885A0, the quad queue called by the 0x48B140 wrapper.
    int32_t(__cdecl* QueueOverlayQuad)(Asura::OverlayQuad*);

    // 0x4885A0 queues a quad and 0x487E60 appends its four vertices
    // immediately; the -0.5 closes D3D8 half-pixel seams.
    int32_t QueueQuad(Asura::OverlayQuad quad) {
        for (float& x : quad.X) x -= 0.5f;
        for (float& y : quad.Y) y -= 0.5f;
        return QueueOverlayQuad(&quad);
    }

    template <DrawMode drawMode, SafetyHookInline& hook, typename Result = int32_t, typename... Args>
    Result __cdecl Draw(Args... args) {
        auto mode = std::exchange(Mode, Mode == DrawMode::Intel ? DrawMode::Intel : drawMode); // Preserve intel context in nested menus
        auto result = hook.template unsafe_ccall<Result>(args...);
        Mode = mode;
        return result;
    }

    // Solid-colour Asura_Overlay_Quad (TextureIndex = -1).
    void DrawBackdrop(uint32_t colour, bool bordersOnly = false) {
        const float width = bordersOnly ? Res.HudOffset : Res.fWidth;
        Asura::OverlayQuad background{
            .X = { 0.0f, width, width, 0.0f },
            .Y = { 0.0f, 0.0f, Res.fHeight, Res.fHeight },
            .GouraudColour = colour,
            .TextureIndex = -1,
            .GouraudShaded = 1,
            .NonRotated = 1
        };

        QueueQuad(background);
        if (bordersOnly) { // Creates another quad on the right side
            background.X[0] = background.X[3] = Res.fWidth - width;
            background.X[1] = background.X[2] = Res.fWidth;
            QueueQuad(background);
        }
    }

    void UpdateMouse(injector::reg_pack& regs) {
        auto& x = *reinterpret_cast<float*>(regs.esp + sizeof(uintptr_t));
        x -= Res.HudOffset;
    }

    // Asura_Font_System::PrintLongString (0x40F570).
    char __cdecl PrintLongString(const wchar_t* text, const Asura::Rectangle* clip, float x, float y,
        float spacing, float scale, uint32_t colour, uint32_t format) {
        auto centered = std::exchange(CenteredText, (format & 1) != 0);
        auto fontWidth = std::exchange(FontWidth, (format & 1) ? Res.Width43 : Res.Width);

        float textShift = 0.0f;
        if (CenteredText && Mode == DrawMode::Game && !HUD::IsPlayerInSniperMode()) {
            float position = x;
            if (format & 0x100) position = (clip->MinX + clip->MaxX) * 0.5f; // x is relative to the text's box
            position = position > 1.5f ? position / Res.fWidth43 : position;
            const float side = position < 0.45f ? -1.0f : position > 0.55f ? 1.0f : 0.0f;

            textShift = side * Res.HudOffset;
        }
        auto shift = std::exchange(HUD::TextShift, textShift);

        auto result = PrintLongStringHook.unsafe_ccall<char>(text, clip, x, y, spacing, scale, colour, format);
        FontWidth = fontWidth;
        HUD::TextShift = shift;
        CenteredText = centered;
        return result;
    }

    // Asura_Font_System::GetPixelClipRectAndScaleOffsets (0x40E450).
    Asura::Rectangle* __cdecl GetPixelClipRectAndScaleOffsets(Asura::Rectangle* rect, float* x, float* y, const Asura::Rectangle* clip, uint32_t format) {
        auto width = std::exchange(FontWidth, (format & 1) ? Res.Width43 : Res.Width);
        auto* result = GetPixelClipHook.unsafe_ccall<Asura::Rectangle*>(rect, x, y, clip, format);
        FontWidth = width;
        return result;
    }

    void __cdecl ProfileViewingArea(const Asura::ViewingArea* area) {
        const Asura::ViewingArea viewport{
            area->Width * Res.fWidthRatio,
            area->Height,
            area->XOffset * Res.fWidthRatio + Res.HudOffsetRatio,
            area->YOffset
        };
        SetViewingArea(&viewport);
    }

    float ScaleText() {
        const float widthScale = Res.fWidth / 1024.0f;
        const float heightScale = Res.fHeight / 768.0f;
        return heightScale / sqrtf(sqrtf((widthScale * widthScale + heightScale * heightScale) * 0.5f));
    }
}

namespace HUD {
    // Replacement for the quad queue call inside 0x48B140. Recentres the UI
    // that was authored for a 4:3 viewport and adds solid backdrops behind it.
    int32_t __cdecl SubmitQuad(Asura::OverlayQuad* quad) {
        const auto type = static_cast<uint32_t>(quad->TextureIndex) < _countof(Textures::Types) ? Textures::Types[quad->TextureIndex] : Textures::Other;
        float scale = (Mode != DrawMode::Game || UI::CenteredText) ? 1.0f : 0.0f;

        if (!scale && UI::LoadingTexture && quad->TextureIndex != -1 && Near(quad->X[0], 0.0f) && Near(quad->X[1], Res.fWidth)) {
            const bool loading = quad->TextureIndex == *UI::LoadingTexture;
            if (loading || type == Textures::Menu || type == Textures::Fullscreen)
                scale = Res.fWidth43 / Res.fWidth;

            if (type == Textures::Fullscreen || loading) // Background of loading textures isn't cleared when the game isn't running in exclusive fullscreen
                UI::DrawBackdrop(0xFF000000);
        }

        if (scale) {
            Asura::OverlayQuad centered{};
            memcpy(&centered, quad, sizeof(centered));
            for (float& x : centered.X)
                x = Res.HudOffset + x * scale + TextShift;

            if (Mode == DrawMode::Intel)
                UI::DrawBackdrop(0xFF000000, true);

            return UI::QueueQuad(centered);
        }

        // Sample scope/binos background
        const float right = Res.fWidth - Res.HudOffset;
        if (Near(quad->X[0], Res.HudOffset) && (type == Textures::Scope || type == Textures::Binoculars)) {
            Asura::OverlayQuad strip{};
            memcpy(&strip, quad, sizeof(strip));

            const bool binoculars = type == Textures::Binoculars;
            const float uWidth = quad->U[1] - quad->U[0];
            const float scopeBorder = 32.5f / 1024.0f;
            const float scopeInset = uWidth * scopeBorder;

            const float leftU = binoculars ? 0.5f / 512.0f : quad->U[0] + scopeInset;
            const float rightU = binoculars ? 0.5f / 512.0f : quad->U[1] - scopeInset;

            const float borderWidth = binoculars ? 0.0f : (right - Res.HudOffset) * scopeBorder;
            const float centerLeft = Res.HudOffset + borderWidth;
            const float centerRight = right - borderWidth;

            strip.X[0] = strip.X[3] = 0.0f;
            strip.X[1] = strip.X[2] = centerLeft;
            strip.U[0] = strip.U[1] = strip.U[2] = strip.U[3] = leftU;
            UI::QueueQuad(strip);

            strip.X[0] = strip.X[3] = centerRight;
            strip.X[1] = strip.X[2] = Res.fWidth;
            strip.U[0] = strip.U[1] = strip.U[2] = strip.U[3] = rightU;
            UI::QueueQuad(strip);

            Asura::OverlayQuad center{};
            memcpy(&center, quad, sizeof(center));

            center.X[0] = center.X[3] = centerLeft;
            center.X[1] = center.X[2] = centerRight;
            if (!binoculars) {
                center.U[0] = center.U[3] = leftU;
                center.U[1] = center.U[2] = rightU;
            }
            return UI::QueueQuad(center);
        }
        return UI::QueueQuad(*quad);
    }
}

namespace FMV {
    struct VideoSize { uint32_t Width, Height; };
    struct Stream { const VideoSize* Video; };
    Stream*** StreamStorage;
    int32_t* Handle;
    SafetyHookMid ScreenRect;

    void FitToScreen(injector::reg_pack& regs) {
        const auto* stream = (*StreamStorage)[*Handle];

        auto* rect = reinterpret_cast<float*>(regs.esp); // x, y, width, height before SetRect
        const float scale = std::min(Res.fWidth / stream->Video->Width, Res.fHeight / stream->Video->Height);
        rect[2] = stream->Video->Width * scale;
        rect[3] = stream->Video->Height * scale;
        rect[0] = (Res.fWidth - rect[2]) / 2.0f;
        rect[1] = (Res.fHeight - rect[3]) / 2.0f;
        UI::DrawBackdrop(0xFF000000); // Prevents game scene being drawn behind videos
    }
}

void GetResolution() {
    if (!Res.Width || !Res.Height)
        std::tie(Res.Width, Res.Height) = GetDesktopRes();

    Res.fWidth = static_cast<float>(Res.Width);
    Res.fHeight = static_cast<float>(Res.Height);
    Res.AspectRatio = Res.fWidth / Res.fHeight;

    Res.Width43 = static_cast<int32_t>(Res.fHeight * Aspect43);
    Res.fWidth43 = static_cast<float>(Res.Width43);
    Res.fWidthRatio = Res.fWidth43 / Res.fWidth;
    Res.HudOffset = (Res.fWidth - Res.fWidth43) / 2.0f;
    Res.HudOffset43 = Res.HudOffset / Res.fWidth43;
    Res.HudOffsetRatio = Res.HudOffset / Res.fWidth;
}

hook::pattern pattern;
void Viewport() {
    // dword_7716C4 = height, dword_7716C8 = width.
    pattern = hook::pattern("C7 ? ? ? ? ? ? ? ? ? C7 ? ? ? ? ? ? ? ? ? C7 ? ? ? ? ? ? ? ? ? C7 ? ? ? ? ? ? ? ? ? 50 A1");
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(6), Res.Height, true);
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(16), Res.Width, true);

    // 0x48B070 writer of the viewport globals.
    pattern = hook::pattern("51 8B ? ? ? 56 8B ? ? ? 57 8B ? ? ? 50");
    SetResHook = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), SetRes);

    // flt_756E50 Asura's physical screen aspect.
    pattern = hook::pattern("D9 ? ? ? 8B ? ? ? D9 ? ? ? ? ? A3 ? ? ? ? DA");
    injector::WriteMemory(*pattern.count(1).get(0).get<uint32_t*>(15), Res.AspectRatio, true);
}

void FieldOfView() {
    // 0x4A1580 multiplies the 75-degree default by flt_6FFF08 before calling
    // Asura_Camera::SetHorizontalFieldOfView (0x4153A0).
    pattern = hook::pattern("D8 ? ? ? ? ? 83 ? ? 6A ? 68 ? ? ? ? 51 D9 ? ? E8 ? ? ? ? 83 ? ? 83");
    injector::WriteMemory(*pattern.count(1).get(0).get<uint32_t*>(2), FoV::Adjust(75.0f, Res.AspectRatio), true);

    // Asura_Cutscene::Start (0x474870) and the cutscene FOV event (0x4789B0).
    pattern = hook::pattern("E8 ? ? ? ? 83 ? ? C6 ? ? ? ? ? ? F6 ? ? ? 74");
    FoV::CutsceneStart = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), FoV::AdjustCutscene);
    pattern = hook::pattern("51 51 D9 ? ? E8 ? ? ? ? 83 ? ?");
    FoV::CutsceneEvent = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(5), FoV::AdjustCutscene);

    // 0x478990 captures Asura_CameraData::HalfFieldOfViewX before starting a
    // cutscene; convert it from the widescreen aspect to 4:3.
    pattern = hook::pattern("D8 ? ? C6 ? ? ? DC ? D9");
    FoV::CutsceneCapture = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), FoV::CaptureCutscene);
}

void Hud() {
    // Absolute address of dword_7716C8 (Asura_Viewport::Width) used by HUD,
    // font and loading code. RedirectViewportWidth rewrites references to it.
    NativeViewportWidthAddress = *hook::get_pattern<uint32_t>("DB ? ? ? ? ? D8 ? D9 ? DB ? ? ? ? ? D8 ? D9", 2);

    // 0x4A1660 / 0x4A0F00 / 0x4A1970: player array access used to detect player's camera mode flag.
    pattern = hook::pattern("8B ? ? ? 85 ? 7C ? 3B ? ? ? ? ? 7D ? 8B ? ? ? ? ? 8B ? ? C3 33 ? C3 ? ? ? A0");
    HUD::GetPlayerByIndex = reinterpret_cast<decltype(HUD::GetPlayerByIndex)>(pattern.count(1).get(0).get<uint32_t>(0));
    pattern = hook::pattern("64 ? ? ? ? ? 6A ? 68 ? ? ? ? 50 8B ? ? ? 64 ? ? ? ? ? ? 53 56 A3");
    HUD::PlayerArrayInit = safetyhook::create_inline(pattern.count(1).get(0).get<uint32_t>(0), HUD::FetchPlayerPointer);
    pattern = hook::pattern("A0 ? ? ? ? 53 33 ? 3A ? 75 ? 38 ? ? ? ? ? 0F");
    if (pattern.empty())
        pattern = hook::pattern("A0 ? ? ? ? 53 33 ? 3A ? 0F ? ? ? ? ? 38");
    HUD::PlayerArrayShutdown = safetyhook::create_inline(pattern.count(1).get(0).get<uint32_t>(0), HUD::ClearPlayerPointer);

    // 0x502232: text and radar arrow scale.
    static const float HudScaling = Res.fWidthRatio * 0.0009765625f;
    pattern = hook::pattern("D8 ? ? ? ? ? D9 ? ? D9 ? ? ? D8 ? ? ? ? ? D9 ? ? ? ? ? D9 ? ? ? D8");
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(2), &HudScaling, true);

    // 0x5020C0 child loop: HUD element layout, recentred inside the 4:3 area.
    pattern = hook::pattern("8B ? ? 85 ? 74 ? 8B ? ? 33 ? 85 ? 7E ? EB ? 8D ? ? 8B");
    HUD::ScaleAndAnchorHook = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), HUD::ScaleAndAnchor);

    // 0x5E880C (compass/interaction hints) and 0x4FEDF1 (quotes/objectives).
    pattern = hook::pattern("D8 ? ? ? ? ? 50 D9 ? ? ? E8 ? ? ? ? 50");
    UI::ActionManagerTextScale *= UI::ScaleText();
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(2), &UI::ActionManagerTextScale, true);
    pattern = hook::pattern("D8 ? ? ? ? ? 89 ? ? ? 8B ? ? ? D8 ? ? ? 89");
    UI::GameplayTextObjectScale *= UI::ScaleText();
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(2), &UI::GameplayTextObjectScale, true);
}

void Scopes() {
    // Asura heart rate and scope reticle renderer (0x507C40). The line primitives are drawn
    // in the 4:3 area, so remap their endpoints.
    pattern = hook::pattern("83 ? ? 56 8B ? 8B ? ? 85 ? 57 74 ? 8B ? 85");
    auto* drawScopeReticle = pattern.count(1).get(0).get<uint8_t>(0);
    static constexpr uint32_t horizontalCalls[] = { 0x31F, 0x3BC, 0x3E9 };
    static constexpr uint32_t verticalCalls[] = { 0x33C, 0x401, 0x416 };
    for (size_t i = 0; i < _countof(HUD::HorizontalLineRender); ++i)
        HUD::HorizontalLineRender[i] = safetyhook::create_mid(drawScopeReticle + horizontalCalls[i], HUD::AdjustHorizontalLineRender);
    for (size_t i = 0; i < _countof(HUD::VerticalLineRender); ++i)
        HUD::VerticalLineRender[i] = safetyhook::create_mid(drawScopeReticle + verticalCalls[i], HUD::AdjustVerticalLineRender);

    // 0x49B05D: scope/binocular blur mask UVs cover the 4:3 area.
    pattern = hook::pattern("BE ? ? ? ? F3 ? A1 ? ? ? ? 8B");
    const float leftU =- Res.HudOffset43;
    const float rightU = 1.f + Res.HudOffset43;
    auto* vertices = reinterpret_cast<HUD::BlurMaskVertex*>(*reinterpret_cast<uint32_t*>(pattern.count(1).get(0).get<uint8_t>(1)));
    for (size_t i = 0; i < 4; ++i) {
        const float u = (i == 1 || i == 2) ? rightU : leftU;
        injector::WriteMemory(&vertices[i].MaskU, u, true);
    }
}

void Minimap() {
    // 0x50BA50 / 0x50BEF0 / 0x50BC10: minimap icon positions.
    pattern = hook::pattern("E8 ? ? ? ? 8B ? 8B ? 04 8B ? ? 89 ? ? ? 8B ? ? 8D");
    for (size_t i = 0; i < 2; ++i)
        HUD::MinimapIcons[i] = safetyhook::create_mid(pattern.count(2).get(i).get<uint32_t>(5), HUD::AlignMinimapIcon);

    uint32_t* minimapPlayers = nullptr;
    auto minimapPlayersSIG = hook::pattern("D9 ? ? ? 8B ? 8B ? ? 8B ? ? ? ? ? D8");
    if (!minimapPlayersSIG.empty())
        minimapPlayers = minimapPlayersSIG.get_first<uint32_t>();
    else
        minimapPlayers = hook::get_pattern<uint32_t>("8B ? 8B ? ? 8D ? ? ? 89 ? ? ? 50 8D ? ? ? 51");
    HUD::MinimapIcons[2] = safetyhook::create_mid(minimapPlayers, HUD::AlignMinimapIcon);

    // 0x508E70: keep the map square inside the wider viewport.
    pattern = hook::pattern("D9 ? ? ? ? ? 5E 5B 83 ? ? C3");
    HUD::MinimapRect = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(6), HUD::SquareMinimap);

    // 0x50AFE0: minimap centre delta.
    pattern = hook::pattern("D9 ? ? ? D8 ? ? ? ? ? D9 ? ? ? ? ? D8 ? ? ? ? ? D9 ? ? ? ? ? E8");
    HUD::MinimapCenter = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), HUD::CenterMinimap);
}

void IntelScreen() {
    // 0x4ADBA0: intel screen entry point; 0x4AC2B0..0x4AE290 lays out the
    // map, markers and frame using the native viewport width.
    const auto intelDraw = hook::get_pattern<uint32_t>("A0 ? ? ? ? 83 ? ? 84 ? 53 55 56 57 0F");
    uint8_t* intelItems = nullptr;
    auto intelItemsSIG = hook::pattern("A0 ? ? ? ? 83 ? ? 84 ? 75 ? A1 ? ? ? ? 85 ? 75 ? 68 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 83 ? ? 84 ? 74 ? ? A1 ? ? ? ? 85");
    if (!intelItemsSIG.empty())
        intelItems = intelItemsSIG.get_first<uint8_t>();
    else
        intelItems = hook::get_pattern<uint8_t>("A1 ? ? ? ? 83 EC ? 85 C0 0F 84 ? ? ? ? ? ? ? ? C1 E0");
    uint8_t* intelEnd = nullptr;
    auto intelEndSIG = hook::pattern("A0 ? ? ? ? 83 ? ? 84 ? 0F ? ? ? ? ? A0 ? ? ? ? 84 ? 75");
    if (!intelEndSIG.empty())
        intelEnd = intelEndSIG.get_first<uint8_t>();
    else
        intelEnd = hook::get_pattern<uint8_t>("A0 ? ? ? ? 83 ? ? 84 ? 0F ? ? ? ? ? 53 8A");
    RedirectViewportWidth(intelItems, intelEnd, NativeViewportWidthAddress, &Res.Width43); // Map, markers and frame
    HUD::IntelScreen = safetyhook::create_inline(intelDraw, UI::Draw<DrawMode::Intel, HUD::IntelScreen, char>);
}

void Menus() {
    // 0x48B140 wraps the quad queue at 0x4885A0; replace the call so every
    // HUD/menu quad can be recentred.
    pattern = hook::pattern("56 8B ? 56 E8 ? ? ? ? 8A");
    UI::QueueOverlayQuad = reinterpret_cast<decltype(UI::QueueOverlayQuad)>(injector::GetBranchDestination(pattern.count(1).get(0).get<uint32_t>(4), true).as_int());
    injector::MakeCALL(pattern.count(1).get(0).get<uint32_t>(4), HUD::SubmitQuad, true);

    // Asura_Texture_Management::AddTextureWithResetGroup (0x408EE0).
    pattern = hook::pattern("51 56 8B ? ? ? 85 ? 0F ? ? ? ? ? 80");
    Textures::AddTextureWithResetGroupHook = safetyhook::create_inline(pattern.get_first(), Textures::AddTextureWithResetGroup); // Let's DrawMode do it's work

    // 0x4819F0 / 0x4D4340 / 0x47DFC0 / 0x4AF0A0 / 0x4D5DF0 / 0x4FE850:
    // menu dispatch, font scale and frontend layout.
    const auto menuDraw = hook::get_pattern<uint8_t>("8B 0D ? ? ? ? E9 ? ? ? ? ? ? ? ? ? 8B 0D");
    const auto menuFontScale = hook::get_pattern<uint8_t>("DB ? ? ? ? ? D8 ? ? ? ? ? DB ? ? ? ? ? D8 ? ? ? ? ? D9");
    const auto menuItems = hook::get_pattern<uint8_t>("51 56 8B ? 8B ? ? 50 E8 ? ? ? ? 0F ? ? ? 8B");
    const auto frontendItems = hook::get_pattern<uint8_t>("56 57 8B ? 8B ? FF ? ? 8B ? ? 8B ? 3B");
    const auto frontendLayout = hook::get_pattern<uint8_t>("A1 ? ? ? ? 56 8B ? ? ? 50 8B");
    const auto frontendEnd = hook::get_pattern<uint8_t>("83 ? ? A1 ? ? ? ? 89 ? ? ? A0 ? ? ? ? 84 ? 0F ? ? ? ? ? A1 ? ? ? ? 84");

    RedirectViewportWidth(menuItems, menuDraw, NativeViewportWidthAddress, &Res.Width43); // Controls and hitboxes; end before menu dispatch
    RedirectViewportWidth(frontendItems, menuFontScale + 0x30, NativeViewportWidthAddress, &Res.Width43); // Frontend controls through font scale
    RedirectViewportWidth(frontendLayout, frontendEnd, NativeViewportWidthAddress, &Res.Width43); // Frontend layout through menu text sizing

    UI::MenuDraw = safetyhook::create_inline(menuDraw, UI::Draw<DrawMode::Menu, UI::MenuDraw>);
    UI::MenuDrawOverlay = safetyhook::create_inline(menuDraw + 0x10, UI::Draw<DrawMode::Menu, UI::MenuDrawOverlay, char>);

    // Asura_Font_System::GetPixelClipRectAndScaleOffsets (0x40E450) and
    // PrintLongString (0x40F570).
    const auto fontClip = hook::get_pattern<uint8_t>("8B ? ? ? 83 ? ? 53 8B ? ? ? 85 ? 56");
    const auto printText = hook::get_pattern<uint8_t>("83 ? ? A0 ? ? ? ? 84 ? 55 57");

    RedirectViewportWidth(fontClip, printText + 0x650, NativeViewportWidthAddress, &UI::FontWidth); // Clip, glyphs and text output

    UI::GetPixelClipHook = safetyhook::create_inline(fontClip, UI::GetPixelClipRectAndScaleOffsets);
    UI::PrintLongStringHook = safetyhook::create_inline(printText, UI::PrintLongString);

    // 0x4C7F24 calls 0x415480 to set the camera ViewingArea for the profile view.
    pattern = hook::pattern("8D ? ? ? 52 E8 ? ? ? ? 83 ? ? 8B ? E8");
    const auto profileViewportCall = pattern.get_first<uint32_t>(5);
    UI::SetViewingArea = reinterpret_cast<decltype(UI::SetViewingArea)>(injector::GetBranchDestination(profileViewportCall, true).as_int());
    injector::MakeCALL(profileViewportCall, UI::ProfileViewingArea, true); // Profile viewport

    // 0x481B50: mouse workspace, aligned with the 4:3 area.
    pattern = hook::pattern("A0 ? ? ? ? 83 ? ? 84 ? 0F ? ? ? ? ? DB");
    UI::MouseUpdate = safetyhook::create_mid(pattern.get_first(), UI::UpdateMouse); // Aligns cursor's workspace area

    // 0x46BD66: visible cursor size (0x757180 / 0x757184).
    pattern = hook::pattern("8B ? ? ? ? ? A1 ? ? ? ? 8B ? ? ? 52 8B");
    const float cursorScale = Res.fHeight / 960.0f;
    auto cursorHeight = *reinterpret_cast<float**>(pattern.get(0).get<uint8_t>(2));
    auto cursorWidth = *reinterpret_cast<float**>(pattern.get(0).get<uint8_t>(7));
    injector::WriteMemory(cursorWidth, *cursorWidth * cursorScale, true);
    injector::WriteMemory(cursorHeight, *cursorHeight * cursorScale, true); // Visible cursor size
}

void LoadingScreen() {
    // 0x4D49E0: loading overlay setup.
    const auto loadingImage = hook::get_pattern<uint8_t>("A1 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? 6A ? 6A");
    UI::LoadingTexture = *reinterpret_cast<int32_t**>(loadingImage + 1);

    // 0x4D4770 / 0x4D4840 / 0x4D4930 / 0x4D4CF0: frame, text, progress bar
    // and percentage overlay. Exclude the fullscreen background image.
    const auto loadingFrame = hook::get_pattern<uint8_t>("83 ? ? DB ? ? ? ? ? 56 8B ? ? ? 83");
    const auto loadingText = hook::get_pattern<uint32_t>("83 ? ? DB ? ? ? ? ? A1 ? ? ? ? 56");
    const auto loadingProgress = hook::get_pattern<uint32_t>("83 ? ? DB ? ? ? ? ? 83 ? ? DB ? ? ? ? ? 8D");
    const auto loadingPercent = hook::get_pattern<uint8_t>("83 ? ? DB ? ? ? ? ? 56 83");

    RedirectViewportWidth(loadingFrame, loadingImage, NativeViewportWidthAddress, &Res.Width43); // Frame, text and progress; exclude background image
    RedirectViewportWidth(loadingPercent, loadingPercent + 0x160, NativeViewportWidthAddress, &Res.Width43); // Percentage overlay

    UI::LoadingFrame = safetyhook::create_inline(loadingFrame, UI::Draw<DrawMode::Menu, UI::LoadingFrame, char, int32_t>);
    UI::LoadingText = safetyhook::create_inline(loadingText, UI::Draw<DrawMode::Menu, UI::LoadingText, int32_t, int32_t>);
    UI::LoadingProgress = safetyhook::create_inline(loadingProgress, UI::Draw<DrawMode::Menu, UI::LoadingProgress, int32_t, float>);
    UI::LoadingPercent = safetyhook::create_inline(loadingPercent, UI::Draw<DrawMode::Menu, UI::LoadingPercent, int32_t, float>);
}

void Cutscene() {
    // Asura_Cutscene_System::RenderWidescreenBars (0x600570). The bar height
    // comes from flt_756E50 / display aspect. Point that load at 4:3
    // to replicate authentic point of view in cutscenes.
    pattern = hook::pattern("83 ? ? DB ? ? ? ? ? A1 ? ? ? ? 83 ? ? D9");
    injector::WriteMemory(pattern.count(1).get(0).get<uint32_t>(81), &Aspect43, true); // Cutscene black bars

    // 0x5DB9C0: cutscene subtitle layout.
    pattern = hook::pattern("A1 ? ? ? ? 83 ? ? 85 ? 74 ? 8A");
    RedirectViewportWidth(pattern.count(1).get(0).get<uint8_t>(0), pattern.count(1).get(0).get<uint8_t>(196), NativeViewportWidthAddress, &Res.Width43); // Cutscene text
}

void VideoPlayback() {
    // 0x628280: FMV render; dword_818B4C is the stream handle.
    pattern = hook::pattern("A1 ? ? ? ? 83 ? ? 0F ? ? ? ? ? 50 E8 ? ? ? ? 83 ? ? 84");
    FMV::Handle = *reinterpret_cast<int32_t**>(pattern.count(1).get(0).get<uint32_t>(1));

    // 0x41367A: dword_768B84 is the stream storage array.
    pattern = hook::pattern("8B ? ? ? ? ? 8B ? ? 85 ? 74 ? 8A ? ? C3");
    FMV::StreamStorage = *reinterpret_cast<FMV::Stream****>(pattern.count(1).get(0).get<uint32_t>(2));

    // 0x6282CE: screen rectangle passed to the FMV presenter.
    pattern = hook::pattern("E8 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? 6A ? 6A ? B9 ? ? ? ? E8 ? ? ? ? A1");
    FMV::ScreenRect = safetyhook::create_mid(pattern.count(1).get(0).get<uint32_t>(0), FMV::FitToScreen);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);

        GetResolution();
        Viewport();
        FieldOfView();
        Hud();
        Scopes();
        Minimap();
        IntelScreen();
        Menus();
        LoadingScreen();
        Cutscene();
        VideoPlayback();
    }
    return TRUE;
}
