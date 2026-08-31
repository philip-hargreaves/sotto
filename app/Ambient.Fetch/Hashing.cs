using System.Security.Cryptography;

namespace Ambient.Fetch;

public static class Hashing
{
    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }
}
