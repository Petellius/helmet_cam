// Helmet Cam (spike): a first-person picture-in-picture from an operator's head.
//
// How it works (one safe hook, no patches):
//   GameRenderer::RenderGame(viewport, camera)   the game's whole frame, camera and viewport are plain arguments.
//   Pre : while the cam is open, call RenderGame ourselves first with a camera we build at the operator's head, then
//         copy the finished image from the back buffer into our own texture (shrunk by the blit). The game's real
//         RenderGame call follows and overwrites the back buffer with the normal view.
//   Post: blit our texture into a corner of the back buffer, before the game draws its GUI over it.
//
// Spike scope: fixed top-right corner, one operator at a time (hold the key to open or close, tap to cycle),
// per-second operator logging to find out what a replay keeps alive. Everything else is later.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "dk2ml.hpp"

DK2ML_PLUGIN_MANIFEST(DK2ML_API_VERSION, "Helmet Cam", "0.1.0", "Petellius", "", 112);

namespace {

const DK2ML_API* g_api = nullptr;

struct Vec3 {
    float x, y, z;
};

// --- the game, by name (checked with symtest --find / --type) ---
dk2ml::Global<void*> g_pGameClient{"g_pGameClient"};
dk2ml::Global<void*> g_pRenderer{"g_pRenderer"};
dk2ml::Field<int> GameClient_m_state{"GameClient", "m_state"};
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

// void GameRenderer::RenderGame(const int viewport[4], const Camera& camera)
dk2ml::Fn<void(void* renderer, const int* viewport, const void* camera)> RenderGame{"GameRenderer::RenderGame"};

// Camera. SetProjectionPerspective(width, height, fov, near, far) is called exactly like this by the game's own
// GameClient::ToggleFPSCamera (FOV 90, near 0.01, far 100, the viewport size).
dk2ml::TypeSize Camera_size{"Camera"};
dk2ml::Fn<void(void* camera)> Camera_SetDefaults{"Camera::SetDefaults"};
dk2ml::Fn<void(void* camera, float width, float height, float fov, float nearPlane, float farPlane)>
    Camera_SetProjectionPerspective{"Camera::SetProjectionPerspective"};
dk2ml::Field<Vec3> Camera_m_pos{"Camera", "m_pos"};
dk2ml::Field<Vec3> Camera_m_actualPos{"Camera", "m_actualPos"};
dk2ml::Field<Vec3> Camera_m_rotAngles{"Camera", "m_rotAngles"};
dk2ml::Field<float> Camera_m_matView{"Camera", "m_matView"}; // Matrix: Vector4[4], row-major, translation in .w

// Humans: a static list of every Human_Client (LinkedList<Human_Client>: head, next, prev, owner).
dk2ml::Global<uint8_t> Human_typeList{"Human_Client::typeList"};
dk2ml::Field<void*> LinkedList_head{"LinkedList<Human_Client>", "head"};
dk2ml::Field<void*> LinkedList_next{"LinkedList<Human_Client>", "next"};
dk2ml::Field<void*> LinkedList_owner{"LinkedList<Human_Client>", "owner"};
dk2ml::Field<bool> Human_isControlable{"Human_Client", "m_isControlable"};
dk2ml::Field<bool> Human_isCrouched{"Human_Client", "m_isCrouched"};
dk2ml::Field<uint32_t> Human_ownerMask{"Human_Client", "m_ownerMask"};
dk2ml::Field<Vec3> Human_headDirection{"Human_Client", "m_headDirection"};
dk2ml::Field<uint32_t> Entity_id{"Entity_Common", "m_id"};
dk2ml::Field<float> Entity_health{"Entity_Common", "m_health"};
dk2ml::Field<Vec3> Entity_origin{"Entity_Common", "m_origin"};
dk2ml::Field<char*> Entity_name{"Entity_Common", "m_szName"};

// Skeleton, to log where the head bone is (the camera itself uses origin + eye height for now).
dk2ml::Field<void*> Entity_animManager{"Entity_Client", "m_pAnimManager"};
dk2ml::Field<uint8_t> AnimManager_skeleton{"KHM::CAnimationManager", "m_skeleton"};
dk2ml::Field<int> Skeleton_numNodes{"KHM::sSkeleton", "numNodes"};
dk2ml::Field<uint8_t> Skeleton_nodes{"KHM::sSkeleton", "nodes"};
dk2ml::TypeSize SkeletonNode_size{"KHM::sSkeletonNode"};
dk2ml::Field<char> SkeletonNode_name{"KHM::sSkeletonNode", "sNodeName"}; // StaticString<48>: the chars come first
dk2ml::Field<float> SkeletonNode_matGlobal{"KHM::sSkeletonNode", "matGlobal"}; // Matrix3x4: 3 x Vector4

// The entity's own basis, to turn the head bone (entity-local) into a world position.
dk2ml::Field<Vec3> Entity_left{"Entity_Common", "m_left"};
dk2ml::Field<Vec3> Entity_up{"Entity_Common", "m_up"};
dk2ml::Field<Vec3> Entity_forward{"Entity_Common", "m_forward"};
dk2ml::Field<float> Entity_scale{"Entity_Common", "m_scale"};

// Automatic switching. The engagement detection follows era's Lights and Lasers (MIT,
// github.com/gabrweu/doorkickers-visible-lasers, src/lasers/Engagement.cpp): the client has no "target", so an operator
// is engaging on each shot (EV_HUMAN_FIREARM_SHOT, one per round, also in replays) and, optionally, after aiming at an
// enemy it can see for a moment; it stays engaged for a while after the last shot or sighting.
dk2ml::Global<void*> g_pGameInput{"g_pGameInput"};
dk2ml::Fn<void*(const void* input)> GameInput_GetSelectedHuman{"GameInput::GetSelectedHuman"};
dk2ml::Field<int> GameCommon_gameTime{"GameCommon", "m_gameTime"}; // ms; stops while paused, slows with slow-mo
dk2ml::Field<void*> Entity_pTemplate{"Entity_Common", "m_pTemplate"};
dk2ml::Field<int> HumanTemplate_type{"Human_Template", "type"};    // eHumanType
dk2ml::Field<Vec3> Human_aimDirection{"Human_Client", "m_aimDirection"};
dk2ml::Field<bool> Human_isSurrendered{"Human_Client", "m_isSurrendered"};
dk2ml::Field<int> Human_currentCommand{"Human_Client", "m_currentCommand"}; // eHumanCommand
dk2ml::Field<bool> Entity_hiddenInFow{"Entity_Client", "m_bHiddenInFOW"};
dk2ml::Enum HUMAN_GOODGUY{"eHumanType", "HUMAN_GOODGUY"};
dk2ml::Enum HUMAN_BADGUY{"eHumanType", "HUMAN_BADGUY"};
dk2ml::Enum EV_HUMAN_FIREARM_SHOT{"Entity_Common::eEntityEvent", "EV_HUMAN_FIREARM_SHOT"};
dk2ml::Enum HUMAN_CMD_SHOTGUN_DOOR{"eHumanCommand", "HUMAN_CMD_SHOTGUN_DOOR"};
dk2ml::Enum COL_VISION_BLOCK{"eCollisionFlags", "COL_VISION_BLOCK"};
// bool GameCommon::HasLineOfSight(const Vector3& eye, const Vector3& dir, float coneDeg, const Entity_Common* target,
//                                 unsigned blockFlags, bool)  (static)
dk2ml::Fn<bool(const Vec3* eye, const Vec3* dir, float coneDeg, const void* target, uint32_t blockFlags, bool flag)>
    GameCommon_HasLineOfSight{"?HasLineOfSight@GameCommon@@SA_NAEBVVector3@@0MPEBVEntity_Common@@I_N@Z"};
// void Human_Client::Net_ReceiveEvent(eNetworkMask, eEntityEvent, unsigned size, const BitPacker&): argument 2 is the event
dk2ml::Fn<void(void* human, int mask, int event, unsigned size, const void* packer)> Human_Net_ReceiveEvent{
    "?Net_ReceiveEvent@Human_Client@@UEAAXW4eNetworkMask@@W4eEntityEvent@Entity_Common@@IAEBVBitPacker@@@Z"};

// The operator's name for the label. Human_Client::m_humanId (sHumanId) starts with the personal name the HUD shows (its
// StaticString<48> chars are at offset 0). Without one: the unit name (Entity_Common::m_szName), then the template's unit
// and class names. A string starting with '@' is a localization key. Loca::GetTextForId(key, logIfMissing, ?) returns the
// text, or null when the key isn't found (it also returns null for a string without '@').
dk2ml::Field<char> Human_humanId{"Human_Client", "m_humanId"};
dk2ml::Field<char> HumanTemplate_unitName{"Human_Template", "unitName"};
dk2ml::Field<char> HumanTemplate_className{"Human_Template", "className"};
dk2ml::Fn<const char*(const char* key, bool logIfMissing, bool flag)> Loca_GetTextForId{
    "?GetTextForId@Loca@@YAPEBDPEBD_N1@Z"};

// ToggleFOW flips the game's global "fog of war disabled" flag (the game's own free-fly FPS camera calls it when it
// starts and stops), so it is called once before the nested pass and once after.
dk2ml::Fn<void(void* renderer)> GameRenderer_ToggleFOW{"GameRenderer::ToggleFOW"};
// The replay's "cinematic mode" button flips m_renderOptions.plans (found in GameGUI::OnEvent for
// GUI_GAME_REPLAY_TOGGLE_CINEMATIC_MODE). Off hides the drawn paths and waypoints, and BuildRenderLists then skips every
// render object whose template says hideInCinematicMode (charge and door highlights among them).
dk2ml::Field<uint8_t> GameRenderer_renderOptions{"GameRenderer", "m_renderOptions"};
dk2ml::Field<bool> RenderOptions_plans{"GameRenderer::sRenderOptions", "plans"};
// The night-vision look the renderer applies on night maps (sRenderOptions +80, next to colorCorrection).
dk2ml::Field<bool> RenderOptions_nvg{"GameRenderer::sRenderOptions", "nvg"};

// The red doors: an entity that an order will open, kick or destroy gets the game's "red ghost" look. The game does it in
// Entity_Client::SetRedGhostEffect(entity, on) by rewriting each of the entity's render objects: alpha blending (blend
// mode 1) and a diffuse colour of (2, 0, 0, 0.5). The plans flag does not undo that, so the helmet pass undoes it for the
// doors and puts it back afterwards, using the game's own function both ways.
dk2ml::Fn<void(void* entity, bool on)> Entity_SetRedGhostEffect{"Entity_Client::SetRedGhostEffect"};
dk2ml::Global<uint8_t> Door_typeList{"Door_Client::typeList"};
dk2ml::Field<void*> DoorList_head{"LinkedList<Door_Client>", "head"};
dk2ml::Field<void*> DoorList_next{"LinkedList<Door_Client>", "next"};
dk2ml::Field<void*> DoorList_owner{"LinkedList<Door_Client>", "owner"};
dk2ml::Field<uint8_t> Entity_renderObjects{"Entity_Client", "m_renderObjects"}; // List<unsigned int>: render object ids
dk2ml::Field<void*> ListId_data{"List<unsigned int>", "m_list"};
dk2ml::Field<int> ListId_count{"List<unsigned int>", "m_elements"};
dk2ml::Field<uint8_t> GameRenderer_renderObjectsPool{"GameRenderer", "m_renderObjectsPool"}; // List<sRenderObject>
dk2ml::Field<void*> ListObject_data{"List<sRenderObject>", "m_list"};
dk2ml::Field<int> ListObject_count{"List<sRenderObject>", "m_elements"};
dk2ml::TypeSize sRenderObject_size{"sRenderObject"};
dk2ml::Field<int> sRenderObject_blendMode{"sRenderObject", "blendMode"};
dk2ml::Field<uint8_t> sRenderObject_data{"sRenderObject", "data"};
dk2ml::Field<float> sRenderObjectData_diffuseColor{"sRenderObjectData", "diffuseColor"};
// void GetShadowMapCameraParams(const Camera& view, Camera& out, Matrix& matrix) const: fits the sun's ortho shadow box
// around `view`'s frustum.
dk2ml::Fn<void(void* renderer, const void* view, void* out, void* matrix)> GetShadowMapCameraParams{
    "GameRenderer::GetShadowMapCameraParams"};

// The game's options (loaded from options.xml).
dk2ml::Global<uint8_t> Options_game{"Options::game"};
// The game's "Colored plans" option (0 No, 1 Individual, 2 Class) and the colour it gives each operator's path:
// Human_Client::GetColor() returns the table colour (data/gameplay_settings/team_colors.xml) as 0xAABBGGRR, white when
// plans aren't coloured.
dk2ml::Field<int> OptionsGame_coloredPlans{"Options::sGame", "coloredPlans"};
dk2ml::Fn<uint32_t(const void* human)> Human_GetColor{"Human_Client::GetColor"};

// OpenGL: the game loads the framebuffer entry points itself, so use its pointers (opengl32 only exports GL 1.1).
using PFN_GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_DeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using PFN_BlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
dk2ml::Global<PFN_GenFramebuffers> gl_GenFramebuffers{"glGenFramebuffers"};
dk2ml::Global<PFN_DeleteFramebuffers> gl_DeleteFramebuffers{"glDeleteFramebuffers"};
dk2ml::Global<PFN_BindFramebuffer> gl_BindFramebuffer{"glBindFramebuffer"};
dk2ml::Global<PFN_FramebufferTexture2D> gl_FramebufferTexture2D{"glFramebufferTexture2D"};
dk2ml::Global<PFN_CheckFramebufferStatus> gl_CheckFramebufferStatus{"glCheckFramebufferStatus"};
dk2ml::Global<PFN_BlitFramebuffer> gl_BlitFramebuffer{"glBlitFramebuffer"};

// OpenGL's own constants (fixed by the GL spec, not by the game build).
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kTextureBinding2D = 0x8069;
constexpr GLenum kReadBuffer = 0x0C02;
constexpr GLenum kSamples = 0x80A9;
constexpr GLenum kScissorTest = 0x0C11;

// --- settings: packaged defaults (mod\defaults.ini, read only) -> the player's copy (<settings folder>\settings.ini) ---
struct Settings {
    bool enabled = true;
    int key = 0x48; // H (T is the game's troop-list key)
    int holdMs = 250;
    float fov = 90.0f;           // degrees, the game's own free-fly camera uses 90
    float offsetForward = 0.10f; // metres ahead of the entity, along the gaze
    float offsetUp = 0.0f;       // metres added to the head height (1.58 standing, 1.14 crouched)
    int testPicture = 0;         // debug: 0 helmet view, 1 mirror of the main view, 2 solid color
    int anchor = 0;              // corner of the window the picture sits in: 0 top right, 1 top left, 2 bottom left, 3 bottom right
    int offsetX = 16;            // pixels in from that corner
    int offsetY = 16;
    int sizePercent = 25;        // picture width as a percentage of the window width
    bool hideNightVision = false; // no night-vision overlay in the helmet picture
    bool colorName = true;       // ...on a chip in the colour of the operator's path, while Colored plans is on
    bool fowOff = false;         // fog of war off for the helmet pass (the game's own FPS camera does this too)
    bool hidePlans = true;       // the replay's "cinematic mode" for the helmet pass only: no paths, waypoints, highlights
    // Automatic switching of the cam's operator.
    int selectionMode = 0;                 // 0: a tap of the key switches to the operator the player selected; 1: the cam follows the selection at once (missions only)
    bool engageSwitch = false;             // switch to an operator who is engaging when the cam's one isn't
    int cooldownMs = 800;                // after any switch, engagement switching waits this long (game time)
    int engageHoldMs = 1000;              // the cam's operator stays "engaging" this long after their last shot or sighting
    bool engageOnAim = true;               // aiming at an enemy the operator can see counts, not just shots
    int aimSettleMs = 400;                 // ...once the aim has been held on that enemy this long
    bool logOperators = false;
    bool allowAllHumans = false; // enemies, civilians and hostages count as operators too (their own helmet cams)
};

Settings g_defaults;
Settings g_settings;
std::wstring g_settingsFile; // empty: no settings folder, defaults only

const wchar_t* kSection = L"helmet_cam";

int ReadInt(const std::wstring& file, const wchar_t* key, int fallback)
{
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(kSection, key, L"", buf, 32, file.c_str());
    return buf[0] ? static_cast<int>(wcstol(buf, nullptr, 0)) : fallback; // base 0: 0x54 or 84
}

void ReadFloat(const std::wstring& file, const wchar_t* key, float& value)
{
    wchar_t buf[32] = {}; // the profile API has no float reader
    GetPrivateProfileStringW(kSection, key, L"", buf, 32, file.c_str());
    if (buf[0]) {
        value = static_cast<float>(_wtof(buf));
    }
}

void ReadIni(const std::wstring& file, Settings& s)
{
    ReadFloat(file, L"fov", s.fov);
    ReadFloat(file, L"offset_forward", s.offsetForward);
    ReadFloat(file, L"offset_up", s.offsetUp);
    s.fov = std::clamp(s.fov, 30.0f, 120.0f);
    s.offsetForward = std::clamp(s.offsetForward, -0.5f, 1.0f);
    s.offsetUp = std::clamp(s.offsetUp, -0.5f, 0.5f);
    s.enabled = ReadInt(file, L"enabled", s.enabled) != 0;
    s.key = ReadInt(file, L"key", s.key);
    s.holdMs = std::clamp(ReadInt(file, L"hold_ms", s.holdMs), 100, 1000);
    s.testPicture = std::clamp(ReadInt(file, L"test_picture", s.testPicture), 0, 2);
    s.anchor = std::clamp(ReadInt(file, L"anchor", s.anchor), 0, 3);
    s.offsetX = std::clamp(ReadInt(file, L"offset_x", s.offsetX), 0, 400);
    s.offsetY = std::clamp(ReadInt(file, L"offset_y", s.offsetY), 0, 400);
    s.sizePercent = std::clamp(ReadInt(file, L"size_percent", s.sizePercent), 10, 60);
    s.hideNightVision = ReadInt(file, L"hide_night_vision", s.hideNightVision) != 0;
    s.colorName = ReadInt(file, L"color_name", s.colorName) != 0;
    s.fowOff = ReadInt(file, L"fow_off", s.fowOff) != 0;
    s.hidePlans = ReadInt(file, L"hide_plans", s.hidePlans) != 0;
    if (ReadInt(file, L"follow_selection", 0) != 0) {
        s.selectionMode = 1; // the old on/off setting: "on" is the instant mode
    }
    s.selectionMode = std::clamp(ReadInt(file, L"selection_mode", s.selectionMode), 0, 1);
    s.engageSwitch = ReadInt(file, L"engage_switch", s.engageSwitch) != 0;
    s.engageOnAim = ReadInt(file, L"engage_on_aim", s.engageOnAim) != 0;
    s.cooldownMs = std::clamp(ReadInt(file, L"cooldown_ms", s.cooldownMs), 100, 4000);
    s.engageHoldMs = std::clamp(ReadInt(file, L"engage_hold_ms", s.engageHoldMs), 100, 4000);
    s.aimSettleMs = std::clamp(ReadInt(file, L"aim_settle_ms", s.aimSettleMs), 0, 1000);
    s.logOperators = ReadInt(file, L"log_operators", s.logOperators) != 0;
    // "any_human" is the old name of this setting; settings.ini files from before the rename keep working.
    s.allowAllHumans = ReadInt(file, L"any_human", s.allowAllHumans) != 0;
    s.allowAllHumans = ReadInt(file, L"allow_all_humans", s.allowAllHumans) != 0;
}

void SaveSettings(const DK2ML_Option*, void*)
{
    if (g_settingsFile.empty()) {
        return;
    }

    auto write = [](const wchar_t* key, int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(kSection, key, buf, g_settingsFile.c_str());
    };

    auto writeFloat = [](const wchar_t* key, float v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.3f", v);
        WritePrivateProfileStringW(kSection, key, buf, g_settingsFile.c_str());
    };
    writeFloat(L"fov", g_settings.fov);
    writeFloat(L"offset_forward", g_settings.offsetForward);
    writeFloat(L"offset_up", g_settings.offsetUp);

    write(L"enabled", g_settings.enabled);
    write(L"key", g_settings.key);
    write(L"hold_ms", g_settings.holdMs);
    write(L"test_picture", g_settings.testPicture);
    write(L"anchor", g_settings.anchor);
    write(L"offset_x", g_settings.offsetX);
    write(L"offset_y", g_settings.offsetY);
    write(L"size_percent", g_settings.sizePercent);
    write(L"hide_night_vision", g_settings.hideNightVision);
    write(L"color_name", g_settings.colorName);
    write(L"fow_off", g_settings.fowOff);
    write(L"hide_plans", g_settings.hidePlans);
    write(L"cooldown_ms", g_settings.cooldownMs);
    write(L"engage_hold_ms", g_settings.engageHoldMs);
    write(L"aim_settle_ms", g_settings.aimSettleMs);
    write(L"selection_mode", g_settings.selectionMode);
    write(L"engage_switch", g_settings.engageSwitch);
    write(L"engage_on_aim", g_settings.engageOnAim);
    write(L"log_operators", g_settings.logOperators);
    write(L"allow_all_humans", g_settings.allowAllHumans);
}

void LoadSettings(const DK2ML_PluginInfo* info)
{
    if (info && info->modDir && info->modDir[0]) {
        ReadIni(std::wstring(info->modDir) + L"defaults.ini", g_defaults); // read only (R16)
    }

    g_settings = g_defaults;

    const wchar_t* dir = g_api->GetConfigDir();
    if (!dir[0]) {
        return; // no settings folder: packaged defaults only
    }

    g_settingsFile = std::wstring(dir) + L"settings.ini"; // not the mod folder, which Steam replaces (R16)
    if (GetFileAttributesW(g_settingsFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ReadIni(g_settingsFile, g_settings);
    } else {
        SaveSettings(nullptr, nullptr); // first run: seed the player's copy from the packaged defaults
    }
}

void OnReset(const DK2ML_Option*, void*)
{
    g_settings = g_defaults;
    SaveSettings(nullptr, nullptr);
    g_api->Log("settings reset to the packaged defaults");
}

// One table of options feeds both UIs: the Modloader page (AddOption, main menu) and the in-mission window below, so
// they list the same controls in the same order. The loader copies each entry; the window reads this table directly.
std::vector<DK2ML_Option> g_options;

void BuildOptions()
{
    auto add = [](DK2ML_Option o) { g_options.push_back(o); };

    add({sizeof(DK2ML_Option), DK2ML_OPTION_HEADER,
         "Helmet Cam - WARNING: renders the scene a second time while open, expect a lower frame rate"});

    DK2ML_Option reset = {sizeof(DK2ML_Option), DK2ML_OPTION_BUTTON, "Reset to defaults",
                          "Restores every setting to the value this mod shipped with"};
    reset.onChange = OnReset;
    add(reset);

    DK2ML_Option enabled = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Enable Helmet Cam",
                            "Master switch. Off: the key does nothing and nothing extra is rendered",
                            &g_settings.enabled};
    enabled.onChange = SaveSettings;
    add(enabled);

    DK2ML_Option key = {sizeof(DK2ML_Option), DK2ML_OPTION_KEY, "Helmet cam key",
                        "Hold to open or close the helmet cam, tap to switch to the next operator",
                        &g_settings.key};
    key.onChange = SaveSettings;
    add(key);

    add({sizeof(DK2ML_Option), DK2ML_OPTION_HEADER, "Camera"});

    auto slider = [&](const char* label, const char* tip, float* value, float lo, float hi, const char* fmt) {
        DK2ML_Option o = {sizeof(DK2ML_Option), DK2ML_OPTION_FLOAT, label, tip, value, lo, hi, fmt};
        o.onChange = SaveSettings;
        add(o);
    };
    slider("Field of view", "How wide the helmet cam sees, in degrees; the game's own free camera uses 90",
           &g_settings.fov, 30.0f, 120.0f, "%.0f deg");
    slider("Forward offset", "Moves the camera ahead of the operator (negative: behind), in metres", &g_settings.offsetForward,
           -0.5f, 1.0f, "%.2f m");
    slider("Height offset", "Raises or lowers the camera from the head height (1.58 m standing, 1.14 m crouched)",
           &g_settings.offsetUp, -0.5f, 0.5f, "%.2f m");

    DK2ML_Option any = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Allow enemy and civilian helmet cams",
                        "The cam can also cycle to enemies, civilians and hostages, not only the operators you control",
                        &g_settings.allowAllHumans};
    any.onChange = SaveSettings;
    add(any);

