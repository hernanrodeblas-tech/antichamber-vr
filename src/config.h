#pragma once

struct Config {
    bool enabled = true;          // master switch for VR
    bool fakeHmd = false;         // run the stereo pipeline without a headset (debug)
    float worldScale = 50.0f;     // Unreal units per metre
    float heightOffset = 0.0f;    // metres added to the tracked head height
    bool seated = false;          // seated play: head height is set to seatedEyeHeight (recalibrated with recenter)
    float seatedEyeHeight = 1.65f; // metres, standing eye height to use when seated
    float resolutionScale = 1.0f; // multiplier on SteamVR's recommended size
    int maxRenderWidth = 0;       // 0 = no cap
    bool snapTurn = true;
    float snapTurnDegrees = 30.0f;
    float smoothTurnSpeed = 120.0f; // degrees per second
    bool moveRelativeToHand = false;
    bool leftHanded = false;
    float stickDeadzone = 0.2f;
    float aimPitchOffset = 0.0f;  // degrees, tilts the aim ray down (+) / up (-)
    float gunOffsetX = 0.0f, gunOffsetY = 0.0f, gunOffsetZ = 0.0f; // weapon mesh offset (uu)
    float gunScale = 1.0f;        // size multiplier for the gun model
    bool roomscale = true;
    float nearClip = 10.0f;        // near clipping plane in Unreal units (engine default 10)
    bool crosshair = true;
    bool stereoCaptures = true;    // place the game's screen-space scene captures per eye
    bool eyeOcclusionFix = false;  // ignore occlusion queries for the right eye
    bool occlusionQueries = true;   // false: skip the engine's occlusion queries in both eyes (render-thread cost)
    bool asymmetricFrustum = false; // each eye renders only its own off-centre frustum (fewer pixels and objects)
    bool eyeFrameAdvance = false;  // each eye counts as its own engine frame (per-frame captures, outlines)
    bool eyeTileFix = true;       // redraw the block placement outline (one-render debug lines) for the right eye
    bool dynamicShadows = true;   // the game's screen-space hatched shadows (look wrong in stereo)
    bool motionBlur = false;       // the engine's camera motion blur (never wanted in VR)
    char engineSettings[256] = {}; // extra engine scalability settings: "Name=Value, Name=Value" (applied with "scale set")
    char cpuCores[32] = {};       // CPU cores the game may use, e.g. "2-7" (Steam Frame: medium and fast cores)
    int renderThreadCore = -1;     // pin the engine render thread to this core (Steam Frame: 7, the fast one); -1 = off
    bool lineworkFix = true;      // scale the line detector's sampling distance to the headset FOV
    bool resizeScreenTargets = true; // let the game resize its capture textures to the headset resolution
    float captureScale = 1.0f;    // capture-texture size relative to the eye resolution (game default 0.75; 0 = leave as is)
    float lineworkScale = 1.0f;   // extra multiplier on that distance (lower = fewer false lines)
    int d3dBridge = 0;            // first eye-image bridge to try: 0 shared D3D9->D3D11, 1 shared D3D11->D3D9, 2 system-memory copy
    bool pipelineFrames = false;   // submit each frame one frame later so CPU and GPU overlap (more fps, one frame more latency)
    bool fixPhysX = true;
    int debugDumpFrame = 0;       // >0: dump both eye images at that stereo frame
    bool debugLog = false;
    bool debugTile = false;        // log how the block placement preview is shown (diagnostics)
    bool debugFireTrace = false;   // log the script functions that run while the gun trigger is held
    bool debugProfile = false;     // log the UnrealScript functions that cost the most game-thread time
    bool debugProjection = false;  // log which code builds each perspective projection (asymmetric rendering research)
    bool debugEvents = false;      // log window focus, SteamVR dashboard/focus events and audio output changes
    // Development aids (fake-headset testing)
    int debugJumpNode = -1;       // map node to jump to after start-up (as if clicked on the map wall)
    float debugFakeYaw = 0, debugFakePitch = 0; // fake head orientation (degrees)
    int debugWalkMs = 0;          // walk forward this long, 2 s after the jump
    bool debugNoLinework = false; // disable the Linework post-process effect
    bool debugJumpDelayed = false; // jump with DelayedJump instead of a simulated click
    float debugTeleport[4] = {0, 0, 0, 0}; // x y z yaw(unreal units): place the player there (0 0 0 = off)
    float debugCapOffset = 0;     // raise the right eye's capture camera (per-eye capture test)
};

extern Config g_cfg;
void LoadConfig();
