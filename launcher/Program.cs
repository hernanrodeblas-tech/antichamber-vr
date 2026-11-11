using System.Diagnostics;
using System.Globalization;

namespace AntichamberVR;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }
}

/// <summary>Launcher window: edits antichamber_vr.ini and starts the game through Steam.
/// English by default; a button switches to Spanish (remembered in the ini, [Launcher] Language).</summary>
public sealed class MainForm : Form
{
    private const string SteamAppId = "219890";

    // One entry per ini key the launcher manages.
    private sealed class Setting
    {
        public string Section, Key, Default, Comment;
        public Action<string> Load;
        public Func<string> Save;
    }

    private readonly List<Setting> _settings = new();
    private readonly ToolTip _tips = new() { AutoPopDelay = 20000, InitialDelay = 300 };
    private readonly string _gameDir;
    private readonly string _iniPath;
    private bool _es;
    private Label _status;
    private ComboBox _playMode;
    private NumericUpDown _seatedHeight;
    private ComboBox _turn;
    private NumericUpDown _smoothSpeed;
    private int _selectedTab;

    /// <summary>Picks the UI text for the current language.</summary>
    private string T(string en, string es) => _es ? es : en;

    public MainForm()
    {
        _gameDir = FindGameDir();
        _iniPath = Path.Combine(_gameDir, "Binaries", "Win32", "antichamber_vr.ini");
        _es = string.Equals(new IniFile(_iniPath).Get("Language"), "es", StringComparison.OrdinalIgnoreCase);

        Text = "Antichamber VR";
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 9.5f);
        MinimumSize = new Size(640, 600);
        Size = new Size(720, 700);