    add({sizeof(DK2ML_Option), DK2ML_OPTION_HEADER, "Picture"});

    static const char* const kAnchors[] = {"Top right", "Top left", "Bottom left", "Bottom right"};
    DK2ML_Option anchor = {sizeof(DK2ML_Option), DK2ML_OPTION_CHOICE, "Anchor",
                           "Which corner of the screen the helmet picture sits in"};
    anchor.value = &g_settings.anchor;
    anchor.choices = kAnchors;
    anchor.choiceCount = 4;
    anchor.onChange = SaveSettings;
    add(anchor);

    auto pixels = [&](const char* label, const char* tip, int* value, int lo, int hi, const char* fmt) {
        DK2ML_Option o = {sizeof(DK2ML_Option), DK2ML_OPTION_INT, label, tip, value, static_cast<float>(lo),
                          static_cast<float>(hi), fmt};
        o.onChange = SaveSettings;
        add(o);
    };
    pixels("Offset from the side", "Distance from the anchor corner's left or right edge of the screen, in pixels",
           &g_settings.offsetX, 0, 400, "%d px");
    pixels("Offset from the top or bottom", "Distance from the anchor corner's top or bottom edge of the screen, in pixels",
           &g_settings.offsetY, 0, 400, "%d px");
    pixels("Picture size", "Width of the helmet picture as a percentage of the window width", &g_settings.sizePercent,
           10, 60, "%d %%");

