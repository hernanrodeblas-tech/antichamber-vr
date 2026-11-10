# Antichamber VR — notas de desarrollo

Diario técnico del desarrollo del mod (en español): cómo se diagnosticó y resolvió cada problema de VR.
El README principal está en la raíz del repositorio.


Mod de realidad virtual para Antichamber (Steam, UDK build 7977, 32 bits, DirectX 9):
6DoF, roomscale y mandos de movimiento a través de SteamVR (OpenVR).

## Cómo funciona

- `Binaries\Win32\d3d9.dll` es una DLL proxy que el juego carga sola. No se modifica ningún archivo original.
- Cada fotograma se dibuja dos veces (una por ojo) enganchando `FViewport::Draw`; las imágenes van al
  compositor de SteamVR mediante texturas compartidas D3D9Ex -> D3D11, sin copias por CPU.
- El intérprete de UnrealScript (`UObject::ProcessInternal`) está enganchado para sustituir el punto de
  vista del jugador por la pose de la cabeza, y por la de la mano para todo el código del arma e interacción.
- Caminar físicamente mueve al personaje con colisión (roomscale).
- Incluye una corrección independiente de la VR: fuerza el PhysX 2.8.4.4 del propio juego. Sin ella el
  juego se cierra al cargar en equipos con el PhysX System Software de NVIDIA instalado.

## Uso

1. Enciende el visor y deja SteamVR conectado.
2. Lanza Antichamber desde Steam.
3. Si no hay visor conectado, el juego arranca en modo plano normal.

Registro: `Binaries\Win32\antichamber_vr.log`. Configuración: `Binaries\Win32\antichamber_vr.ini`.

## Controles (Steam Frame, Quest Touch y cualquier mando con entrada clásica de SteamVR)

| Acción | Mando |
| --- | --- |
| Moverse | Stick izquierdo (dirección según la cabeza; velocidad analógica) |
| Girar | Stick derecho (por pasos de 30°, o suave con `SnapTurn=0`) |
| Disparo primario / pulsar carteles | Gatillo derecho |
| Disparo secundario | Agarre (grip) derecho |
| Tercer modo del arma (clic central) | Gatillo izquierdo o botón B |
| Saltar | Botón A o X |
| Volver a la antecámara (mantener para salir) | Botón Y |

`LeftHanded=1` intercambia las manos. El punto blanco/verde marca dónde apunta el arma.

## Compilar

```
sh build.sh install
```

Usa el llvm-mingw incluido en `tools/`. `install` copia `d3d9.dll` y `openvr_api.dll` a `Binaries\Win32`
(el juego debe estar cerrado).

## Desinstalar

Borra de `Binaries\Win32`: `d3d9.dll`, `openvr_api.dll`, `antichamber_vr.ini`, `antichamber_vr.log`.

## Notas de la segunda iteración

- Teletransportes: el giro que el juego aplica al jugador se traslada a la rotación del espacio de juego,
  de modo que el corte no se ve (igual que en pantalla plana).
- El arma se coloca en la mano y se dibuja con la proyección del mundo (su malla usaba un FOV propio de 60°).
  En la antecámara el juego la oculta, como en el original.
- `MaxRenderWidth=3000` por defecto: con supermuestreo alto en SteamVR el juego bajaba de 90 fps.
- Si se cambia la resolución desde la pared de opciones, el mod repone la del visor a los pocos segundos.
- `DebugLog=1` registra las funciones de script que se ejecutan mientras se dibuja cada ojo.

## Notas de la tercera iteración

- El aro giratorio del arma (`HazardWeap_TileGunRotator`) tiene su propia malla con FOV de 60°: ahora todas las
  mallas del arma, del aro y de la munición se dibujan con la proyección del mundo, y el aro se mueve a la mano.
