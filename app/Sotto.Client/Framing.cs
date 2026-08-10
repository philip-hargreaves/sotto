namespace Sotto.Client;

/// <summary>Length-prefixed framing: 4-byte little-endian payload length, then the payload.</summary>
public static class Framing
{
    // The pipe imposes no size limit, and the reader allocates whatever the header claims
    public const int MaxFrameBytes = 4 * 1024 * 1024;

    public const int HeaderBytes = 4;

    public static byte[] Encode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length > MaxFrameBytes)
        {
            throw new ArgumentOutOfRangeException(nameof(payload), "frame exceeds MaxFrameBytes");
        }

        var frame = new byte[HeaderBytes + payload.Length];
        BitConverter.TryWriteBytes(frame, (uint)payload.Length);
        payload.CopyTo(frame.AsSpan(HeaderBytes));
        return frame;
    }

    public static int ReadDeclaredLength(ReadOnlySpan<byte> header)
    {
        var length = BitConverter.ToUInt32(header);
        if (length > MaxFrameBytes)
        {
            throw new InvalidDataException("declared frame length exceeds MaxFrameBytes");
        }

        return (int)length;
    }
}