    DK2ML_Option colorName = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Colour the name like the operator's path",
                              "While the game's Colored plans option is on, the name's background takes the colour of "
                              "that operator's path. With Colored plans set to No it stays plain",
                              &g_settings.colorName};
    colorName.onChange = SaveSettings;
    add(colorName);

    DK2ML_Option fow = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Fog of war off in the helmet view",
                        "Shows the floor everywhere in the helmet picture. Only the picture changes; the game still hides "
                        "enemies you haven't seen",
                        &g_settings.fowOff};
    fow.onChange = SaveSettings;
    add(fow);

    DK2ML_Option plans = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Hide plans and UI in the helmet view",
                          "Like the replay's cinematic mode, but only for the helmet picture: no drawn operator paths, "
                          "waypoints or door and charge highlights. The main view is unaffected",
                          &g_settings.hidePlans};
    plans.onChange = SaveSettings;
    add(plans);

    DK2ML_Option nvg = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Remove night vision overlay in the helmet view",
                        "On night maps the helmet picture is drawn without the night-vision effect. The scene may look "
                        "darker. The main view is unaffected",
                        &g_settings.hideNightVision};
    nvg.onChange = SaveSettings;
    add(nvg);

    add({sizeof(DK2ML_Option), DK2ML_OPTION_HEADER, "Automatic switching"});

    auto toggle = [&](const char* label, const char* tip, bool* value) {
        DK2ML_Option o = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, label, tip, value};
        o.onChange = SaveSettings;
        add(o);
    };
    auto autoSlider = [&](const char* label, const char* tip, int* value, int lo, int hi) {
        DK2ML_Option o = {sizeof(DK2ML_Option), DK2ML_OPTION_INT, label, tip, value, static_cast<float>(lo),
                          static_cast<float>(hi), "%d ms"};
        o.onChange = SaveSettings;
        add(o);
    };
    static const char* const kSelectionModes[] = {"On key press", "Instantly"};
    DK2ML_Option selection = {sizeof(DK2ML_Option), DK2ML_OPTION_CHOICE, "Selection switching",
                              "On key press: tapping the key switches to the operator you selected, then keeps cycling. "
                              "Instantly: the cam switches the moment you select someone (not in replays)"};
    selection.value = &g_settings.selectionMode;
    selection.choices = kSelectionModes;
    selection.choiceCount = 2;
    selection.onChange = SaveSettings;
    add(selection);
    toggle("Switch to operators who are engaging",
           "If the cam's operator isn't engaging (aiming at an enemy they see, or shooting) but another operator is, the cam "
           "switches to them",
           &g_settings.engageSwitch);
    autoSlider("Switch cooldown", "After an action cam switch, the action cam waits this long before it switches again. Selection and key switching ignore it",
               &g_settings.cooldownMs, 100, 4000);
    toggle("Count aiming at a seen enemy",
           "Engaging starts when an operator aims at an enemy they can see, before the first shot. Off: shots only",
           &g_settings.engageOnAim);
    autoSlider("Aim held for", "How long an operator must keep aiming at an enemy they see before it counts; sweeping past "
               "one doesn't", &g_settings.aimSettleMs, 0, 1000);
    autoSlider("Stay engaged after", "How long the cam stays on its operator after their last shot or sighting. Other operators only count while they are shooting or aiming",
               &g_settings.engageHoldMs, 100, 4000);

    add({sizeof(DK2ML_Option), DK2ML_OPTION_HEADER, "Debug"});

    static const char* const kTestPictures[] = {"Off (helmet view)", "Mirror the main view", "Solid color"};
    DK2ML_Option test = {sizeof(DK2ML_Option), DK2ML_OPTION_CHOICE, "Test picture",
                         "Finds why the picture is black: 'Mirror' shows a shrunk copy of the normal view in the corner, "
                         "'Solid color' a flat color. Neither renders the helmet view"};
    test.value = &g_settings.testPicture;
    test.choices = kTestPictures;
    test.choiceCount = 3;
    test.onChange = SaveSettings;
    add(test);

    DK2ML_Option log = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Log operators",
                        "Once a second in a mission or replay, write every human's position, health and head bone to dk2ml.log",
                        &g_settings.logOperators};
    log.onChange = SaveSettings;
    add(log);
}

bool AddOptions()
{
    BuildOptions();
    for (const DK2ML_Option& o : g_options) {
        if (g_api->AddOption(&o) != DK2ML_OK) {
            return false;
        }
    }
    return true;
}

// --- the in-mission settings window (opened by the Esc-menu "CAM" button) ---
// ImGui names verified with symtest. Several helpers are inlined away in this build (PopID, PopStyleColor, EndCombo,
// Separator, PopItemWidth, IsItemHovered, SetNextWindowPos, GetIO), so the window avoids them: unique "##" ids instead
// of PushID, buttons instead of a combo, hint text instead of tooltips, and ImGui remembers the window position.
struct ImVec2 {
    float x, y;
};

struct ImVec4 {
    float x, y, z, w;
};

dk2ml::Fn<bool(const char* name, bool* open, int flags)> ImGui_Begin{"ImGui::Begin"};
dk2ml::Fn<void()> ImGui_End{"ImGui::End"};
dk2ml::Fn<bool(const char* label, bool* value)> ImGui_Checkbox{"ImGui::Checkbox"};
dk2ml::Fn<bool(const char* label, int dataType, void* data, const void* min, const void* max, const char* format,
               int flags)>
    ImGui_SliderScalar{"ImGui::SliderScalar"};
dk2ml::Fn<bool(const char* label, const ImVec2* size, int flags)> ImGui_ButtonEx{"ImGui::ButtonEx"};
dk2ml::Fn<void(const char* text, const char* textEnd, int flags)> ImGui_TextEx{"ImGui::TextEx"};
dk2ml::Fn<void(const char* fmt, ...)> ImGui_TextWrapped{"ImGui::TextWrapped"};
dk2ml::Fn<void(const ImVec4* color, const char* fmt, ...)> ImGui_TextColored{"ImGui::TextColored"};
// The style colours are plain floats inside the context: the name chip's background is set there for the Begin call only.
dk2ml::Field<uint8_t> ImGuiContext_Style{"ImGuiContext", "Style"};
dk2ml::Field<uint8_t> ImGuiStyle_Colors{"ImGuiStyle", "Colors"};
dk2ml::Enum ImGuiCol_WindowBg{"ImGuiCol_", "ImGuiCol_WindowBg"};
dk2ml::Fn<void(float offsetFromStartX, float spacing)> ImGui_SameLine{"ImGui::SameLine"};
dk2ml::Fn<void()> ImGui_Spacing{"ImGui::Spacing"};
dk2ml::Fn<void(int flags)> ImGui_SeparatorEx{"ImGui::SeparatorEx"};

