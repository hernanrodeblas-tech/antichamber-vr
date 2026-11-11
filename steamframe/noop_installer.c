/* Stand-in for a redistributable installer that never finishes under Proton/FEX (e.g. on the
   Steam Frame). It does nothing and reports success, so Steam's first-launch install script can
   move on. Antichamber VR loads the game's own PhysX from Binaries\Win32 (FixPhysX=1). */
#include <windows.h>
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d) { (void)a; (void)b; (void)c; (void)d; return 0; }