- Recuadro de colocación en un solo ojo: el HUD volvía a ejecutar `ActiveRenderOverlays` → `CalcWeaponFire` tras el
  ojo izquierdo y ese cambio llegaba al derecho. Esa lógica (y `InteractTest`) se ejecuta ahora una sola vez, tras
  el ojo derecho, igual que en pantalla plana.
- Retirado el recálculo por ojo de las salas vistas a través de cristales: no mejoraba y daba artefactos.

## Notas de la cuarta iteración

- Antichamber dibuja buena parte de la imagen (las líneas negras y grises, las sombras, las salas vistas a través
  del mapa o de cristales) con capturas de escena que compone en espacio de pantalla. El juego las coloca desde la
  cámara plana (`UpdateWorldCamera` / `UpdateChamberCamera`): una vez por fotograma, desde el centro de la cabeza,
  sin roll y con FOV 90. Ahora se colocan para cada ojo con su pose exacta y el FOV del visor (`StereoCaptures=1`).
  El intento anterior conservaba el FOV 90 y por eso daba artefactos.
- Recuadro del bloque en un solo ojo: el ojo derecho hereda las consultas de oclusión del izquierdo dentro del mismo
  fotograma, y el recuadro, que el juego recrea cada fotograma, quedaba descartado. El ojo derecho ignora ahora la
  oclusión (`EyeOcclusionFix=1`, usa la misma variable que el comando de consola `TOGGLEOCCLUSION`).
- Resultado con visor: ni `StereoCaptures` ni `EyeOcclusionFix` resolvieron nada. Ambos quedan desactivados por
  defecto (siguen disponibles en el ini). Pruebas en modo simulado: las capturas parecen renderizarse una sola vez
  por fotograma, no por ojo.

## Captura de diagnóstico

Con el visor puesto, **F9** o **los dos sticks pulsados a la vez** guardan la imagen de cada ojo en
`Binaries\Win32\vr_shotNN_eye0.jpg` / `vr_shotNN_eye1.jpg` (los mandos vibran) y anotan el estado del juego en
`antichamber_vr.log` (líneas `SHOT:`).