dk2ml::Enum ImGuiWindowFlags_AlwaysAutoResize{"ImGuiWindowFlags_", "ImGuiWindowFlags_AlwaysAutoResize"};
dk2ml::Enum ImGuiWindowFlags_NoCollapse{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoCollapse"};
// The name label: a borderless, click-through window placed by pixel position (SetNextWindowPos is inlined away, so the
// current window is moved after Begin).
dk2ml::Enum ImGuiWindowFlags_NoDecoration{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoDecoration"};
dk2ml::Enum ImGuiWindowFlags_NoSavedSettings{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoSavedSettings"};
dk2ml::Enum ImGuiWindowFlags_NoMouseInputs{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoMouseInputs"};
dk2ml::Enum ImGuiWindowFlags_NoFocusOnAppearing{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoFocusOnAppearing"};
dk2ml::Enum ImGuiWindowFlags_NoNav{"ImGuiWindowFlags_", "ImGuiWindowFlags_NoNav"};
dk2ml::Enum ImGuiCond_Always{"ImGuiCond_", "ImGuiCond_Always"};
dk2ml::Field<void*> ImGuiContext_CurrentWindow{"ImGuiContext", "CurrentWindow"};
// SetWindowFontScale is inlined away: it only stores the window's scale and refreshes the two cached font sizes, so
// the name label does the same by hand (ImGui::End re-derives them for the window underneath).
dk2ml::Field<float> ImGuiWindow_FontWindowScale{"ImGuiWindow", "FontWindowScale"};
dk2ml::Field<float> ImGuiContext_FontSize{"ImGuiContext", "FontSize"};
dk2ml::Field<float> ImGuiContext_FontBaseSize{"ImGuiContext", "FontBaseSize"};
dk2ml::Field<uint8_t> ImGuiContext_DrawListSharedData{"ImGuiContext", "DrawListSharedData"};
dk2ml::Field<float> ImDrawListSharedData_FontSize{"ImDrawListSharedData", "FontSize"};
dk2ml::Fn<void(void* window, const ImVec2* pos, int cond)> ImGui_SetWindowPos{
    "?SetWindowPos@ImGui@@YAXPEAUImGuiWindow@@AEBUImVec2@@H@Z"};
dk2ml::Enum ImGuiDataType_Float{"ImGuiDataType_", "ImGuiDataType_Float"};
dk2ml::Enum ImGuiDataType_S32{"ImGuiDataType_", "ImGuiDataType_S32"};

// ImGui's own IO flags say when ImGui uses the mouse or keyboard (R28). GetIO was inlined away: read the context.
dk2ml::Global<void*> GImGui{"GImGui"};
dk2ml::Field<uint8_t> ImGuiContext_IO{"ImGuiContext", "IO"};
dk2ml::Field<bool> ImGuiIO_WantCaptureMouse{"ImGuiIO", "WantCaptureMouse"};
dk2ml::Field<bool> ImGuiIO_WantCaptureKeyboard{"ImGuiIO", "WantCaptureKeyboard"};

bool g_windowOpen = false;
bool g_waitingForKey = false;  // the window's "Rebind" was clicked: the next key press becomes the helmet cam key
bool g_capturing = false;      // we told the game to ignore the map and HUD (R28)
bool g_waitingNeedsRelease = false; // the click that started the rebind (mouse) must be released first

std::string KeyName(int vk)
{
    switch (vk) {
    case 0: return "(none)";
    case VK_LBUTTON: return "Mouse left";
    case VK_RBUTTON: return "Mouse right";
    case VK_MBUTTON: return "Mouse middle";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_CAPITAL: return "Caps Lock";
    default: break;
    }
    LONG scan = static_cast<LONG>(MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
    char name[64] = {};
    if (scan && GetKeyNameTextA(scan, name, sizeof(name)) > 0) {
        return name;
    }
    char hex[16];
    snprintf(hex, sizeof(hex), "VK 0x%X", vk);
    return hex;
}

// While rebinding: the first key or mouse button (not left/right click: those clicked the window) that goes down. Esc
// cancels. Left/right Shift, Ctrl and Alt are stored as the generic VK_SHIFT, VK_CONTROL and VK_MENU, as the loader does.
void PollRebind()
{
    if (g_waitingNeedsRelease) {
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            g_waitingNeedsRelease = false;
        }
        return;
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_waitingForKey = false;
        return;
    }
    for (int vk = 0x04; vk < 0xFF; ++vk) {
        if (vk == VK_ESCAPE || vk == VK_LBUTTON || vk == VK_RBUTTON || (vk >= VK_LSHIFT && vk <= VK_RMENU)) {
            continue; // the generic modifiers VK_SHIFT/VK_CONTROL/VK_MENU stand for the left and right ones
        }
        if (GetAsyncKeyState(vk) & 0x8000) {
            g_settings.key = vk;
            g_waitingForKey = false;
            SaveSettings(nullptr, nullptr);
            g_api->Log("helmet cam key set to %s (vk=0x%X)", KeyName(vk).c_str(), vk);
            return;
        }
    }
}

// One row per table entry. Each change runs the entry's own onChange, like the Modloader page does.
void DrawOption(DK2ML_Option& o, int index)
{
    char id[96];
    switch (o.type) {
    case DK2ML_OPTION_HEADER:
        ImGui_Spacing();
        ImGui_SeparatorEx(1); // ImGuiSeparatorFlags_Horizontal
        ImGui_TextEx(o.label, nullptr, 0);
        break;
    case DK2ML_OPTION_BOOL: {
        snprintf(id, sizeof(id), "%s##opt%d", o.label, index);
        if (ImGui_Checkbox(id, static_cast<bool*>(o.value)) && o.onChange) {
            o.onChange(&o, o.user);
        }
        ImGui_TextWrapped("%s", o.tooltip ? o.tooltip : "");
        break;
    }
    case DK2ML_OPTION_FLOAT:
    case DK2ML_OPTION_INT: {
        snprintf(id, sizeof(id), "%s##opt%d", o.label, index);
        bool isFloat = o.type == DK2ML_OPTION_FLOAT;
        float fmin = o.min, fmax = o.max;
        int imin = static_cast<int>(o.min), imax = static_cast<int>(o.max);
        bool changed = ImGui_SliderScalar(id, static_cast<int>((isFloat ? ImGuiDataType_Float : ImGuiDataType_S32).Get()),
                                          o.value, isFloat ? static_cast<const void*>(&fmin) : &imin,
                                          isFloat ? static_cast<const void*>(&fmax) : &imax, o.format ? o.format : "%.2f",
                                          0);
        if (changed && o.onChange) {
            o.onChange(&o, o.user);
        }
        ImGui_TextWrapped("%s", o.tooltip ? o.tooltip : "");
        break;
    }
    case DK2ML_OPTION_CHOICE: {
        int& value = *static_cast<int*>(o.value);
        ImGui_TextEx(o.label, nullptr, 0);
        for (int i = 0; i < o.choiceCount; ++i) {
            // The chosen entry is marked with brackets: styles can't be pushed and popped from here.
            snprintf(id, sizeof(id), "%s%s%s##opt%d_%d", i == value ? "[ " : "", o.choices[i], i == value ? " ]" : "",
                     index, i);
            ImVec2 size = {0.0f, 0.0f};
            if (ImGui_ButtonEx(id, &size, 0) && i != value) {
                value = i;
                if (o.onChange) {
                    o.onChange(&o, o.user);
                }
            }
            if (i + 1 < o.choiceCount) {
                ImGui_SameLine(0.0f, -1.0f);
            }
        }
        ImGui_TextWrapped("%s", o.tooltip ? o.tooltip : "");
        break;
    }
    case DK2ML_OPTION_KEY: {
        std::string label = g_waitingForKey ? std::string("Press a key, or Esc to cancel...")
                                            : std::string(o.label) + ": " + KeyName(*static_cast<int*>(o.value));
        ImGui_TextEx(label.c_str(), nullptr, 0);
        ImGui_SameLine(0.0f, -1.0f);
        snprintf(id, sizeof(id), "Rebind##opt%d", index);
        ImVec2 size = {0.0f, 0.0f};
        if (ImGui_ButtonEx(id, &size, 0) && !g_waitingForKey) {
            g_waitingForKey = true;
            g_waitingNeedsRelease = true; // the click on this button is still down
        }
        ImGui_TextWrapped("%s", o.tooltip ? o.tooltip : "");
        break;
    }
    case DK2ML_OPTION_BUTTON: {
        snprintf(id, sizeof(id), "%s##opt%d", o.label, index);
        ImVec2 size = {0.0f, 0.0f};
        if (ImGui_ButtonEx(id, &size, 0) && o.onChange) {
            o.onChange(&o, o.user);
        }
        break;
    }
    default:
        break;
    }
}

void UpdateInputCapture(bool windowShown)
{
    void* context = GImGui ? *GImGui : nullptr;
    bool wants = false;
    if (context) {
        int io = ImGuiContext_IO.Offset();
        wants = dk2ml::At<bool>(context, io + ImGuiIO_WantCaptureMouse.Offset()) ||
                dk2ml::At<bool>(context, io + ImGuiIO_WantCaptureKeyboard.Offset());
    }
    bool capture = windowShown && (wants || g_waitingForKey);
    if (capture != g_capturing && dk2ml::CaptureInput(g_api, capture)) {
        g_capturing = capture;
    }
}

void DrawSettingsWindow()
{
    bool shown = false;
    if (g_windowOpen) {
        int flags = static_cast<int>(ImGuiWindowFlags_AlwaysAutoResize.Get() | ImGuiWindowFlags_NoCollapse.Get());
        bool open = true;
        shown = ImGui_Begin("Helmet Cam##helmetcam_settings", &open, flags);
        if (shown) {
            ImGui_TextEx("WARNING: the helmet cam renders the scene a second time while open.", nullptr, 0);
            for (size_t i = 0; i < g_options.size(); ++i) {
                if (i == 0 && g_options[i].type == DK2ML_OPTION_HEADER) {
                    continue; // the warning above says the same as the first header
                }
                DrawOption(g_options[i], static_cast<int>(i));
            }
        }
        ImGui_End(); // always, even when the window is collapsed (Begin's result)
        if (!open) {
            g_windowOpen = false;
        }
    }
    if (!g_windowOpen) {
        g_waitingForKey = false;
    }
    if (g_waitingForKey) {
        PollRebind();
    }
    UpdateInputCapture(g_windowOpen && shown);
}

// --- game state helpers (main thread only) ---
bool InMission()
{
    void* client = g_pGameClient ? *g_pGameClient : nullptr;
    return client && GameClient_m_state(client) == StateRunning.Get();
}

// Every Human_Client, in list order. The list ends where a link's next equals the list's head.
std::vector<void*> AllHumans()
{
    std::vector<void*> out;
    void* list = Human_typeList.Address();
    if (!list) {
        return out;
    }
    void* end = dk2ml::At<void*>(list, LinkedList_head.Offset());
    void* node = dk2ml::At<void*>(list, LinkedList_next.Offset());
    for (int guard = 0; node && node != end && node != list && guard < 1024; ++guard) {
        void* human = dk2ml::At<void*>(node, LinkedList_owner.Offset());
        if (human) {
            out.push_back(human);
        }
        node = dk2ml::At<void*>(node, LinkedList_next.Offset());
    }
    return out;
}

bool IsOperator(void* human)
{
    return g_settings.allowAllHumans || Human_isControlable(human);
}

bool IsAlive(void* human)
{
    return Entity_health(human) > 0.0f;
}

// The head bone's matGlobal (3 rows of 4 floats), or null. Logging only.
const float* HeadBone(void* human)
{
    void* anim = Entity_animManager(human);
    if (!anim) {
        return nullptr;
    }
    char* skeleton = static_cast<char*>(anim) + AnimManager_skeleton.Offset();
    int count = dk2ml::At<int>(skeleton, Skeleton_numNodes.Offset());
    if (count <= 0 || count > 64) {
        return nullptr;
    }
    char* nodes = skeleton + Skeleton_nodes.Offset();
    for (int i = 0; i < count; ++i) {
        char* node = nodes + static_cast<size_t>(i) * SkeletonNode_size.Get();
        if (std::strncmp(node + SkeletonNode_name.Offset(), "Bip001 Head", 48) == 0) {
            return reinterpret_cast<const float*>(node + SkeletonNode_matGlobal.Offset());
        }
    }
    return nullptr;
}

// --- the cam: which operator, open or closed ---
bool g_open = false;
uint32_t g_targetId = 0;       // Entity_Common::m_id of the operator we show; 0 = none yet
bool g_cycleRequested = false;
uint32_t g_lockedId = 0;        // the HUD lock button: the cam stays on this operator until unlocked (or they die)
bool g_selectionPending = false; // the player selected someone the cam hasn't gone to: the next tap goes there

uint32_t SelectedOperatorId();

// --- automatic switching: game time (ms) so pauses and replay speed apply, per-operator engagement state by entity id ---
constexpr int kNever = -100000000;
int g_lastSwitchMs = kNever;               // the last action cam (engagement) switch; the cooldown measures from it
uint32_t g_lastSelectedId = 0;             // the operator selected last frame, to see a selection change
std::unordered_map<uint32_t, int> g_lastEngaged;   // last shot or settled aim, per operator
std::unordered_map<uint32_t, int> g_engageStart;   // when the current run of engagement began, per operator
struct AimState {
    uint32_t enemy;
    int since;
};
std::unordered_map<uint32_t, AimState> g_aims;
ULONGLONG g_lastEngageTick = 0;            // real time of the last aim check (they run at about 10 Hz)

bool GameNow(int* now)
{
    void* client = g_pGameClient ? *g_pGameClient : nullptr;
    if (!client) {
        return false;
    }
    *now = GameCommon_gameTime(client);
    return true;
}

void ResetCam()
{
    g_open = false;
    g_targetId = 0;
    g_cycleRequested = false;
    g_lockedId = 0;
    g_selectionPending = false;
    g_lastSwitchMs = kNever;
    g_lastSelectedId = 0;
    g_lastEngaged.clear();
    g_engageStart.clear();
    g_aims.clear();
}

// The operator to show, or null. `alive` is false when the cam should be black: that operator died (the cam stays
// on them), or nobody is left.
void* FindTarget(bool& alive)
{
    alive = false;
    std::vector<void*> ops;
    for (void* h : AllHumans()) {
        if (IsOperator(h)) {
            ops.push_back(h);
        }
    }
    if (ops.empty()) {
        return nullptr;
    }

    // Locked: the cam stays on that operator, taps do nothing. The lock ends when they die or leave the list.
    if (g_lockedId != 0) {
        void* locked = nullptr;
        for (void* op : ops) {
            if (Entity_id(op) == g_lockedId) {
                locked = op;
            }
        }
        if (locked && IsAlive(locked)) {
            g_targetId = g_lockedId;
            g_cycleRequested = false;
        } else {
            g_api->Log("helmet cam unlocked: operator %u is gone", g_lockedId);
            g_lockedId = 0;
        }
    }

    int current = -1;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (Entity_id(ops[i]) == g_targetId) {
            current = static_cast<int>(i);
        }
    }

    // "On key press" selection switching: the first tap after the player selected someone goes to them, then taps cycle.
    if (g_cycleRequested && g_selectionPending && g_settings.selectionMode == 0) {
        g_selectionPending = false;
        uint32_t selected = SelectedOperatorId();
        for (size_t i = 0; selected != 0 && selected != g_targetId && i < ops.size(); ++i) {
            if (Entity_id(ops[i]) == selected && IsAlive(ops[i])) {
                current = static_cast<int>(i);
                g_targetId = selected;
                g_cycleRequested = false;
                g_api->Log("switch (selection, on key press): operator %u", g_targetId);
            }
        }
    }

    if (g_cycleRequested || current < 0) {
        // Next living operator after the current one (the first one when there is none): dead operators are skipped.
        // No living operator leaves the target alone, so a dead target stays on a black cam.
        g_cycleRequested = false;
        int n = static_cast<int>(ops.size());
        for (int k = 1; k <= n; ++k) {
            int candidate = (current + k + n) % n;
            if (IsAlive(ops[candidate])) {
                current = candidate;
                g_targetId = Entity_id(ops[candidate]);
                break;
            }
        }
    }

    if (current < 0) {
        return nullptr;
    }
    alive = IsAlive(ops[current]);
    return ops[current];
}

// --- automatic switching: who is engaging, and the rules for switching ---
constexpr float kAimRangeM = 25.0f;
constexpr float kAimConeDeg = 10.0f; // either side of the aim
constexpr float kEyeHeightM = 1.8f;  // as the game's own sight checks

bool IsHumanType(void* human, const dk2ml::Enum& type)
{
    void* tmpl = Entity_pTemplate(human);
    return tmpl && HumanTemplate_type(tmpl) == static_cast<int>(type.Get());
}

// A shot by one of our side. One event per round; door breaching shots don't count.
int ReceiveEventPre(DK2ML_Regs* regs, void*)
{
    if (!g_settings.engageSwitch || dk2ml::Arg<int>(regs, 2) != static_cast<int>(EV_HUMAN_FIREARM_SHOT.Get())) {
        return DK2ML_CALL_ORIGINAL;
    }
    void* human = dk2ml::Arg<void*>(regs, 0);
    int now = 0;
    if (human && IsHumanType(human, HUMAN_GOODGUY) && IsAlive(human) &&
        Human_currentCommand(human) != static_cast<int>(HUMAN_CMD_SHOTGUN_DOOR.Get()) && GameNow(&now)) {
        g_lastEngaged[Entity_id(human)] = now;
    }
    return DK2ML_CALL_ORIGINAL;
}

// The nearest live enemy near the operator's level aim that the team sees (m_bHiddenInFOW) and the operator has a line of
// sight to. 0 if none.
uint32_t AimedEnemy(void* human, const std::vector<void*>& humans)
{
    Vec3 aim = Human_aimDirection(human);
    float aimLength = std::sqrt(aim.x * aim.x + aim.z * aim.z);
    if (aimLength < 1e-4f) {
        return 0;
    }

    Vec3 from = Entity_origin(human);
    Vec3 eye = {from.x, kEyeHeightM, from.z};
    Vec3 level = {aim.x / aimLength, 0.0f, aim.z / aimLength};
    float minCos = std::cos(kAimConeDeg * 3.14159265f / 180.0f);
    uint32_t visionBlock = static_cast<uint32_t>(COL_VISION_BLOCK.Get());
    uint32_t nearest = 0;
    float nearestDistance = kAimRangeM;
    for (void* enemy : humans) {
        if (!IsHumanType(enemy, HUMAN_BADGUY) || !IsAlive(enemy) || Human_isSurrendered(enemy) ||
            Entity_hiddenInFow(enemy)) {
            continue;
        }
        Vec3 to = Entity_origin(enemy);
        float dx = to.x - from.x;
        float dz = to.z - from.z;
        float distance = std::sqrt(dx * dx + dz * dz);
        if (distance < 1e-3f || distance > nearestDistance || (dx * level.x + dz * level.z) / distance < minCos) {
            continue;
        }
        if (GameCommon_HasLineOfSight(&eye, &level, 2.0f * kAimConeDeg, enemy, visionBlock, false)) {
            nearest = Entity_id(enemy);
            nearestDistance = distance;
        }
    }
    return nearest;
}

// True once the operator has kept aiming at the same seen enemy for aimSettleMs.
bool AimSettled(void* human, const std::vector<void*>& humans, int now)
{
    uint32_t id = Entity_id(human);
    uint32_t enemy = AimedEnemy(human, humans);
    if (enemy == 0) {
        g_aims.erase(id);
        return false;
    }
    auto found = g_aims.find(id);
    if (found == g_aims.end() || found->second.enemy != enemy || now < found->second.since) {
        g_aims[id] = {enemy, now};
        return g_settings.aimSettleMs <= 0;
    }
    return now - found->second.since >= g_settings.aimSettleMs;
}

// Engaging: a shot or settled aim within windowMs. The cam's own operator is held for engageHoldMs after their last one,
// so the cam stays on them while the fight winds down; every other operator only counts while they are shooting or aiming
// right now (kActiveMs). A replay that jumped back leaves a "future" time: forget it.
constexpr int kActiveMs = 300; // longer than the 100 ms aim check, so a held aim stays "active"

bool IsEngaging(uint32_t id, int now, int windowMs)
{
    auto found = g_lastEngaged.find(id);
    if (found == g_lastEngaged.end()) {
        return false;
    }
    int age = now - found->second;
    if (age < 0) {
        g_lastEngaged.erase(found);
        return false;
    }
    return age < windowMs;
}

// Aim checks (the costly part: a sight line per operator and enemy) run at about 10 Hz and only while the cam is open.
// This also notes when each operator's current run of engagement began, to pick the one engaging longest.
void UpdateEngagement(const std::vector<void*>& ops, int now)
{
    ULONGLONG tick = GetTickCount64();
    if (tick - g_lastEngageTick >= 100) {
        g_lastEngageTick = tick;
        if (g_settings.engageOnAim) {
            std::vector<void*> humans = AllHumans();
            for (void* op : ops) {
                if (IsAlive(op) && AimSettled(op, humans, now)) {
                    g_lastEngaged[Entity_id(op)] = now;
                }
            }
        }
    }
    for (void* op : ops) {
        uint32_t id = Entity_id(op);
        if (IsAlive(op) && IsEngaging(id, now, kActiveMs)) {
            g_engageStart.emplace(id, now); // keeps the first time of an ongoing run
        } else {
            g_engageStart.erase(id);
        }
    }
}

uint32_t SelectedOperatorId()
{
    void* input = g_pGameInput ? *g_pGameInput : nullptr;
    void* human = input ? GameInput_GetSelectedHuman(input) : nullptr;
    return human && IsOperator(human) && IsAlive(human) ? Entity_id(human) : 0;
}

void SwitchTo(void* op, const char* reason, int now)
{
    g_targetId = Entity_id(op);
    if (strcmp(reason, "engagement") == 0) {
        g_lastSwitchMs = now; // only action cam switches start the action cam's cooldown
    }
    const char* name = Entity_name(op);
    g_api->Log("auto switch (%s): operator %u '%s'", reason, g_targetId, name ? name : "");
}

// The HUD lock button: lock the cam to the selected operator (the cam's current one when nobody is selected), or unlock.
void ToggleLock()
{
    if (g_lockedId != 0) {
        g_api->Log("helmet cam unlocked from operator %u", g_lockedId);
        g_lockedId = 0;
        return;
    }
    uint32_t id = SelectedOperatorId();
    if (id == 0) {
        id = g_targetId;
    }
    for (void* h : AllHumans()) {
        if (IsOperator(h) && Entity_id(h) == id && IsAlive(h)) {
            g_lockedId = id;
            g_targetId = id;
            g_selectionPending = false;
            const char* name = Entity_name(h);
            g_api->Log("helmet cam locked to operator %u '%s'", id, name ? name : "");
            return;
        }
    }
    g_api->Log("helmet cam lock: no living operator to lock to");
}

// Runs every frame in a mission. Selection changes switch at once (instant mode); engagement (action cam) switches wait
// for the cooldown, which only they start.
void UpdateAutoTarget()
{
    int now = 0;
    if (!g_settings.enabled || !GameNow(&now)) {
        return;
    }
    if (now < g_lastSwitchMs) {
        g_lastSwitchMs = kNever; // a replay jumped back
    }

    // The selection is watched in both modes: instant mode switches on a change, on-key mode remembers it for the next tap.
    uint32_t selected = dk2ml::IsReplaying(g_api) ? 0 : SelectedOperatorId();
    bool selectionChanged = selected != g_lastSelectedId;
    g_lastSelectedId = selected;
    if (selectionChanged && selected != 0) {
        g_selectionPending = true;
    }

    if (g_lockedId != 0 || (g_settings.selectionMode != 1 && !g_settings.engageSwitch)) {
        return; // locked: nothing moves the cam
    }

    std::vector<void*> ops;
    for (void* h : AllHumans()) {
        if (IsOperator(h)) {
            ops.push_back(h);
        }
    }
    auto byId = [&](uint32_t id) -> void* {
        for (void* op : ops) {
            if (Entity_id(op) == id) {
                return op;
            }
        }
        return nullptr;
    };

    if (g_settings.engageSwitch && g_open) {
        UpdateEngagement(ops, now);
    }

    if (g_settings.selectionMode == 1 && selectionChanged) {
        void* op = selected != 0 && selected != g_targetId ? byId(selected) : nullptr;
        if (op) {
            g_selectionPending = false;
            SwitchTo(op, "selection", now);
        }
    }

    if (g_settings.engageSwitch && g_open) {
        void* current = byId(g_targetId);
        bool currentDead = current && !IsAlive(current);
        bool cooledDown = now - g_lastSwitchMs >= g_settings.cooldownMs;
        // A dead operator's black cam shouldn't wait: someone else is fighting.
        if ((cooledDown || currentDead) && !(current && IsAlive(current) && IsEngaging(g_targetId, now, g_settings.engageHoldMs))) {
            void* best = nullptr;
            int bestStart = 0;
            for (void* op : ops) {
                uint32_t id = Entity_id(op);
                auto start = g_engageStart.find(id);
                if (id == g_targetId || !IsAlive(op) || start == g_engageStart.end()) {
                    continue;
                }
                if (!best || start->second < bestStart) { // the one engaging longest; ties keep roster order
                    best = op;
                    bestStart = start->second;
                }
            }
            if (best) {
                SwitchTo(best, "engagement", now);
            }
        }
    }
}

// --- GL resources: a texture the size of the corner picture, and a framebuffer around it ---
GLuint g_tex = 0;
GLuint g_fbo = 0;
int g_texW = 0;
int g_texH = 0;
int g_texFill = -1; // what the texture holds: -1 an image (or unknown), 0 black, 1 magenta
std::vector<unsigned char> g_fillPixels;

// GL errors are logged for the first few passes after the cam opens, tagged with the stage that raised them. The
// queue is drained before our own steps, so the game's leftovers are told apart from ours.
int g_glLogBudget = 0;

void GlStage(const char* stage)
{
    for (int i = 0; i < 8; ++i) {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR) {
            return;
        }
        if (g_glLogBudget > 0) {
            --g_glLogBudget;
            g_api->Log("helmet cam: GL error 0x%X %s", e, stage);
        }
    }
}

struct GlSaved {
    GLint readFb = 0, drawFb = 0, tex = 0;
    GLboolean scissor = GL_FALSE;
};

void SaveGl(GlSaved& s)
{
    glGetIntegerv(kReadFramebufferBinding, &s.readFb);
    glGetIntegerv(kDrawFramebufferBinding, &s.drawFb);
    glGetIntegerv(kTextureBinding2D, &s.tex);
    s.scissor = glIsEnabled(kScissorTest);
}

void RestoreGl(const GlSaved& s)
{
    (*gl_BindFramebuffer)(kReadFramebuffer, static_cast<GLuint>(s.readFb));
    (*gl_BindFramebuffer)(kDrawFramebuffer, static_cast<GLuint>(s.drawFb));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex));
    if (s.scissor) {
        glEnable(kScissorTest);
    }
}

bool GlReady()
{
    return *gl_GenFramebuffers && *gl_BindFramebuffer && *gl_FramebufferTexture2D && *gl_CheckFramebufferStatus &&
           *gl_BlitFramebuffer;
}

void DestroyResources()
{
    if (g_fbo && *gl_DeleteFramebuffers) {
        (*gl_DeleteFramebuffers)(1, &g_fbo);
    }
    if (g_tex) {
        glDeleteTextures(1, &g_tex);
    }
    g_fbo = 0;
    g_tex = 0;
    g_texW = g_texH = 0;
}

// Where the helmet picture sits, in window pixels measured from the top left: a quarter of the window wide, the window's
// aspect, placed by the anchor, offsets and size settings. The picture, its texture and the name label all use this.
struct PipRect {
    int x, yTop, w, h; // where the picture is shown, and how big (the texture has the same size)
};

PipRect ComputePipRect(int viewW, int viewH)
{
    int w = std::clamp(viewW * g_settings.sizePercent / 100, 64, std::max(64, viewW));
    int h = std::clamp(static_cast<int>(static_cast<int64_t>(w) * viewH / std::max(viewW, 1)), 36, std::max(36, viewH));

    // Offsets measure in from the anchor corner; the clamp keeps the whole picture inside the window.
    bool right = g_settings.anchor == 0 || g_settings.anchor == 3;
    bool bottom = g_settings.anchor == 2 || g_settings.anchor == 3;
    int x = right ? viewW - g_settings.offsetX - w : g_settings.offsetX;
    int y = bottom ? viewH - g_settings.offsetY - h : g_settings.offsetY;
    x = std::clamp(x, 0, std::max(0, viewW - w));
    y = std::clamp(y, 0, std::max(0, viewH - h));

    return {x, y, w, h};
}

bool EnsureResources(int viewW, int viewH)
{
    if (!GlReady() || viewW <= 0 || viewH <= 0) {
        return false;
    }
    PipRect rect = ComputePipRect(viewW, viewH);
    int w = rect.w;
    int h = rect.h;
    if (g_fbo && g_tex && w == g_texW && h == g_texH) {
        return true;
    }

    GlSaved saved;
    SaveGl(saved);
    DestroyResources();

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    (*gl_GenFramebuffers)(1, &g_fbo);
    (*gl_BindFramebuffer)(kFramebuffer, g_fbo);
    (*gl_FramebufferTexture2D)(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_tex, 0);
    GLenum status = (*gl_CheckFramebufferStatus)(kFramebuffer);
    RestoreGl(saved);

    if (status != kFramebufferComplete) {
        g_api->Log("helmet cam: framebuffer %dx%d incomplete (0x%X), cam off", w, h, status);
        DestroyResources();
        return false;
    }
    GlStage("while creating the texture and framebuffer");
    g_texW = w;
    g_texH = h;
    g_texFill = -1;
    g_api->Log("helmet cam: %dx%d picture for a %dx%d window", w, h, viewW, viewH);
    return true;
}

// A full-window, single-sample copy of the back buffer. Only used when the back buffer is multisampled: GL refuses to
// scale a multisampled source (INVALID_OPERATION), so it is resolved 1:1 first and shrunk from here.
GLuint g_fullTex = 0;
GLuint g_fullFbo = 0;
int g_fullW = 0;
int g_fullH = 0;
int g_backSamples = -1; // samples per pixel of the default framebuffer, read on the first capture

bool EnsureFullCopy(int w, int h)
{
    if (g_fullFbo && g_fullW == w && g_fullH == h) {
        return true;
    }
    GlSaved saved;
    SaveGl(saved);
    if (g_fullFbo) {
        (*gl_DeleteFramebuffers)(1, &g_fullFbo);
        g_fullFbo = 0;
    }
    if (g_fullTex) {
        glDeleteTextures(1, &g_fullTex);
        g_fullTex = 0;
    }
    glGenTextures(1, &g_fullTex);
    glBindTexture(GL_TEXTURE_2D, g_fullTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    (*gl_GenFramebuffers)(1, &g_fullFbo);
    (*gl_BindFramebuffer)(kFramebuffer, g_fullFbo);
    (*gl_FramebufferTexture2D)(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_fullTex, 0);
    GLenum status = (*gl_CheckFramebufferStatus)(kFramebuffer);
    RestoreGl(saved);
    if (status != kFramebufferComplete) {
        g_api->Log("helmet cam: full-size copy %dx%d incomplete (0x%X)", w, h, status);
        (*gl_DeleteFramebuffers)(1, &g_fullFbo);
        glDeleteTextures(1, &g_fullTex);
        g_fullFbo = g_fullTex = 0;
        return false;
    }
    g_fullW = w;
    g_fullH = h;
    return true;
}

// The finished frame is in the back buffer: shrink it into our texture.
void CaptureBackBuffer(int viewW, int viewH)
{
    GlSaved saved;
    SaveGl(saved);
    glDisable(kScissorTest);

    // The default framebuffer's read buffer is state the game may have changed (GL_NONE would make every blit from
    // it INVALID_OPERATION), so name the back buffer explicitly and put the old value back afterwards.
    (*gl_BindFramebuffer)(kReadFramebuffer, 0);
    GLint previousRead = GL_BACK;
    glGetIntegerv(kReadBuffer, &previousRead);
    if (g_backSamples < 0) {
        GLint samples = 0;
        glGetIntegerv(kSamples, &samples);
        g_backSamples = samples;
        g_api->Log("helmet cam: back buffer has %d samples, its read buffer was 0x%X", samples, previousRead);
    }
    glReadBuffer(GL_BACK);
    GlStage("while preparing to read the back buffer");

    if (g_backSamples > 0 && EnsureFullCopy(viewW, viewH)) {
        (*gl_BindFramebuffer)(kDrawFramebuffer, g_fullFbo);
        (*gl_BlitFramebuffer)(0, 0, viewW, viewH, 0, 0, viewW, viewH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        GlStage("in the full-size resolve blit");
        (*gl_BindFramebuffer)(kReadFramebuffer, g_fullFbo);
    }
    (*gl_BindFramebuffer)(kDrawFramebuffer, g_fbo);
    (*gl_BlitFramebuffer)(0, 0, viewW, viewH, 0, 0, g_texW, g_texH, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    (*gl_BindFramebuffer)(kReadFramebuffer, 0);
    glReadBuffer(static_cast<GLenum>(previousRead));
    RestoreGl(saved);
    g_texFill = -1;
}

// 0: opaque black (a dead operator, or nobody left), 1: opaque magenta (the solid-color test).
void FillSolid(int code)
{
    if (g_texFill == code) {
        return;
    }
    GlSaved saved;
    SaveGl(saved);
    const unsigned char r = code == 1 ? 255 : 0;
    const unsigned char b = code == 1 ? 255 : 0;
    g_fillPixels.resize(static_cast<size_t>(g_texW) * g_texH * 4);
    for (size_t i = 0; i < g_fillPixels.size(); i += 4) {
        g_fillPixels[i] = r;
        g_fillPixels[i + 1] = 0;
        g_fillPixels[i + 2] = b;
        g_fillPixels[i + 3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_texW, g_texH, GL_RGBA, GL_UNSIGNED_BYTE, g_fillPixels.data());
    RestoreGl(saved);
    g_texFill = code;
}

// Our texture into the top-right corner of the back buffer.
void BlitToCorner(int viewW, int viewH)
{
    PipRect rect = ComputePipRect(viewW, viewH);
    int x0 = rect.x;
    int x1 = rect.x + rect.w;
    int y1 = viewH - rect.yTop; // GL's origin is the bottom left
    int y0 = y1 - rect.h;

    GlSaved saved;
    SaveGl(saved);
    glDisable(kScissorTest);
    (*gl_BindFramebuffer)(kReadFramebuffer, g_fbo);
    (*gl_BindFramebuffer)(kDrawFramebuffer, 0);
    (*gl_BlitFramebuffer)(0, 0, g_texW, g_texH, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    RestoreGl(saved);
}

// --- the camera at the operator's head ---
alignas(16) unsigned char g_camera[1024];
int g_helmetFrames = 0; // how many passes were logged in detail

Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Normalize(Vec3 v)
{
    float len = std::sqrt(Dot(v, v));
    return len > 1e-5f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.0f, 0.0f, -1.0f};
}

// The head bone's world position. Its matGlobal translation is entity-local (the log showed about (0, 1.58, 0.09)
// standing), so it goes through the entity's own basis: origin + left*x + up*y + forward*z, scaled. False when the
// numbers don't look like a head (no skeleton, a degenerate basis, a bone far from the body): the caller then falls back
// to the fixed height.
Vec3 g_lastFixedEye = {};
Vec3 g_lastBoneEye = {};
bool g_lastBoneOk = false;

bool HeadWorldPosition(void* human, Vec3& out)
{
    const float* b = HeadBone(human);
    if (!b) {
        return false;
    }
    Vec3 local = {b[3], b[7], b[11]};
    Vec3 l = Entity_left(human), u = Entity_up(human), fw = Entity_forward(human);
    float scale = Entity_scale(human);
    bool basisOk = std::fabs(Dot(l, l) - 1.0f) < 0.1f && std::fabs(Dot(u, u) - 1.0f) < 0.1f &&
                   std::fabs(Dot(fw, fw) - 1.0f) < 0.1f;
    bool localOk = std::isfinite(local.x) && std::isfinite(local.y) && std::isfinite(local.z) && local.y > 0.3f &&
                   local.y < 2.5f && std::fabs(local.x) < 1.5f && std::fabs(local.z) < 1.5f;
    if (!basisOk || !localOk || !(scale > 0.1f && scale < 10.0f)) {
        return false;
    }
    Vec3 o = Entity_origin(human);
    out = {o.x + (l.x * local.x + u.x * local.y + fw.x * local.z) * scale,
           o.y + (l.y * local.x + u.y * local.y + fw.y * local.z) * scale,
           o.z + (l.z * local.x + u.z * local.y + fw.z * local.z) * scale};
    return true;
}

// A look-at view matrix for the game's world: Y is up and the ground is X/Z (every human's origin is (x, 0, z)).
// Stored the way the game stores its own, which the log showed for the top-down camera: row-major, rows right / up /
// -forward, translation in the fourth column = -(row . position).
void BuildCamera(void* human, const int* viewport, Vec3& eyeOut, Vec3& fwdOut)
{
    std::memset(g_camera, 0, sizeof(g_camera));
    Camera_SetDefaults(g_camera);
    Camera_SetProjectionPerspective(g_camera, static_cast<float>(viewport[2]), static_cast<float>(viewport[3]),
                                    g_settings.fov, 0.05f, 100.0f);

    Vec3 look = Human_headDirection(human);
    look.y = 0.0f; // level gaze for now; pitch comes later
    Vec3 f = Normalize(look);

    // The origin is on the floor. The fixed fallback uses the head heights seen in the logged bones: 1.58 standing, 1.14
    // crouched. With "Eye follows the animated head" the live head bone replaces it, so the camera moves with the
    // animation. The player's offsets are added on top (by default a little ahead, so it isn't inside the head).
    Vec3 fixedEye = Entity_origin(human);
    fixedEye.y += Human_isCrouched(human) ? 1.14f : 1.58f;

    Vec3 eye = fixedEye;
    Vec3 boneEye = {};
    bool haveBone = HeadWorldPosition(human, boneEye);
    if (haveBone) { // the live head bone; the fixed height only when the bone data is unusable
        eye = boneEye;
    }
    g_lastFixedEye = fixedEye;
    g_lastBoneEye = boneEye;
    g_lastBoneOk = haveBone;

    eye.y += g_settings.offsetUp;
    eye.x += f.x * g_settings.offsetForward;
    eye.z += f.z * g_settings.offsetForward;

    Vec3 up = {0.0f, 1.0f, 0.0f};
    Vec3 r = Normalize(Cross(f, up));
    Vec3 u = Cross(r, f);

    float* m = &Camera_m_matView(g_camera);
    float rows[4][4] = {
        {r.x, r.y, r.z, -Dot(r, eye)},
        {u.x, u.y, u.z, -Dot(u, eye)},
        {-f.x, -f.y, -f.z, Dot(f, eye)},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
    std::memcpy(m, rows, sizeof(rows));

    Camera_m_pos(g_camera) = eye;
    Camera_m_actualPos(g_camera) = eye;
    eyeOut = eye;
    fwdOut = f;
}

// --- the hook ---
thread_local bool t_inHelmetPass = false;
const void* g_mainCamera = nullptr; // the camera the game passed to the current RenderGame (GameClient::m_camera)

// What the helmet pass changes about the game's state, only for the nested pass: it is put back right after it, so the
// game's own frame, which runs next, is untouched.
// True when one of the entity's render objects is in the game's red ghost state (see Entity_SetRedGhostEffect above).
bool HasRedGhost(void* renderer, void* entity)
{
    void* ids = static_cast<char*>(entity) + Entity_renderObjects.Offset();
    int idCount = dk2ml::At<int>(ids, ListId_count.Offset());
    const uint32_t* idList = dk2ml::At<const uint32_t*>(ids, ListId_data.Offset());
    void* pool = static_cast<char*>(renderer) + GameRenderer_renderObjectsPool.Offset();
    int poolCount = dk2ml::At<int>(pool, ListObject_count.Offset());
    char* objects = dk2ml::At<char*>(pool, ListObject_data.Offset());
    if (!idList || !objects || idCount <= 0 || idCount > 64) {
        return false;
    }
    for (int i = 0; i < idCount; ++i) {
        if (idList[i] >= static_cast<uint32_t>(poolCount)) {
            continue;
        }
        char* object = objects + static_cast<size_t>(idList[i]) * sRenderObject_size.Get();
        const float* c = reinterpret_cast<const float*>(object + sRenderObject_data.Offset() +
                                                         sRenderObjectData_diffuseColor.Offset());
        if (dk2ml::At<int>(object, sRenderObject_blendMode.Offset()) == 1 && c[0] == 2.0f && c[1] == 0.0f &&
            c[2] == 0.0f && c[3] == 0.5f) {
            return true;
        }
    }
    return false;
}

struct PassOverrides {
    void* renderer = nullptr;
    bool toggledFow = false;
    bool changedPlans = false;
    bool plansBefore = false;
    bool changedNvg = false;
    bool nvgBefore = false;
    std::vector<void*> redDoors; // doors that were red and are plain for the helmet pass

    void Apply(void* r)
    {
        renderer = r;
        if (g_settings.hidePlans) {
            // Only the flag is written, as the replay button does. The button's handler also clears entity state, which
            // would change the real game, so it is not called.
            bool& plans = RenderOptions_plans(static_cast<uint8_t*>(r) + GameRenderer_renderOptions.Offset());
            plansBefore = plans;
            changedPlans = true;
            plans = false;

            // The doors an order will open or kick are red; make them plain for this pass.
            void* list = Door_typeList.Address();
            if (list) {
                void* end = dk2ml::At<void*>(list, DoorList_head.Offset());
                void* node = dk2ml::At<void*>(list, DoorList_next.Offset());
                for (int guard = 0; node && node != end && node != list && guard < 1024; ++guard) {
                    void* door = dk2ml::At<void*>(node, DoorList_owner.Offset());
                    if (door && HasRedGhost(r, door)) {
                        Entity_SetRedGhostEffect(door, false);
                        redDoors.push_back(door);
                    }
                    node = dk2ml::At<void*>(node, DoorList_next.Offset());
                }
            }
        }
        if (g_settings.hideNightVision) {
            bool& nvg = RenderOptions_nvg(static_cast<uint8_t*>(r) + GameRenderer_renderOptions.Offset());
            nvgBefore = nvg;
            changedNvg = true;
            nvg = false; // restored in Restore, so the main view keeps its night vision
        }
        if (g_settings.fowOff) {
            GameRenderer_ToggleFOW(r); // fog of war off (flag: disabled)
            toggledFow = true;
        }
    }

    void Restore()
    {
        if (changedPlans) {
            RenderOptions_plans(static_cast<uint8_t*>(renderer) + GameRenderer_renderOptions.Offset()) = plansBefore;
        }
        if (changedNvg) {
            RenderOptions_nvg(static_cast<uint8_t*>(renderer) + GameRenderer_renderOptions.Offset()) = nvgBefore;
        }
        for (void* door : redDoors) {
            Entity_SetRedGhostEffect(door, true); // red again for the game's own frame
        }
        if (toggledFow) {
            GameRenderer_ToggleFOW(renderer); // and back on
        }
    }
};

// The sun's shadow box is fitted to the camera passed in, which suits the top-down view but not a head-height one
// (outdoor characters came out unlit). While the helmet pass runs, hand it the game's main camera instead, so the sun
// lights the scene as it does in the normal view (R6: only inside our pass).
int ShadowParamsPre(DK2ML_Regs* regs, void*)
{
    if (t_inHelmetPass && g_mainCamera) {
        dk2ml::SetArg<const void*>(regs, 1, g_mainCamera);
    }
    return DK2ML_CALL_ORIGINAL;
}

struct FrameInfo {
    bool active = false; // a corner picture is due after this RenderGame
    int viewW = 0, viewH = 0;
} g_frame;

int RenderGamePre(DK2ML_Regs* regs, void*)
{
    if (t_inHelmetPass) {
        return DK2ML_CALL_ORIGINAL; // our own nested call
    }
    g_frame.active = false;
    if (!g_settings.enabled || !g_open || !InMission()) {
        return DK2ML_CALL_ORIGINAL;
    }

    void* renderer = dk2ml::Arg<void*>(regs, 0);
    const int* viewport = dk2ml::Arg<const int*>(regs, 1);
    const void* mainCamera = dk2ml::Arg<const void*>(regs, 2);
    GlStage("left over from the game, before our pass (not ours)");
    if (!renderer || !viewport || !EnsureResources(viewport[2], viewport[3])) {
        return DK2ML_CALL_ORIGINAL;
    }

    // Test pictures leave the helmet view out: 1 captures the finished normal frame in the post hook, 2 is a flat color.
    if (g_settings.testPicture == 2) {
        FillSolid(1);
        g_frame.active = true;
        g_frame.viewW = viewport[2];
        g_frame.viewH = viewport[3];
        return DK2ML_CALL_ORIGINAL;
    }
    if (g_settings.testPicture == 1) {
        g_frame.active = true;
        g_frame.viewW = viewport[2];
        g_frame.viewH = viewport[3];
        return DK2ML_CALL_ORIGINAL;
    }

    bool alive = false;
    void* target = FindTarget(alive);
    if (target && alive) {
        Vec3 eye, fwd;
        BuildCamera(target, viewport, eye, fwd);

        g_mainCamera = mainCamera;
        PassOverrides overrides;
        overrides.Apply(renderer);
        t_inHelmetPass = true;
        RenderGame(renderer, viewport, g_camera);
        t_inHelmetPass = false;
        overrides.Restore();
        GlStage("in the nested RenderGame");

        CaptureBackBuffer(viewport[2], viewport[3]);
        GlStage("in the back buffer capture blit");

        if (g_helmetFrames < 3) {
            ++g_helmetFrames;
            const float* mv = &Camera_m_matView(g_camera);
            const float* gv = mainCamera ? &Camera_m_matView(mainCamera) : nullptr;
            g_api->Log("helmet pass %d: operator %u eye (%.2f %.2f %.2f) facing (%.2f %.2f %.2f)", g_helmetFrames,
                       Entity_id(target), eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z);
            g_api->Log("  eye sources: fixed (%.2f %.2f %.2f), head bone %s (%.2f %.2f %.2f), using %s", g_lastFixedEye.x,
                       g_lastFixedEye.y, g_lastFixedEye.z, g_lastBoneOk ? "ok" : "unusable", g_lastBoneEye.x,
                       g_lastBoneEye.y, g_lastBoneEye.z, g_lastBoneOk ? "the bone" : "the fixed height");
            g_api->Log("  fog of war off in the helmet view: %d, hide plans: %d (game's plans flag was %d, now restored), "
                       "red doors made plain for this pass: %d, night vision removed: %d (game's flag was %d, now restored)",
                       g_settings.fowOff, g_settings.hidePlans, overrides.plansBefore,
                       static_cast<int>(overrides.redDoors.size()), g_settings.hideNightVision, overrides.nvgBefore);
            g_api->Log("  our view: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f", mv[0], mv[1],
                       mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11]);
            if (gv) {
                g_api->Log("  game view: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f", gv[0], gv[1],
                           gv[2], gv[3], gv[4], gv[5], gv[6], gv[7], gv[8], gv[9], gv[10], gv[11]);
            }
        }
    } else {
        FillSolid(0); // the operator died, or nobody is left: a black picture
    }

    g_frame.active = true;
    g_frame.viewW = viewport[2];
    g_frame.viewH = viewport[3];
    return DK2ML_CALL_ORIGINAL;
}

void RenderGamePost(DK2ML_Regs*, void*)
{
    if (t_inHelmetPass || !g_frame.active) {
        return;
    }
    g_frame.active = false;
    if (!g_fbo) {
        return;
    }
    GlStage("left by the game's own frame (not ours)");
    if (g_settings.testPicture == 1) {
        CaptureBackBuffer(g_frame.viewW, g_frame.viewH); // the normal frame is finished: shrink a copy of it
        GlStage("in the mirror capture blit");
    }
    BlitToCorner(g_frame.viewW, g_frame.viewH);
    GlStage("in the corner blit");
}

// --- events ---
bool g_keyWasDown = false;
bool g_keyIgnored = false;  // pressed while Ctrl or Alt was held: another mod's or the game's combination (R27)
bool g_keyConsumed = false; // this press already did its hold action
ULONGLONG g_keyPressedAt = 0;

void HandleKey()
{
    bool usable = g_settings.enabled && g_settings.key && !g_waitingForKey && InMission() && g_api->IsGameFocused() &&
                  !g_api->IsGameMenuOpen();
    bool down = usable && (GetAsyncKeyState(g_settings.key) & 0x8000) != 0;
    bool vkIsModifier = g_settings.key == VK_CONTROL || g_settings.key == VK_MENU;
    bool chord = !vkIsModifier && ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
    ULONGLONG now = GetTickCount64();

    if (down && !g_keyWasDown) {
        g_keyIgnored = chord; // a key held through a combination waits for its next press
        g_keyConsumed = false;
        g_keyPressedAt = now;
    }

    if (down && !g_keyIgnored && !g_keyConsumed && now - g_keyPressedAt >= static_cast<ULONGLONG>(g_settings.holdMs)) {
        g_keyConsumed = true;
        g_open = !g_open; // hold: open or close
        if (g_open) {
            g_helmetFrames = 0;
            g_glLogBudget = 12;
        }
        g_api->Log("helmet cam %s", g_open ? "opened" : "closed");
    }

    if (!down && g_keyWasDown && !g_keyIgnored && !g_keyConsumed && g_open) {
        g_cycleRequested = true; // tap while open: next operator. A tap while closed does nothing.
    }

    g_keyWasDown = down;
}

ULONGLONG g_lastLog = 0;

void LogOperators()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLog < 1000) {
        return;
    }
    g_lastLog = now;

    std::vector<void*> humans = AllHumans();
    g_api->Log("-- %zu humans, replaying=%d, cam open=%d target=%u --", humans.size(), dk2ml::IsReplaying(g_api),
               g_open, g_targetId);
    for (void* h : humans) {
        const char* name = Entity_name(h);
        Vec3 o = Entity_origin(h);
        Vec3 d = Human_headDirection(h);
        g_api->Log("  id %u '%s' ctrl=%d owner=0x%X hp=%.0f crouch=%d origin (%.2f %.2f %.2f) head dir (%.2f %.2f %.2f)",
                   Entity_id(h), name ? name : "", Human_isControlable(h), Human_ownerMask(h), Entity_health(h),
                   Human_isCrouched(h), o.x, o.y, o.z, d.x, d.y, d.z);
        if (const float* b = HeadBone(h)) {
            g_api->Log("    head bone: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f", b[0], b[1], b[2],
                       b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
        } else {
            g_api->Log("    no head bone (anim manager %p)", Entity_animManager(h));
        }
    }
}

// The operator's name for the label; see the bindings above for the fallback order. Strings are cut at their field size.
std::string OperatorLabel(void* human)
{
    auto bounded = [](const char* s, size_t max) { return s ? std::string(s, strnlen(s, max)) : std::string(); };

    std::string candidates[4];
    candidates[0] = bounded(&Human_humanId(human), 48);
    candidates[1] = bounded(Entity_name(human), 96);
    if (void* tmpl = Entity_pTemplate(human)) {
        candidates[2] = bounded(&HumanTemplate_unitName(tmpl), 48);
        candidates[3] = bounded(&HumanTemplate_className(tmpl), 48);
    }
    for (const std::string& c : candidates) {
        if (c.empty()) {
            continue;
        }
        if (c[0] != '@') {
            return c;
        }
        const char* text = Loca_GetTextForId(c.c_str(), false, false); // null: not a known key, try the next string
        if (text && text[0]) {
            return bounded(text, 96);
        }
    }
    return "Operator";
}

void* TargetHuman()
{
    for (void* h : AllHumans()) {
        if (IsOperator(h) && Entity_id(h) == g_targetId) {
            return h;
        }
    }
    return nullptr;
}

// The name at the top left of the helmet picture: a small borderless, click-through ImGui window, placed in window pixels
// (the same space as the picture). Its default background keeps the text readable on any scene.
void DrawNameLabel()
{
    if (!g_settings.enabled || !g_open || !g_fbo || g_frame.viewW <= 0 || !InMission() ||
        dk2ml::GameMenuOpen(g_api)) {
        return;
    }
    void* human = TargetHuman();
    if (!human) {
        return;
    }

    std::string label = OperatorLabel(human);

    // With the game's Colored plans on (Individual or Class), the chip takes the colour of the operator's path. The game
    // itself says which colour that is (GetColor, 0xAABBGGRR). The text is black or white, whichever reads better on it.
    uint8_t* game = Options_game.Address();
    int coloredPlans = game ? OptionsGame_coloredPlans(game) : 0;
    bool tint = g_settings.colorName && coloredPlans != 0;
    ImVec4 chip = {};
    ImVec4 textColor = {1.0f, 1.0f, 1.0f, 1.0f};
    if (tint) {
        uint32_t argb = Human_GetColor(human);
        // The game packs it as 0xAABBGGRR (red in the low byte): the logged values match the operators' ring colours.
        chip = {static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 16) & 0xFF) / 255.0f, 0.92f};
        float luminance = 0.299f * chip.x + 0.587f * chip.y + 0.114f * chip.z;
        textColor = luminance > 0.6f ? ImVec4{0.0f, 0.0f, 0.0f, 1.0f} : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};

        static uint32_t s_loggedId = 0;
        if (s_loggedId != g_targetId) {
            s_loggedId = g_targetId;
            g_api->Log("name chip colour 0x%08X (coloured plans mode %d) for operator %u '%s'", argb, coloredPlans,
                       g_targetId, label.c_str());
        }
    }

    int flags = static_cast<int>(ImGuiWindowFlags_NoDecoration.Get() | ImGuiWindowFlags_AlwaysAutoResize.Get() |
                                 ImGuiWindowFlags_NoSavedSettings.Get() | ImGuiWindowFlags_NoMouseInputs.Get() |
                                 ImGuiWindowFlags_NoFocusOnAppearing.Get() | ImGuiWindowFlags_NoNav.Get());
    void* context = GImGui ? *GImGui : nullptr;
    // The window background is read inside Begin: set the style colour just for that call, then put it back so no other
    // ImGui window changes.
    ImVec4* background = nullptr;
    ImVec4 savedBackground = {};
    if (tint && context) {
        background = reinterpret_cast<ImVec4*>(static_cast<char*>(context) + ImGuiContext_Style.Offset() +
                                               ImGuiStyle_Colors.Offset()) +
                     ImGuiCol_WindowBg.Get();
        savedBackground = *background;
        *background = chip;
    }
    bool begun = ImGui_Begin("##helmetcam_name", nullptr, flags);
    if (background) {
        *background = savedBackground;
    }
    if (begun) {
        void* window = context ? dk2ml::At<void*>(context, ImGuiContext_CurrentWindow.Offset()) : nullptr;
        if (window) {
            PipRect rect = ComputePipRect(g_frame.viewW, g_frame.viewH);
            ImVec2 pos = {static_cast<float>(rect.x + 8), static_cast<float>(rect.yTop + 8)};
            ImGui_SetWindowPos(window, &pos, static_cast<int>(ImGuiCond_Always.Get()));

            // Twice the default text size for this window only.
            constexpr float kNameScale = 2.0f;
            dk2ml::At<float>(window, ImGuiWindow_FontWindowScale.Offset()) = kNameScale;
            float size = dk2ml::At<float>(context, ImGuiContext_FontBaseSize.Offset()) * kNameScale;
            dk2ml::At<float>(context, ImGuiContext_FontSize.Offset()) = size;
            dk2ml::At<float>(context, ImGuiContext_DrawListSharedData.Offset() + ImDrawListSharedData_FontSize.Offset()) =
                size;
        }
        if (tint) {
            ImGui_TextColored(&textColor, "%s", label.c_str());
        } else {
            ImGui_TextEx(label.c_str(), nullptr, 0);
        }
    }
    ImGui_End(); // always, whatever Begin returned
}

// --- the HUD widget: one Helmet Cam icon in the game's bottom HUD bar; clicking it fans out an arc of buttons ---
// mod\gui\helmetcam_hud.xml (like Free Camera's wheel). There is one main icon per bar ("HUD" live, "HUD_Replay_Full" in
// replays; an item has one parent), and one shared arc ("wheel") that moves into the icon in use while the arc is open (a
// click on the icon opens it, another closes it). The arc holds four controls, each an item pair "_a"/"_b" (the state buttons): selection mode, lock,
// action cam, night vision. Items are found after every GUI_LOADED and moved each time the HUD changes.
dk2ml::Enum EVENT_CLICK{"GUI::Item::eItemEventType", "EVENT_CLICK"};

enum HudControl { kHudSelect, kHudLock, kHudAction, kHudNightVision, kHudControls };

struct HudWidget {
    void* menu[2] = {};    // the main icon items: [0] live bar, [1] replay bar
    void* menuOff[2] = {}; // their buttons: the cam is closed
    void* menuOn[2] = {};  // the cam is open
    void* wheel = nullptr;
    void* buttonA[kHudControls] = {};
    void* buttonB[kHudControls] = {};
};
HudWidget g_hud;
bool g_arcOpen = false; // the main icon was clicked: the arc of buttons is out until it is clicked again

bool HudState(int control) // true: the second ("_b") button
{
    switch (control) {
    case kHudSelect: return g_settings.selectionMode == 1;
    case kHudLock: return g_lockedId != 0;
    case kHudAction: return g_settings.engageSwitch;
    default: return g_settings.hideNightVision;
    }
}

void OnMenuClick(void*, float, float, void*)
{
    g_arcOpen = !g_arcOpen;
}

void OnHudClick(void*, float, float, void* user)
{
    switch (static_cast<int>(reinterpret_cast<intptr_t>(user))) {
    case kHudSelect: g_settings.selectionMode = g_settings.selectionMode == 1 ? 0 : 1; break;
    case kHudLock: ToggleLock(); return;
    case kHudAction: g_settings.engageSwitch = !g_settings.engageSwitch; break;
    default: g_settings.hideNightVision = !g_settings.hideNightVision; break;
    }
    SaveSettings(nullptr, nullptr);
}

void OnGuiLoaded(const DK2ML_Event*, void*)
{
    g_hud = HudWidget{};
    g_arcOpen = false;
    int found = 0;
    int bound = 0;

    static const char* const kMenus[2] = {"#helmetcam_menu", "#helmetcam_menu_replay"};
    for (int i = 0; i < 2; ++i) {
        g_hud.menu[i] = dk2ml::gui::Find(g_api, kMenus[i]);
        char name[64];
        snprintf(name, sizeof(name), "%s_off", kMenus[i]);
        g_hud.menuOff[i] = g_hud.menu[i] ? dk2ml::gui::Find(g_api, name, g_hud.menu[i]) : nullptr;
        snprintf(name, sizeof(name), "%s_on", kMenus[i]);
        g_hud.menuOn[i] = g_hud.menu[i] ? dk2ml::gui::Find(g_api, name, g_hud.menu[i]) : nullptr;
        found += g_hud.menu[i] && g_hud.menuOff[i] && g_hud.menuOn[i];
        bound += g_hud.menuOff[i] && dk2ml::gui::OnAction(g_api, g_hud.menuOff[i], EVENT_CLICK.Get(), OnMenuClick);
        bound += g_hud.menuOn[i] && dk2ml::gui::OnAction(g_api, g_hud.menuOn[i], EVENT_CLICK.Get(), OnMenuClick);
    }

    g_hud.wheel = dk2ml::gui::Find(g_api, "#helmetcam_wheel");
    static const char* const kNames[kHudControls] = {"select", "lock", "action", "nv"};
    for (int i = 0; g_hud.wheel && i < kHudControls; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "#helmetcam_wheel_%s_a", kNames[i]);
        g_hud.buttonA[i] = dk2ml::gui::Find(g_api, name, g_hud.wheel);
        snprintf(name, sizeof(name), "#helmetcam_wheel_%s_b", kNames[i]);
        g_hud.buttonB[i] = dk2ml::gui::Find(g_api, name, g_hud.wheel);
        void* user = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        for (void* button : {g_hud.buttonA[i], g_hud.buttonB[i]}) {
            bound += button && dk2ml::gui::OnAction(g_api, button, EVENT_CLICK.Get(), OnHudClick, user);
            found += button != nullptr;
        }
    }
    g_api->Log("HUD widget: %d of %d items found (2 menu icons, 8 arc buttons), %d callbacks bound, arc %s", found, 2 + 8,
               bound, g_hud.wheel ? "found" : "NOT found");
}

