# Antichamber VR

An unofficial VR mod for [Antichamber](https://store.steampowered.com/app/219890/Antichamber/) (Steam version) with
6DoF head tracking, roomscale, motion controls and a settings launcher. It works through SteamVR (OpenVR), has been
played with the Steam Frame, and also supports Quest Touch controllers.

> Not affiliated with Alexander Bruce or Demruth. You need your own copy of Antichamber on Steam.
> The mod does not modify any game file: it is a `d3d9.dll` loaded next to the game executable.

**[Español más abajo](#español)**

## Features

- **Stereo rendering at the headset's resolution**, submitted to SteamVR through shared D3D9Ex / D3D11 textures.
- **6DoF and roomscale**: the camera follows your head and walking moves the character, with collision.
- **Standing or seated play**, with recentering.
- **Motion controls**: the gun is held in your hand and aims where it points, with a 3D aim dot. Snap turn
  (15/30/45/90°) or smooth turn, and head- or hand-relative movement.
- **Antichamber-specific fixes for VR:**
  - Seamless teleports stay seamless (the game's rotation is folded into the playspace).
  - The line/shading detector is retuned for the headset FOV (no striped shading).
  - Mirrors and portals are rendered per eye, with depth, at the headset resolution.
  - The block placement outline shows in both eyes.
  - The gun's rotating ring and ammo are drawn with the world projection.
- **Launcher** (`AntichamberVR.exe`) with every option, in English or Spanish.
- **Runs standalone on the Steam Frame** (no PC), through Proton: see [Steam Frame (standalone)](#steam-frame-standalone).
- Bonus: fixes the start-up crash some systems get with NVIDIA's PhysX installed.

## Requirements

- Antichamber on Steam (Windows, the current Steam build: UDK 7977, 32-bit DX9).
- SteamVR and a headset. Tested with the Steam Frame.
- For the launcher: the [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0) (x64).

## Install

1. Download the zip from [Releases](../../releases).
2. Extract it into the Antichamber folder (Steam → Antichamber → Manage → Browse local files), so that
   `AntichamberVR.exe` sits next to `Binaries`.
3. Start SteamVR with your headset connected, open `AntichamberVR.exe`, adjust what you like and press **Play in VR**.

Launching Antichamber from Steam directly also works; the mod reads `Binaries\Win32\antichamber_vr.ini`. Without a
headset (or with VR disabled) the game runs flat as usual.

## Steam Frame (standalone)

The mod also runs directly on the Steam Frame, without a PC: SteamOS runs the Windows game through Proton and FEX.
The launcher is not used there; the settings come from a ready-made ini.

1. Install Antichamber on the Frame. In its **Properties → Compatibility**, force **Proton Experimental**
   (Proton 11.0 (ARM64) crashes the game while loading, with or without the mod).
2. Switch to desktop mode, download the release zip and extract it into the Antichamber folder
   (Steam → Antichamber → Manage → Browse local files).
3. Copy `SteamFrame/antichamber_vr.ini` over `Binaries/Win32/antichamber_vr.ini`.
4. Before the first launch, replace the game's 2010 PhysX installer, which never finishes under Proton/FEX (Steam
   stays on *Running install script*): in `Binaries/Redist/physx`, rename `PhysX_9.10.0224_SystemSoftware.exe` to
   `PhysX_9.10.0224_SystemSoftware.exe.orig` and copy `SteamFrame/noop_installer.exe` there under the original name.
   The game does not need it, because the mod loads the game's own PhysX. (*Verify integrity of game files* restores
   the original installer.)
5. In Antichamber's **Properties → General → Launch options**, enter:
   ```
   WINEDLLOVERRIDES="d3d9=n,b" %command%
   ```
6. Back in the headset, start Antichamber from the library.

To change a setting, edit `Binaries/Win32/antichamber_vr.ini` (same keys as the launcher). If something goes wrong,
the log is `Binaries/Win32/antichamber_vr.log`.

### Performance on the Steam Frame

The Frame runs Antichamber as an emulated x86 game (FEX) on an ARM chip, so what limits it is not the GPU but the
CPU, and in particular **one thread: the engine's render thread** (the one that issues every draw call for both eyes).
Measured with the `perf:` lines the mod writes to the log, the game thread needs 8–10 ms per frame and the scripts only
2–4 ms, while the render thread needed 14–18 ms. Lowering the resolution changed nothing; these did:

| Setting (Frame preset) | What it does | Effect measured on the Frame |
| --- | --- | --- |
| `DynamicShadows=0` | No dynamic shadows: each shadowed light re-draws the scene from the light, for each eye | Biggest single gain: average ~35 → ~50 fps |
| `PipelineFrames=1` | Each frame is handed to SteamVR one frame later, so the CPU never waits for the GPU | ~10 ms of waiting per frame removed |
| `AsymmetricFrustum=1` | Each eye renders only its own field of view (1728×1728 instead of 2004×2076) | ~28 % fewer pixels, fewer objects to draw; render thread 14–18 → 9–16 ms |
| `RenderThreadCore=7`, `CpuCores=2-7` | Pins the render thread to the chip's fast core and lets the game use cores 2–7 | Light rooms reach **90 fps at 90 Hz**; average ~51 fps over a long session |

**The CPU-core finding.** The Frame's Snapdragon 8 Gen 3 has two small cores (0–1), five medium cores (2–6) and one
fast core (7), and under emulation the fast core is much quicker than the others. SteamOS starts the game restricted to
some of the cores (3–7 on the unit we tested), and the render thread, which limits everything, was being moved around
the medium cores. Pinning it to core 7 (and keeping the game thread off that core) took light rooms from 45 to 80–90 fps;
heavy rooms stay at 45. The headset runs warmer this way; remove `RenderThreadCore=7` from the ini if that bothers you.
The idea comes from DDD's [Deadlock Steam Frame patch](https://steamcommunity.com/sharedfiles/filedetails/?id=3834239849),
which moves Deadlock to the fast cores for the same reason.

Tried without a measurable gain: turning off depth of field, ambient occlusion, light shafts, lens flares, decals or
distortion (`EngineSettings`), per-eye scene captures (`StereoCaptures=0`) and occlusion queries (`OcclusionQueries=0`).
To measure on your own unit, the log has a `perf:` line every few seconds (fps, frame time and its split) and a
`perf cpu busy per frame` line (CPU time of the game and render threads).

**Uninstall:** delete `AntichamberVR.exe` and, from `Binaries\Win32`, `d3d9.dll`, `openvr_api.dll`,
`antichamber_vr.ini` and `antichamber_vr.log`.

## Controls

| Action | Controller |
| --- | --- |
| Move | Free-hand stick |
| Turn (snap or smooth) | Gun-hand stick |
| Primary fire / press signs | Gun trigger |
| Secondary fire | Gun grip |
| Third gun mode | Other hand's trigger, or B |
| Jump | A or X |
| Back to the antechamber (hold to quit) | Y |
| Recenter (and recalibrate height when seated) | Right stick click (keyboard F8) |
| Diagnostic snapshot of both eyes | Both sticks clicked (keyboard F9) |

The gun is in the right hand by default; *Gun hand: Left* swaps the roles of both hands.

## Settings

Everything is in the launcher, which writes `Binaries\Win32\antichamber_vr.ini`. Notable options:

| Option | Meaning |
| --- | --- |
| `PlayMode`, `SeatedEyeHeight` | Standing (real height) or seated (fixed eye height) |
| `SnapTurn`, `SnapTurnDegrees`, `SmoothTurnSpeed` | Turning |
| `MaxRenderWidth`, `ResolutionScale` | Per-eye resolution |
| `CaptureScale` | Mirror/portal resolution relative to the eye (game default 0.75) |
| `WorldScale` | Unreal units per metre (default 50) |
| `LineworkFix`, `LineworkScale` | Line/shading detector retuned for the headset FOV |
| `StereoCaptures`, `ResizeScreenTargets`, `EyeTileFix` | Per-eye mirrors, full-res mirrors, outline in both eyes |
| `GunScale`, `GunOffsetX/Y/Z`, `AimPitchOffset` | Gun model and aiming |
| `DynamicShadows`, `MotionBlur` | The game's dynamic shadows; camera motion blur (off by default) |
| `PipelineFrames` | CPU and GPU work in parallel: more fps on slow systems, one frame more latency |
| `AsymmetricFrustum` | Each eye renders only its own field of view: ~28% fewer pixels and fewer objects to draw |
| `CpuCores`, `RenderThreadCore` | CPU cores the game may use; core the render thread is pinned to (Steam Frame: `2-7`, `7`) |
| `D3DBridge` | How images reach SteamVR (automatic by default) |

## Known limitations

- After a VR session the game keeps the headset resolution in its own `UDKEngine.ini`; when playing flat
  afterwards, pick a resolution again on the in-game options wall.
- Only the Steam build of Antichamber is supported (the mod checks code signatures and falls back to flat if they
  don't match).
- Uses SteamVR's legacy controller input (no custom action bindings yet).

## Building

Requirements: Git Bash (or any POSIX shell), curl and unzip; the .NET 8 SDK for the launcher.

```sh
sh scripts/setup_toolchain.sh   # downloads llvm-mingw (32-bit) into tools/
sh build.sh                     # builds build/d3d9.dll
sh build.sh install             # ...and copies it (plus openvr_api.dll) into ../Binaries/Win32
```

Clone the repository inside the Antichamber folder (e.g. `Antichamber/VRMod`) so `install` finds the game.

Launcher:

```sh
cd launcher
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -p:DebugType=none -o ../build/launcher
```

## How it works

`d3d9.dll` is a proxy for the system Direct3D 9 library. It gives the game a D3D9Ex device, renders every frame
twice (once per eye) by hooking `FViewport::Draw`, and hooks the UnrealScript interpreter
(`UObject::ProcessInternal`) to replace the player's viewpoint, FOV, aim and input. UE3 objects, properties and
functions are found at runtime through `GNames`/`GObjects`, so no game SDK or script compiler is needed. The details,
and the history of every VR issue and how it was diagnosed, are in [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md)
(in Spanish).

## License

MIT, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Español

Mod de realidad virtual no oficial para Antichamber (versión de Steam): seguimiento 6DoF, roomscale, mandos de
movimiento y un launcher con todas las opciones. Funciona con SteamVR, se ha jugado con las Steam Frame (también sin PC, ver más abajo)
y también admite los mandos Quest Touch. No tiene relación con los autores del juego; necesitas tu propia copia en Steam.

**Instalación:** descarga el zip de [Releases](../../releases), extráelo en la carpeta de Antichamber
(`AntichamberVR.exe` junto a `Binaries`), abre SteamVR con el visor conectado, ejecuta `AntichamberVR.exe` y pulsa
**Jugar en VR**. El launcher está en inglés por defecto; el botón **Español** lo cambia. El launcher necesita el
runtime de escritorio de .NET 8.

**Controles:** stick de la mano libre para moverte, stick del arma para girar (por pasos de 15/30/45/90° o suave),
gatillo del arma para disparar, grip para el disparo secundario, gatillo de la otra mano o B para el tercer modo,
A o X para saltar, Y para volver a la antecámara, stick derecho para recentrar (y recalibrar la altura sentado),
ambos sticks para una captura de diagnóstico.

**Modo sentado:** en el launcher, *Modo de juego: Sentado*; la vista se coloca a la altura de ojos indicada.

### Steam Frame (sin PC)

El mod funciona directamente en las Steam Frame, sin PC: SteamOS ejecuta el juego de Windows con Proton y FEX. Allí no
se usa el launcher; la configuración viene en un ini preparado.

1. Instala Antichamber en las Frame. En **Propiedades → Compatibilidad**, fuerza **Proton Experimental**
   (con Proton 11.0 (ARM64) el juego se cierra al cargar, con o sin el mod).
2. Pasa al modo escritorio, descarga el zip de la release y extráelo en la carpeta de Antichamber
   (Steam → Antichamber → Administrar → Ver archivos locales).
3. Copia `SteamFrame/antichamber_vr.ini` encima de `Binaries/Win32/antichamber_vr.ini`.
4. Antes de abrir el juego por primera vez, sustituye el instalador de PhysX de 2010, que nunca termina en Proton/FEX
   (Steam se queda en *Ejecutando script de instalación*): en `Binaries/Redist/physx`, renombra
   `PhysX_9.10.0224_SystemSoftware.exe` a `PhysX_9.10.0224_SystemSoftware.exe.orig` y copia ahí
   `SteamFrame/noop_installer.exe` con el nombre original. El juego no lo necesita, porque el mod usa el PhysX del
   propio juego. (*Verificar integridad de los archivos* devuelve el instalador original.)
5. En **Propiedades → General → Opciones de lanzamiento** de Antichamber, escribe:
   ```
   WINEDLLOVERRIDES="d3d9=n,b" %command%
   ```
6. Vuelve al visor y abre Antichamber desde la biblioteca.

Para cambiar algo, edita `Binaries/Win32/antichamber_vr.ini` (las mismas claves que el launcher). El registro está en
`Binaries/Win32/antichamber_vr.log`.

**Rendimiento en las Frame.** Las Frame ejecutan Antichamber emulado (FEX, de x86 a ARM), así que lo que limita no es
la GPU sino el procesador, y en concreto **un solo hilo: el de render del motor**, que manda todos los dibujos de los dos
ojos. Medido con las líneas `perf:` del registro: el hilo del juego necesita 8–10 ms por fotograma y los scripts 2–4 ms,
pero el de render necesitaba 14–18 ms. Bajar la resolución no cambiaba nada; esto sí:

| Ajuste (ini de las Frame) | Qué hace | Efecto medido en las Frame |
| --- | --- | --- |
| `DynamicShadows=0` | Sin sombras dinámicas: cada luz con sombras vuelve a dibujar la escena, por ojo | La mayor mejora: media de ~35 a ~50 fps |
| `PipelineFrames=1` | Cada fotograma se entrega a SteamVR uno más tarde, y la CPU nunca espera a la GPU | Quita ~10 ms de espera por fotograma |
| `AsymmetricFrustum=1` | Cada ojo dibuja solo su campo de visión (1728×1728 en vez de 2004×2076) | ~28 % menos píxeles y menos objetos; hilo de render de 14–18 a 9–16 ms |
| `RenderThreadCore=7`, `CpuCores=2-7` | Fija el hilo de render al núcleo rápido del chip y deja usar los núcleos 2–7 | Las salas ligeras llegan a **90 fps a 90 Hz**; media de ~51 fps en sesión larga |

**El hallazgo de los núcleos.** El Snapdragon 8 Gen 3 de las Frame tiene dos núcleos pequeños (0–1), cinco medianos (2–6)
y uno rápido (7), y con emulación el rápido rinde bastante más que los demás. SteamOS arranca el juego limitado a parte de
los núcleos (3–7 en el visor de pruebas) y el hilo de render, que es el que limita todo, iba saltando entre los medianos.
Fijarlo al núcleo 7 (y apartar de él el hilo del juego) llevó las salas ligeras de 45 a 80–90 fps; las pesadas siguen a 45.
El visor se calienta algo más; si te molesta, quita `RenderThreadCore=7` del ini. La idea viene del
[parche de Deadlock para Steam Frame](https://steamcommunity.com/sharedfiles/filedetails/?id=3834239849) de DDD, que mueve
Deadlock a los núcleos rápidos por el mismo motivo.

Probado sin mejora medible: quitar profundidad de campo, oclusión ambiental, rayos de luz, destellos, calcomanías o
distorsión (`EngineSettings`), las capturas por ojo (`StereoCaptures=0`) y las consultas de oclusión
(`OcclusionQueries=0`). Para medir en tu visor, el registro tiene una línea `perf:` cada pocos segundos y otra
`perf cpu busy per frame` con el tiempo de CPU de los hilos del juego y de render.

**Limitación conocida:** tras jugar en VR, el juego guarda la resolución del visor en su configuración; si luego
juegas en plano, elige otra vez la resolución en la pared de opciones del juego.

**Desinstalar:** borra `AntichamberVR.exe` y, en `Binaries\Win32`, `d3d9.dll`, `openvr_api.dll`,
`antichamber_vr.ini` y `antichamber_vr.log`.

Las notas técnicas completas (en español) están en [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md).
