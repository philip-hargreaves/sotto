namespace Sotto.App.Tests;

/// <summary>Writes a temp 16 kHz mono PCM16 silence wav.</summary>
internal static class SessionContractWav
{
    public static string Write(int seconds)
    {
        var frames = seconds * 16000;
        var bytes = new byte[44 + frames * 2];
        void Tag(int offset, string tag) =>
            System.Text.Encoding.ASCII.GetBytes(tag).CopyTo(bytes, offset);
        void U32(int offset, uint value) => BitConverter.GetBytes(value).CopyTo(bytes, offset);
        void U16(int offset, ushort value) => BitConverter.GetBytes(value).CopyTo(bytes, offset);
        Tag(0, "RIFF");
        U32(4, (uint)(bytes.Length - 8));
        Tag(8, "WAVE");
        Tag(12, "fmt ");
        U32(16, 16);
        U16(20, 1);
        U16(22, 1);
        U32(24, 16000);
        U32(28, 32000);
        U16(32, 2);
        U16(34, 16);
        Tag(36, "data");
        U32(40, (uint)(frames * 2));

        var path = Path.Combine(Path.GetTempPath(), $"sotto-demo-{Guid.NewGuid():N}.wav");
        File.WriteAllBytes(path, bytes);
        return path;
    }
}