void UpdateHud()
{
    if (!g_hud.wheel) {
        return;
    }
    bool replaying = dk2ml::IsReplaying(g_api);
    int active = replaying ? 1 : 0;
    void* bar = g_settings.enabled && InMission() ? dk2ml::gui::Find(g_api, replaying ? "HUD_Replay_Full" : "HUD") : nullptr;

    // The icon of the bar in use is shown, the other one hidden (an item has one parent, so each bar has its own).
    for (int i = 0; i < 2; ++i) {
        void* menu = g_hud.menu[i];
        if (!menu) {
            continue;
        }
        bool here = bar && i == active;
        if (here && dk2ml::gui::Parent(g_api, menu) != bar && dk2ml::gui::AddChild(g_api, bar, menu)) {
            g_api->Log("HUD widget: menu icon placed in %s", replaying ? "HUD_Replay_Full" : "HUD");
        }
        if (dk2ml::gui::IsShown(g_api, menu) != here) {
            dk2ml::gui::Show(g_api, menu, here);
        }
    }

    void* menu = g_hud.menu[active];
    if (!bar) {
        g_arcOpen = false; // left the mission, or the mod is off
    }
    bool arc = bar && menu && g_arcOpen;
    if (!arc) {
        if (dk2ml::gui::IsShown(g_api, g_hud.wheel)) {
            dk2ml::gui::Show(g_api, g_hud.wheel, false);
        }
    }
    if (!bar || !menu) {
        return;
    }

    // The main icon: amber while the cam is open.
    if (g_hud.menuOff[active] && dk2ml::gui::IsShown(g_api, g_hud.menuOff[active]) == g_open) {
        dk2ml::gui::Show(g_api, g_hud.menuOff[active], !g_open);
    }
    if (g_hud.menuOn[active] && dk2ml::gui::IsShown(g_api, g_hud.menuOn[active]) != g_open) {
        dk2ml::gui::Show(g_api, g_hud.menuOn[active], g_open);
    }

    if (!arc) {
        return;
    }
    if (dk2ml::gui::Parent(g_api, g_hud.wheel) != menu) {
        dk2ml::gui::AddChild(g_api, menu, g_hud.wheel);
    }
    if (!dk2ml::gui::IsShown(g_api, g_hud.wheel)) {
        dk2ml::gui::Show(g_api, g_hud.wheel);
    }
    for (int i = 0; i < kHudControls; ++i) {
        bool b = HudState(i);
        if (g_hud.buttonA[i] && dk2ml::gui::IsShown(g_api, g_hud.buttonA[i]) == b) {
            dk2ml::gui::Show(g_api, g_hud.buttonA[i], !b);
        }
        if (g_hud.buttonB[i] && dk2ml::gui::IsShown(g_api, g_hud.buttonB[i]) != b) {
            dk2ml::gui::Show(g_api, g_hud.buttonB[i], b);
        }
    }
}

