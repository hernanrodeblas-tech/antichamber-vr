#pragma once
// Thin wrapper over the OpenVR C API (function tables), plus a headset-less
// fake mode used to exercise the stereo pipeline during development.
#include <cstdint>
#include "mathutil.h"

namespace vr {

struct EyeInfo {
    float tanLeft, tanRight, tanUp, tanDown; // positive magnitudes
    Mat34 eyeToHead;
    float uMin, vMin, uMax, vMax;            // crop inside the render (the whole image when asymmetric)
    // Projection of this eye on the rendered image: ndcX = projX * x/z + offX, ndcY = projY * y/z + offY
    // (view space x right, y up, z forward; ndc -1..1, y up). Symmetric: offsets are 0.
    float projX, projY, offX, offY;
};

struct Setup {
    bool fake = false;
    uint32_t recW = 0, recH = 0;       // SteamVR recommended per-eye size
    uint32_t renderW = 0, renderH = 0; // size the game must render at (symmetric frustum)
    float tanH = 1, tanV = 1;          // symmetric half-FOV tangents
    float fovDeg = 90;                 // horizontal FOV to give Unreal
    bool asymmetric = false;           // each eye renders its own off-centre frustum (AsymmetricFrustum=1)
    float lineTanH = 1, lineTanV = 1;  // half-tangent spanned by the render (for the linework fix)
    float displayHz = 90;
    float vsyncToPhotons = 0.011f;
    int adapterIndex = 0;
    EyeInfo eye[2];
};
extern Setup S;

struct Hand {
    bool valid = false;
    Mat34 raw = Mat34::Identity(); // device pose, standing universe
    Mat34 aim = Mat34::Identity(); // pointing pose (-Z forward)
    float stickX = 0, stickY = 0;
    float trigger = 0, grip = 0;
    bool triggerBtn = false, gripBtn = false, aBtn = false, bBtn = false, stickBtn = false;
};

struct Frame {
    bool hmdValid = false;
    Mat34 hmd = Mat34::Identity();
    Hand hand[2]; // 0 = left, 1 = right
};

bool HmdPresent();          // cheap check, before Init
bool Init();                // game thread or render thread, once
bool Active();
void ComputeFrustum();      // (re)computes the render size and per-eye projections from the config
void Shutdown();

void GetFrame(Frame& out);  // predicted poses + controller state (game thread)
void WaitGetPoses();        // render thread, once per stereo frame
bool Submit(void* d3d11TexLeft, void* d3d11TexRight, const Mat34& hmdPoseUsed);
void PollEvents(bool& quitRequested);
void Haptic(int hand, int microseconds);

} // namespace vr
