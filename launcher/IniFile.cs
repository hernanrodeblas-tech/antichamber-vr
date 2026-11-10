using System.Globalization;
using System.Text;

namespace AntichamberVR;

/// <summary>
/// Minimal INI editor for antichamber_vr.ini that keeps comments, ordering and any key it
/// does not know about. Values are updated in place; missing keys are added to their section.
/// </summary>
public sealed class IniFile
{
    private readonly List<string> _lines = new();
    public string Path { get; }

    public IniFile(string path)
    {
        Path = path;
        if (File.Exists(path))
            _lines.AddRange(File.ReadAllLines(path, Encoding.ASCII));
    }

    private static bool IsSection(string line, out string name)
    {
        var t = line.Trim();
        if (t.StartsWith("[") && t.EndsWith("]"))
        {
            name = t[1..^1].Trim();
            return true;
        }
        name = null;
        return false;
    }

    private int FindKey(string key)
    {
        for (int i = 0; i < _lines.Count; i++)
        {
            var t = _lines[i].TrimStart();
            if (t.StartsWith(";") || t.StartsWith("#")) continue;
            int eq = t.IndexOf('=');
            if (eq <= 0) continue;
            if (string.Equals(t[..eq].Trim(), key, StringComparison.OrdinalIgnoreCase)) return i;
        }
        return -1;
    }

    public string Get(string key)
    {
        int i = FindKey(key);
        if (i < 0) return null;
        var t = _lines[i];
        var v = t[(t.IndexOf('=') + 1)..];
        int sc = v.IndexOf(';');
        if (sc >= 0) v = v[..sc];
        return v.Trim();
    }

    public void Set(string section, string key, string value, string comment)
    {
        int i = FindKey(key);
        if (i >= 0)
        {
            var old = _lines[i];
            var rest = old[(old.IndexOf('=') + 1)..];
            int sc = rest.IndexOf(';');
            string existingComment = sc >= 0 ? rest[sc..] : null;
            _lines[i] = Format(key, value, existingComment ?? (comment != null ? "; " + comment : null));
            return;
        }
        // Insert at the end of the section (create it if needed).
        int sectionStart = -1, insertAt = -1;
        for (int j = 0; j < _lines.Count; j++)
        {
            if (IsSection(_lines[j], out var name))
            {
                if (sectionStart >= 0 && insertAt < 0) insertAt = j;
                if (string.Equals(name, section, StringComparison.OrdinalIgnoreCase)) sectionStart = j;
            }
        }
        var line = Format(key, value, comment != null ? "; " + comment : null);
        if (sectionStart < 0)
        {
            if (_lines.Count > 0 && _lines[^1].Trim().Length > 0) _lines.Add("");
            _lines.Add("[" + section + "]");
            _lines.Add(line);
            return;
        }
        if (insertAt < 0) insertAt = _lines.Count;
        // Skip back over blank lines so the key stays inside its block.
        while (insertAt - 1 > sectionStart && _lines[insertAt - 1].Trim().Length == 0) insertAt--;
        _lines.Insert(insertAt, line);
    }

    private static string Format(string key, string value, string comment)
    {
        var kv = key + "=" + value;
        if (string.IsNullOrEmpty(comment)) return kv;
        return kv.PadRight(23) + comment;
    }

    public void Save()
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
        File.WriteAllLines(Path, _lines, Encoding.ASCII);
    }

    public static string F(double v) => v.ToString("0.###", CultureInfo.InvariantCulture);
}