void OnFrame(const DK2ML_Event*, void*)
{
    if (g_settings.logOperators && InMission()) {
        LogOperators();
    }
    HandleKey();
    if (InMission()) {
        UpdateAutoTarget();
    }
    DrawSettingsWindow();
    DrawNameLabel();
    UpdateHud();
}

void OnStateChanged(const DK2ML_Event* e, void*)
{
    if (e->newState != StateRunning.Get()) {
        if (g_open) {
            g_api->Log("helmet cam closed: left the mission");
        }
        ResetCam();
        g_windowOpen = false; // the Esc-menu button only exists in a mission
    }
}

void OnSettingsButton(void*, float, float, void*)
{
    g_windowOpen = !g_windowOpen;
}

void OnMapLoaded(const DK2ML_Event*, void*)
{
    ResetCam(); // a restart or a new mission: the old operators are gone
    g_helmetFrames = 0;
}

} // namespace

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)
{
    g_api = api;
    LoadSettings(info);

    if (!dk2ml::ResolveAll(api)) { // logs every name this game build doesn't have
        return 1;
    }
    if (Camera_size.Get() > sizeof(g_camera)) {
        api->Log("Camera is %u bytes, more than the %zu reserved", Camera_size.Get(), sizeof(g_camera));
        return 2;
    }

    if (!AddOptions()) {
        return 5;
    }

    // The Esc menu's button opens the settings window. It borrows the game's own night-vision goggles icon (a head-worn
    // device, drawn in the same round badge as the other buttons), so there's no art to ship. If a game update moves
    // these textures, set `label = "CAM"` instead.
    DK2ML_MenuButton button = {sizeof(button)};
    button.tooltip = "Helmet Cam settings";
    button.icon = "data/textures/hud/toggle_nvg_off_normal.dds";
    button.iconHover = "data/textures/hud/toggle_nvg_off_hover.dds";
    button.onClick = OnSettingsButton;
    if (!dk2ml::gui::AddMenuButton(api, button)) {
        api->Log("no Esc-menu button (the settings are still on the Modloader page)");
    }

    if (!dk2ml::On(api, DK2ML_EVENT_FRAME, OnFrame) || !dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnStateChanged) ||
        !dk2ml::On(api, DK2ML_EVENT_MAP_LOADED, OnMapLoaded) || !dk2ml::On(api, DK2ML_EVENT_GUI_LOADED, OnGuiLoaded)) {
        return 3;
    }
    if (!dk2ml::Hook(api, RenderGame, RenderGamePre, RenderGamePost)) {
        return 4;
    }
    if (!dk2ml::Hook(api, GetShadowMapCameraParams, ShadowParamsPre)) {
        return 6;
    }
    if (!dk2ml::Hook(api, Human_Net_ReceiveEvent, ReceiveEventPre)) { // a no-op unless engagement switching is on
        return 7;
    }

    api->Log("ready (spike): hold vk=0x%X for %d ms to open or close the cam, tap to cycle", g_settings.key,
             g_settings.holdMs);
    return 0;
}
