using System.Buffers.Binary;

namespace Ambient.Client;

/// <summary>Length-prefixed framing: 4-byte little-endian payload length, then the payload.</summary>
public static class Framing
{
    // Pipe imposes no size limit; capped to prevent unbounded allocation
    public const int MaxFrameBytes = 4 * 1024 * 1024;

    public const int HeaderBytes = 4;

    public static byte[] Encode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length > MaxFrameBytes)
        {
            throw new ArgumentOutOfRangeException(nameof(payload), "frame exceeds MaxFrameBytes");
        }

        var frame = new byte[HeaderBytes + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame, (uint)payload.Length);
        payload.CopyTo(frame.AsSpan(HeaderBytes));
        return frame;
    }

    public static int ReadDeclaredLength(ReadOnlySpan<byte> header)
    {
        var length = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (length > MaxFrameBytes)
        {
            throw new InvalidDataException("declared frame length exceeds MaxFrameBytes");
        }

        return (int)length;
    }
}