- Herramientas de desarrollo: `scripts\shot.ps1` lanza el juego con visor simulado, puede saltar a una sala del mapa
  (`-Node`), girar la cabeza (`-Yaw`/`-Pitch`) y guarda las imágenes de ambos ojos en `build\`.
- Limitación conocida: el cambio a la resolución del visor queda guardado en `UDKGame\Config\UDKEngine.ini`;
  si juegas en plano después, cambia la resolución desde la pared de opciones.

## Estado

Verificado en modo de visor simulado (`FakeHMD=1`): render estéreo a resolución completa, cámara a la
altura de la cabeza, puntero de apuntado, trazado desde la mano, ~90 fps estéreo.
Pendiente de verificar con un visor conectado: envío al compositor, lectura de mandos, roomscale,
colocación del modelo del arma en la mano (`GunOffsetX/Y/Z`, `AimPitchOffset`).

## Notas de la quinta iteración (capturas de diagnóstico del usuario)

- **Cristal:** con la cámara plana, lo que se ve a través del cristal sale ampliado (FOV 90) e idéntico en ambos
  ojos. Con `StereoCaptures=1` se ve al tamaño correcto y con paralaje real. Verificado en modo simulado subiendo
  100 unidades solo la cámara del ojo derecho: la vista cambia solo en ese ojo, luego las capturas sí se
  renderizan por ojo. `StereoCaptures=1` vuelve a estar activo por defecto.
- **Sombreado rayado:** son trazos dibujados en espacio de pantalla (verticales en pantalla, iguales en ambos ojos),
  ligados a las sombras dinámicas. En VR no pueden verse bien. `DynamicShadows=0` (por defecto) las apaga con
  `scale set DynamicShadows False` y `scale set LightEnvironmentShadows False`; `DynamicShadows=1` las mantiene.
- **Recuadro del bloque:** visible solo en el ojo izquierdo (contorno de líneas finas). Prueba en curso:
  `EyeFrameAdvance=1` avanza `GFrameCounter` (0x024E1968) antes del ojo derecho para que cada ojo cuente como un
  fotograma del motor. No se ha podido reproducir el recuadro en modo simulado.
- Herramientas: `DebugTeleport=x y z yaw` coloca al jugador simulado donde se tomó una captura (líneas `SHOT:` del log).

## Notas de la sexta iteración (verificadas en el visor)

- **Líneas de sombra rayadas — resuelto.** Las líneas de Antichamber salen de un detector de bordes por profundidad
  (post-proceso, parámetros `ThinHorizontalThickness` / `ThinVerticalThickness`). Muestrea a una distancia fija en UV,
  pensada para 16:9 a 90°; con el FOV del visor abarcaba 1,7× (horizontal) y ~3× (vertical) más ángulo y las superficies
  inclinadas se llenaban de curvas de nivel. El mod escala esas distancias al FOV del visor (`LineworkFix=1`,
  `LineworkScale`). El plano cercano vuelve a 10 (`NearClip=10`), el valor para el que el juego ajustó el umbral.
- **Espejos/portales en baja resolución — resuelto.** Tras cambiar a la resolución del visor, el mod llama a
  `HazardPlayerController.UpdateScreenMaterialResolutions`, que crea las texturas de captura al tamaño del visor
  (× `TextureScale` 0,75: 2250×2331 en vez de 1020×1058). `ResizeScreenTargets=1`.
- Con `StereoCaptures=1` lo que se ve a través de cristales y portales tiene paralaje correcto en cada ojo.
- **Recuadro de colocación del bloque — resuelto.** `ShowProjectedTile` lo dibuja con `DrawDebugBox`: líneas de
  depuración que viven un solo render, así que solo las veía el ojo izquierdo. El mod copia esas 12 líneas del
  `LineBatchComponent` del motor cuando se crean y las redibuja en el HUD del ojo derecho, proyectadas con su cámara
  y `Canvas.Draw2DLine` (este motor no tiene `Draw3DLine`). `EyeTileFix=1`.
- **Espejos a resolución completa:** `CaptureScale=1.0` (el juego usaba 0,75) sin pérdida de rendimiento apreciable.

## Launcher (`AntichamberVR.exe`, en la carpeta del juego)

Interfaz en inglés por defecto; el botón de arriba a la derecha cambia a español (se recuerda en `[Launcher] Language`).
Ventana de configuración que edita `Binaries\Win32\antichamber_vr.ini` (conserva comentarios y claves desconocidas)
y lanza el juego por Steam (`steam://rungameid/219890`). Botones: **Jugar en VR**, **Jugar en plano** (escribe
`Enabled=0`), **Guardar**, **Valores por defecto**, **Abrir carpeta**. Pestañas:

- **Juego:** modo de pie / sentado (`PlayMode`, `SeatedEyeHeight`), ajuste de altura, roomscale, giro por pasos de
  15/30/45/90° o suave con velocidad, dirección de avance, mano del arma, zona muerta.
- **Imagen:** resolución por ojo (`MaxRenderWidth`), escala de resolución, resolución de espejos/portales
  (`CaptureScale`), escala del mundo, plano cercano, punto de mira, sombras dinámicas, corrección de líneas y grosor,
  espejos con profundidad, texturas a resolución del visor, recuadro en ambos ojos.
- **Arma:** tamaño, desplazamientos e inclinación del apuntado.
- **Mandos:** referencia de controles.
- **Avanzado:** corrección de PhysX, opciones experimentales y de depuración.

Requiere el runtime de escritorio de .NET 8. Compilación:

```
cd launcher
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -p:DebugType=none -o ../build/launcher
```

## Modo sentado y recentrado

- `PlayMode=Seated`: la vista se coloca a `SeatedEyeHeight` metros (calibrado al arrancar).
- **Stick derecho pulsado** (o **F8**): recentra el cuerpo bajo la cabeza y, sentado, recalibra la altura.