        BuildUi();
        var ini = new IniFile(_iniPath);
        LoadValues(key => ini.Get(key));
        CheckInstall();
    }

    // ------------------------------------------------------------------------------ layout

    private void BuildUi()
    {
        SuspendLayout();
        Controls.Clear();
        _settings.Clear();

        var tabs = new TabControl { Dock = DockStyle.Fill, Padding = new Point(14, 5) };
        tabs.TabPages.Add(BuildGameTab());
        tabs.TabPages.Add(BuildImageTab());
        tabs.TabPages.Add(BuildGunTab());
        tabs.TabPages.Add(BuildControlsTab());
        tabs.TabPages.Add(BuildAdvancedTab());
        tabs.SelectedIndex = Math.Min(_selectedTab, tabs.TabCount - 1);
        tabs.SelectedIndexChanged += (_, _) => _selectedTab = tabs.SelectedIndex;

        var bottom = new FlowLayoutPanel
        {
            Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, AutoSize = true, Padding = new Padding(8), WrapContents = false
        };
        Button Btn(string text, bool bold = false) => new Button
        {
            Text = text, AutoSize = true, Padding = new Padding(bold ? 10 : 6, 4, bold ? 10 : 6, 4), Font = bold ? new Font(Font, FontStyle.Bold) : Font
        };
        var playVr = Btn(T("Play in VR", "Jugar en VR"), true);
        var playFlat = Btn(T("Play flat", "Jugar en plano"));
        var save = Btn(T("Save", "Guardar"));
        var defaults = Btn(T("Defaults", "Valores por defecto"));
        var folder = Btn(T("Open folder", "Abrir carpeta"));
        playVr.Click += (_, _) => Launch(true);
        playFlat.Click += (_, _) => Launch(false);
        save.Click += (_, _) => { SaveIni(null); SetStatus(T("Settings saved.", "Configuración guardada.")); };
        defaults.Click += (_, _) =>
        {
            foreach (var s in _settings) s.Load(s.Default);
            _turn.SelectedIndex = 1;
            SetStatus(T("Defaults loaded (press Save or Play to apply them).", "Valores por defecto cargados (pulsa Guardar o Jugar para aplicarlos)."));
        };
        folder.Click += (_, _) => OpenFolder();
        bottom.Controls.AddRange(new Control[] { playVr, playFlat, save, defaults, folder });
        AcceptButton = playVr;

        // Language switch, top right.
        var lang = new Button { Text = _es ? "English" : "Español", AutoSize = true, Dock = DockStyle.Right };
        lang.Click += (_, _) => SwitchLanguage();
        _tips.SetToolTip(lang, _es ? "Switch the launcher to English" : "Cambiar el launcher a español");
        var top = new Panel { Dock = DockStyle.Top, Height = 38, Padding = new Padding(8, 5, 8, 3) };
        var title = new Label
        {
            Text = T("Antichamber VR settings", "Configuración de Antichamber VR"), Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
            Font = new Font(Font.FontFamily, 11f, FontStyle.Bold)
        };
        top.Controls.Add(title);
        top.Controls.Add(lang);

        _status = new Label { Dock = DockStyle.Bottom, AutoSize = false, Height = 26, Padding = new Padding(10, 4, 10, 0), ForeColor = Color.DimGray };

        Controls.Add(tabs);
        Controls.Add(top);
        Controls.Add(bottom);
        Controls.Add(_status);
        ResumeLayout(true);
    }

    private void SwitchLanguage()
    {
        // Keep whatever the user changed: snapshot the values, rebuild in the other language, restore.
        var values = _settings.GroupBy(s => s.Key).ToDictionary(g => g.Key, g => g.First().Save());
        int turn = _turn.SelectedIndex;
        _es = !_es;
        BuildUi();
        LoadValues(key => values.TryGetValue(key, out var v) ? v : null);
        _turn.SelectedIndex = turn;
        try
        {
            var ini = new IniFile(_iniPath);
            ini.Set("Launcher", "Language", _es ? "es" : "en", "launcher language (en / es)");
            ini.Save();
        }
        catch
        {
            // The choice just won't be remembered.
        }
        CheckInstall();
    }

    // ------------------------------------------------------------------------------ tabs

    private TabPage BuildGameTab()
    {
        var (page, grid) = NewTab(T("Game", "Juego"));

        Header(grid, T("Posture", "Postura"));
        _playMode = Combo(grid, T("Play mode", "Modo de juego"),
            T("Standing: your real height is used.\nSeated: the view is placed at the eye height below; recalibrated when you recenter (right stick click).",
              "De pie: se usa tu altura real.\nSentado: la cabeza se coloca a la altura de ojos indicada; se recalibra al recentrar (stick derecho)."),
            T("Standing", "De pie"), T("Seated", "Sentado"));
        Bind("VR", "PlayMode", "Standing", "Standing (de pie) o Seated (sentado)",
            v => _playMode.SelectedIndex = v != null && (v.Equals("Seated", StringComparison.OrdinalIgnoreCase) || v.Equals("Sentado", StringComparison.OrdinalIgnoreCase)) ? 1 : 0,
            () => _playMode.SelectedIndex == 1 ? "Seated" : "Standing");
        _seatedHeight = Number(grid, T("Seated eye height (m)", "Altura de ojos sentado (m)"),
            T("Height of the view in seated mode.", "Altura a la que se coloca la vista en modo sentado."), 1.0m, 2.2m, 0.01m, 2);
        BindNum("VR", "SeatedEyeHeight", "1.65", "metros, altura de ojos usada en modo sentado", _seatedHeight);
        _playMode.SelectedIndexChanged += (_, _) => _seatedHeight.Enabled = _playMode.SelectedIndex == 1;
        var hOff = Number(grid, T("Height adjustment (m)", "Ajuste de altura (m)"),
            T("Metres added to the head height (both modes).", "Metros que se suman a la altura de la cabeza (en ambos modos)."), -1.0m, 1.0m, 0.01m, 2);
        BindNum("VR", "HeightOffset", "0.0", "metros que se suman a tu altura", hOff);
        var room = Check(grid, "Roomscale", T("Walking physically moves the character (with collision).", "Caminar físicamente mueve al personaje (con colisión)."));
        BindBool("VR", "Roomscale", "1", "1 = caminar fisicamente mueve al personaje", room);

        Header(grid, T("Turning", "Giro"));
        _turn = Combo(grid, T("Turn type", "Tipo de giro"), T("Stick of the gun hand (right by default).", "Stick de la mano del arma (derecha por defecto)."),
            T("Snap 15°", "Por pasos de 15°"), T("Snap 30°", "Por pasos de 30°"), T("Snap 45°", "Por pasos de 45°"), T("Snap 90°", "Por pasos de 90°"),
            T("Smooth", "Suave"));
        Bind("Controles", "SnapTurn", "1", "1 = giro por pasos, 0 = giro suave", v => { }, () => _turn.SelectedIndex == 4 ? "0" : "1");
        Bind("Controles", "SnapTurnDegrees", "30", "grados por paso", v => _turn.Tag = ParseD(v, 30),
            () => _turn.SelectedIndex switch { 0 => "15", 1 => "30", 2 => "45", 3 => "90", _ => IniFile.F(_turn.Tag is double d ? d : 30) });
        _smoothSpeed = Number(grid, T("Smooth turn speed (°/s)", "Velocidad de giro suave (°/s)"),
            T("Degrees per second with the stick fully pushed.", "Grados por segundo con el stick a tope."), 30, 360, 10, 0);
        BindNum("Controles", "SmoothTurnSpeed", "120", "grados por segundo (giro suave)", _smoothSpeed);
        _turn.SelectedIndexChanged += (_, _) => _smoothSpeed.Enabled = _turn.SelectedIndex == 4;

        Header(grid, T("Movement", "Movimiento"));
        var dir = Combo(grid, T("Movement direction", "Dirección de avance"),
            T("Where the movement stick moves you.", "Hacia dónde avanza el stick de movimiento."),
            T("Where you look", "Hacia donde miras"), T("Where the hand points", "Hacia donde apunta la mano"));
        BindCombo("Controles", "MoveRelativeToHand", "0", "0 = hacia donde miras, 1 = hacia la mano", dir);
        var hand = Combo(grid, T("Gun hand", "Mano del arma"), T("The other hand is used for moving.", "La otra mano se usa para moverse."),
            T("Right", "Derecha"), T("Left", "Izquierda"));
        BindCombo("Controles", "LeftHanded", "0", "1 = arma en la mano izquierda", hand);
        var dz = Number(grid, T("Stick deadzone", "Zona muerta de los sticks"),
            T("Stick travel that is ignored (0 to 0.6).", "Recorrido del stick que se ignora (0 a 0,6)."), 0, 0.6m, 0.05m, 2);
        BindNum("Controles", "StickDeadzone", "0.2", null, dz);

        return page;
    }

    private TabPage BuildImageTab()
    {
        var (page, grid) = NewTab(T("Image", "Imagen"));

        Header(grid, T("Resolution", "Resolución"));
        var maxW = Combo(grid, T("Per-eye resolution (max width)", "Resolución por ojo (anchura máxima)"),
            T("Caps the width of each eye in pixels. SteamVR suggests a resolution for your headset and supersampling;\nlower it if you can't reach the headset refresh rate.",
              "Limita la anchura de cada ojo en píxeles. SteamVR recomienda una resolución según el visor y su supermuestreo;\nbájala si no llegas a la frecuencia del visor."),
            T("No limit (SteamVR's choice)", "Sin límite (la que pida SteamVR)"), "4000 px", "3500 px", "3000 px", "2500 px", "2000 px", "1600 px");
        var widths = new[] { 0, 4000, 3500, 3000, 2500, 2000, 1600 };
        Bind("VR", "MaxRenderWidth", "3000", "limite de anchura por ojo (0 = sin limite)",
            v =>
            {
                int w = (int)ParseD(v, 3000);
                int best = Array.IndexOf(widths, w);
                if (best < 0)
                {
                    maxW.Items.Add(w + " px");
                    best = maxW.Items.Count - 1;
                    maxW.Tag = w;
                }
                maxW.SelectedIndex = best;
            },
            () => maxW.SelectedIndex < widths.Length ? widths[Math.Max(0, maxW.SelectedIndex)].ToString() : (maxW.Tag?.ToString() ?? "3000"));
        var rs = Number(grid, T("Resolution scale", "Escala de resolución"),
            T("Multiplier on SteamVR's recommended resolution (applied before the width cap).",
              "Multiplicador sobre la resolución recomendada por SteamVR (antes del límite de anchura)."), 0.5m, 2.0m, 0.05m, 2);
        BindNum("VR", "ResolutionScale", "1.0", "multiplicador sobre la resolucion de SteamVR", rs);
        var cap = Combo(grid, T("Mirror and portal resolution", "Resolución de espejos y portales"),
            T("Size of the mirror/portal captures relative to the eye.\n75% is what the original game uses.",
              "Tamaño de las capturas de espejos/portales respecto al ojo.\n75% es lo que usa el juego original."),
            "50%", "75% (original)", "100%");
        var caps = new[] { 0.5, 0.75, 1.0 };
        Bind("VR", "CaptureScale", "1.0", "resolucion de espejos/portales respecto al ojo",
            v =>
            {
                double c = ParseD(v, 1.0);
                int b = 2;
                for (int i = 0; i < caps.Length; i++)
                    if (Math.Abs(caps[i] - c) < 0.01) b = i;
                cap.SelectedIndex = b;
            },
            () => IniFile.F(caps[Math.Max(0, cap.SelectedIndex)]));

        Header(grid, T("World", "Mundo"));
        var ws = Number(grid, T("World scale (uu/m)", "Escala del mundo (uu/m)"),
            T("Unreal units per metre. Higher = the world looks bigger and you smaller.", "Unidades de Unreal por metro. Más alto = el mundo se ve más grande y tú más pequeño."),
            30, 80, 1, 0);
        BindNum("VR", "WorldScale", "50", "unidades Unreal por metro", ws);
        var near = Number(grid, T("Near clipping plane", "Plano de recorte cercano"),
            T("10 is the original value; the game's line detector is tuned for it.", "10 es el valor original; el detector de líneas del juego está ajustado para él."),
            1, 20, 1, 0);
        BindNum("VR", "NearClip", "10", "plano de recorte cercano (10 = original)", near);

        Header(grid, T("Effects", "Efectos"));
        var xh = Check(grid, T("3D aim dot", "Punto de apuntado 3D"), T("A dot where the gun points, at the right depth.", "Un punto donde señala el arma, a la profundidad correcta."));
        BindBool("VR", "Crosshair", "1", "1 = punto de apuntado 3D", xh);
        var ds = Check(grid, T("Dynamic shadows", "Sombras dinámicas"), T("The original game's dynamic shadows.", "Las sombras dinámicas del juego original."));
        BindBool("VR", "DynamicShadows", "1", "0 = apaga las sombras dinamicas", ds);
        var mb = Check(grid, T("Camera motion blur", "Desenfoque de movimiento"),
            T("The engine's motion blur. Off is recommended in VR.", "El desenfoque de movimiento del motor. Se recomienda desactivarlo en VR."));
        BindBool("VR", "MotionBlur", "0", "0 = sin desenfoque de movimiento de camara", mb);
        var lw = Check(grid, T("Line and shading fix", "Corrección de líneas y sombras"),
            T("Fits the game's line detector to the headset FOV (prevents the striped shading).", "Ajusta el detector de líneas del juego al FOV del visor (evita el rayado)."));
        BindBool("VR", "LineworkFix", "1", "1 = ajusta el detector de lineas al FOV del visor", lw);
        var lws = Number(grid, T("Line thickness", "Grosor de líneas"),
            T("Multiplier on that fix: lower = fewer false lines, higher = thicker outlines.", "Multiplicador de la corrección: menos = menos líneas falsas, más = contornos más gruesos."),
            0.3m, 2.0m, 0.05m, 2);
        BindNum("VR", "LineworkScale", "1.0", "multiplicador del ajuste de lineas", lws);
        lw.CheckedChanged += (_, _) => lws.Enabled = lw.Checked;
        var sc = Check(grid, T("Mirrors and portals with depth", "Espejos y portales con profundidad"),
            T("Each eye gets its own view through glass and portals.", "Cada ojo ve su propia imagen a través de cristales y portales."));
        BindBool("VR", "StereoCaptures", "1", "1 = capturas colocadas por ojo", sc);
        var rt = Check(grid, T("Mirrors at headset resolution", "Espejos a la resolución del visor"),
            T("Mirror/portal textures follow the headset resolution.", "Las texturas de espejos/portales se adaptan a la resolución del visor."));
        BindBool("VR", "ResizeScreenTargets", "1", "1 = texturas de espejos a la resolucion del visor", rt);
        var tile = Check(grid, T("Block outline in both eyes", "Recuadro del bloque en ambos ojos"),
            T("Redraws the block placement outline in the right eye.", "Redibuja el recuadro de colocación del bloque en el ojo derecho."));
        BindBool("VR", "EyeTileFix", "1", "1 = recuadro del bloque en ambos ojos", tile);

        return page;
    }

    private TabPage BuildGunTab()
    {
        var (page, grid) = NewTab(T("Gun", "Arma"));
        Header(grid, T("Gun model", "Modelo del arma"));
        var gs = Number(grid, T("Gun size", "Tamaño del arma"), T("1 = original size.", "1 = tamaño original."), 0.3m, 2.0m, 0.05m, 2);
        BindNum("Controles", "GunScale", "1.0", "tamano del modelo del arma", gs);
        var gx = Number(grid, T("Forward offset (uu)", "Desplazamiento adelante (uu)"),
            T("Moves the gun model relative to the hand (50 uu = 1 m).", "Mueve el modelo del arma respecto a la mano (50 uu = 1 m)."), -50, 50, 1, 1);
        BindNum("Controles", "GunOffsetX", "0", null, gx);
        var gy = Number(grid, T("Sideways offset (uu)", "Desplazamiento lateral (uu)"), null, -50, 50, 1, 1);
        BindNum("Controles", "GunOffsetY", "0", null, gy);
        var gz = Number(grid, T("Vertical offset (uu)", "Desplazamiento vertical (uu)"), null, -50, 50, 1, 1);
        BindNum("Controles", "GunOffsetZ", "0", null, gz);
        Header(grid, T("Aiming", "Apuntado"));
        var ap = Number(grid, T("Aim pitch (°)", "Inclinación del apuntado (°)"),
            T("Positive tilts the aim ray down relative to the controller.", "Positivo inclina el rayo de apuntado hacia abajo respecto al mando."), -45, 45, 1, 0);
        BindNum("Controles", "AimPitchOffset", "0", "grados; positivo inclina el apuntado hacia abajo", ap);
        return page;
    }

    private TabPage BuildControlsTab()
    {
        var page = new TabPage(T("Controls", "Mandos")) { Padding = new Padding(16), AutoScroll = true };
        var text = new Label
        {
            Dock = DockStyle.Fill, AutoSize = false, Font = new Font("Consolas", 10f),
            Text = _es
                ? "Controles (Steam Frame, Quest Touch y mandos con entrada clásica de SteamVR)\n\n" +
                  "Stick de la mano libre ........ moverse\n" +
                  "Stick de la mano del arma ..... girar (por pasos o suave)\n" +
                  "Gatillo del arma .............. disparo primario / pulsar carteles\n" +
                  "Agarre (grip) del arma ........ disparo secundario\n" +
                  "Gatillo de la otra mano o B ... tercer modo del arma\n" +
                  "A o X ......................... saltar\n" +
                  "Y ............................. volver a la antecámara (mantener para salir)\n" +
                  "Stick derecho pulsado ......... recentrar (y recalibrar la altura sentado)\n" +
                  "Ambos sticks pulsados ......... captura de diagnóstico (también F9)\n\n" +
                  "Teclado: F8 recentrar, F9 captura.\n\n" +
                  "Con 'Mano del arma: Izquierda' se intercambian los papeles de cada mano."
                : "Controls (Steam Frame, Quest Touch and controllers with SteamVR legacy input)\n\n" +
                  "Free-hand stick ............... move\n" +
                  "Gun-hand stick ................ turn (snap or smooth)\n" +
                  "Gun trigger ................... primary fire / press signs\n" +
                  "Gun grip ...................... secondary fire\n" +
                  "Other trigger or B ............ third gun mode\n" +
                  "A or X ........................ jump\n" +
                  "Y ............................. back to the antechamber (hold to quit)\n" +
                  "Right stick click ............. recenter (and recalibrate height when seated)\n" +
                  "Both sticks clicked ........... diagnostic snapshot (also F9)\n\n" +
                  "Keyboard: F8 recenter, F9 snapshot.\n\n" +
                  "With 'Gun hand: Left' the roles of both hands are swapped."
        };
        page.Controls.Add(text);
        return page;
    }

    private TabPage BuildAdvancedTab()
    {
        var (page, grid) = NewTab(T("Advanced", "Avanzado"));
        Header(grid, T("Compatibility", "Compatibilidad"));
        var px = Check(grid, T("PhysX fix", "Corrección de PhysX"),
            T("Forces the game's own PhysX 2.8.4.4 (prevents the crash on load with NVIDIA's PhysX installed).",
              "Fuerza el PhysX 2.8.4.4 del juego (evita el cierre al cargar con el PhysX de NVIDIA instalado)."));
        BindBool("Avanzado", "FixPhysX", "1", "fuerza el PhysX 2.8.4.4 del juego", px);
        Header(grid, T("Performance", "Rendimiento"));
        var pf = Check(grid, T("Overlap CPU and GPU", "CPU y GPU en paralelo"),
            T("Submits each frame one frame later so the CPU never waits for the GPU. More fps on slow systems (Steam Frame), one frame more latency.",
              "Envía cada fotograma uno más tarde para que la CPU no espere a la GPU. Más fps en equipos lentos (Steam Frame), un fotograma más de latencia."));
        BindBool("Avanzado", "PipelineFrames", "0", "1 = CPU y GPU trabajan a la vez (mas fps, un fotograma mas de latencia)", pf);
        var br = Combo(grid, T("Image hand-off to SteamVR", "Paso de imagen a SteamVR"),
            T("Automatic tries each method in turn. Only change it if the log reports a problem.",
              "Automático prueba cada método por orden. Cámbialo solo si el registro indica un problema."),
            new[] { T("Automatic", "Automático"), T("Shared D3D11 texture", "Textura D3D11 compartida"), T("System-memory copy (slow)", "Copia por memoria (lenta)") });
        Bind("Avanzado", "D3DBridge", "0", "0 = prueba los metodos de paso de imagen en orden; 2 = copia por memoria",
            v => br.SelectedIndex = v == "2" ? 2 : v == "1" ? 1 : 0, () => br.SelectedIndex.ToString());
        Header(grid, "Experimental");
        var occ = Check(grid, T("Right eye without occlusion", "Ojo derecho sin oclusión"),
            T("The right eye ignores occlusion queries inherited from the left eye.", "El ojo derecho ignora las consultas de oclusión heredadas del izquierdo."));
        BindBool("VR", "EyeOcclusionFix", "0", "experimental", occ);
        var fa = Check(grid, T("Own engine frame per eye", "Fotograma propio por ojo"),
            T("Each eye counts as an engine frame.", "Cada ojo cuenta como un fotograma del motor."));
        BindBool("VR", "EyeFrameAdvance", "0", "experimental", fa);
        Header(grid, T("Debugging", "Depuración"));
        var fake = Check(grid, T("Simulated headset (no headset)", "Visor simulado (sin visor)"),
            T("Runs the stereo rendering without a headset, for testing.", "Ejecuta el render estéreo sin visor, para pruebas."));
        BindBool("Avanzado", "FakeHMD", "0", "modo de pruebas sin visor", fake);
        var dbg = Check(grid, T("Verbose log", "Registro detallado"),
            T("Writes much more information to antichamber_vr.log.", "Escribe mucha más información en antichamber_vr.log."));
        BindBool("Avanzado", "DebugLog", "0", null, dbg);
        var df = Number(grid, T("Save eye images at frame", "Guardar ojos en el fotograma"),
            T("0 = off. Saves vr_eye0/1.jpg at that stereo frame.", "0 = desactivado. Guarda vr_eye0/1.jpg en ese fotograma estéreo."), 0, 100000, 100, 0);
        BindNum("Avanzado", "DebugDumpFrame", "0", null, df);
        return page;
    }

    // ------------------------------------------------------------------------------ helpers

    private (TabPage, TableLayoutPanel) NewTab(string title)
    {
        var page = new TabPage(title) { Padding = new Padding(12), AutoScroll = true };
        var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
        page.Controls.Add(grid);
        return (page, grid);
    }

    private void Header(TableLayoutPanel g, string text)
    {
        var l = new Label { Text = text, AutoSize = true, Font = new Font(Font, FontStyle.Bold), Margin = new Padding(0, g.RowCount == 0 ? 2 : 14, 0, 4) };
        g.Controls.Add(l, 0, g.RowCount);
        g.SetColumnSpan(l, 2);
        g.RowCount++;
    }

    private void Row(TableLayoutPanel g, string label, Control c, string tip)
    {
        var l = new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(12, 6, 6, 6) };
        c.Anchor = AnchorStyles.Left;
        c.Margin = new Padding(3, 4, 3, 4);
        g.Controls.Add(l, 0, g.RowCount);
        g.Controls.Add(c, 1, g.RowCount);
        g.RowCount++;
        if (tip != null)
        {
            _tips.SetToolTip(l, tip);
            _tips.SetToolTip(c, tip);
        }
    }

    private ComboBox Combo(TableLayoutPanel g, string label, string tip, params string[] items)
    {
        var c = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 250 };
        c.Items.AddRange(items);
        Row(g, label, c, tip);
        return c;
    }

    private NumericUpDown Number(TableLayoutPanel g, string label, string tip, decimal min, decimal max, decimal step, int decimals)
    {
        var n = new NumericUpDown { Minimum = min, Maximum = max, Increment = step, DecimalPlaces = decimals, Width = 100 };
        Row(g, label, n, tip);
        return n;
    }

    private CheckBox Check(TableLayoutPanel g, string label, string tip)
    {
        var c = new CheckBox { AutoSize = true, Text = "" };
        Row(g, label, c, tip);
        return c;
    }

    private void Bind(string section, string key, string def, string comment, Action<string> load, Func<string> save) =>
        _settings.Add(new Setting { Section = section, Key = key, Default = def, Comment = comment, Load = load, Save = save });

    private void BindNum(string section, string key, string def, string comment, NumericUpDown n) =>
        Bind(section, key, def, comment,
            v => n.Value = Math.Min(n.Maximum, Math.Max(n.Minimum, (decimal)ParseD(v, ParseD(def, 0)))),
            () => IniFile.F((double)n.Value));

    private void BindBool(string section, string key, string def, string comment, CheckBox c) =>
        Bind(section, key, def, comment, v => c.Checked = ParseBool(v ?? def), () => c.Checked ? "1" : "0");

    private void BindCombo(string section, string key, string def, string comment, ComboBox c) =>
        Bind(section, key, def, comment, v => c.SelectedIndex = Math.Min(c.Items.Count - 1, Math.Max(0, (int)ParseD(v, ParseD(def, 0)))),
            () => Math.Max(0, c.SelectedIndex).ToString());

    private static double ParseD(string v, double fallback) =>
        double.TryParse(v?.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : fallback;

    private static bool ParseBool(string v) =>
        v != null && (v == "1" || v.Equals("true", StringComparison.OrdinalIgnoreCase) || v.Equals("yes", StringComparison.OrdinalIgnoreCase) ||
                      v.Equals("on", StringComparison.OrdinalIgnoreCase));

    // ------------------------------------------------------------------------------ ini / launch

    private void LoadValues(Func<string, string> get)
    {
        foreach (var s in _settings) s.Load(get(s.Key) ?? s.Default);
        // Turning: combine SnapTurn + SnapTurnDegrees.
        bool snap = ParseBool(get("SnapTurn") ?? "1");
        double deg = ParseD(get("SnapTurnDegrees"), 30);
        _turn.SelectedIndex = !snap ? 4 : deg <= 22 ? 0 : deg <= 37 ? 1 : deg <= 67 ? 2 : 3;
        _seatedHeight.Enabled = _playMode.SelectedIndex == 1;
        _smoothSpeed.Enabled = _turn.SelectedIndex == 4;
    }

    private void SaveIni(bool? vrEnabled)
    {
        var ini = new IniFile(_iniPath);
        foreach (var s in _settings) ini.Set(s.Section, s.Key, s.Save(), s.Comment);
        if (vrEnabled.HasValue) ini.Set("VR", "Enabled", vrEnabled.Value ? "1" : "0", "0 = jugar en plano");
        ini.Set("Launcher", "Language", _es ? "es" : "en", "launcher language (en / es)");
        ini.Save();
    }

    private void Launch(bool vr)
    {
        try
        {
            SaveIni(vr);
            Process.Start(new ProcessStartInfo("steam://rungameid/" + SteamAppId) { UseShellExecute = true });
            SetStatus(vr ? T("Launching Antichamber in VR (with the headset connected to SteamVR)...", "Lanzando Antichamber en VR (con el visor conectado a SteamVR)...")
                         : T("Launching Antichamber flat...", "Lanzando Antichamber en plano..."));
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, T("Could not launch the game:\n", "No se pudo lanzar el juego:\n") + ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OpenFolder()
    {
        var dir = Path.GetDirectoryName(_iniPath);
        if (Directory.Exists(dir)) Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });
    }

    private void CheckInstall()
    {
        var bin = Path.GetDirectoryName(_iniPath);
        bool mod = File.Exists(Path.Combine(bin, "d3d9.dll")), openvr = File.Exists(Path.Combine(bin, "openvr_api.dll"));
        bool game = File.Exists(Path.Combine(bin, "UDK.exe"));
        if (!game) SetStatus(T("The game was not found next to the launcher (", "No se encuentra el juego junto al launcher (") + _gameDir + ").");
        else if (!mod || !openvr) SetStatus(T("The mod is missing from Binaries\\Win32 (d3d9.dll / openvr_api.dll).", "Falta el mod en Binaries\\Win32 (d3d9.dll / openvr_api.dll)."));
        else SetStatus(T("Mod installed and ready.", "Mod instalado y listo."));
    }

    private void SetStatus(string s) => _status.Text = s;

    private static string FindGameDir()
    {
        // The launcher lives in the game folder (or VRMod\launcher during development).
        var dir = AppContext.BaseDirectory;
        for (int i = 0; i < 5 && dir != null; i++)
        {
            if (File.Exists(Path.Combine(dir, "Binaries", "Win32", "UDK.exe"))) return dir;
            dir = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar));
        }
        return AppContext.BaseDirectory;
    }
}
