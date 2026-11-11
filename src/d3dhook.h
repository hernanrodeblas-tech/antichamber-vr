#pragma once
#include <d3d9.h>
#include "mathutil.h"

namespace d3dhook {

// Called by the proxy's Direct3DCreate9. Returns the interface the game will use.
IDirect3D9* CreateD3D(UINT sdk, HMODULE realD3D9);

bool VrMode();                                // VR pipeline requested and initialised
bool StereoReady();                           // eye targets exist; stereo draws may start
unsigned NoteDraw();                           // game thread: count a presenting viewport draw
void PushEye(int eye, const Mat34& hmdPose, unsigned drawOrdinal); // tag that draw's Present
unsigned StereoFrames();
void BackBufferSize(unsigned& w, unsigned& h); // size of the game's current back buffer
void BeginDesktopSpoof();
void RequestShot();                          // save both eye images on the next stereo frame
DWORD RenderThreadId();                      // thread that presents (the engine's render thread)

} // namespace d3dhook
