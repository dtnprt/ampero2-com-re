using System;
using System.Buffers.Binary;

namespace Ampero.Protocol;

/// <summary>Thrown when an LZO1X stream is malformed or truncated.</summary>
public sealed class LzoException : Exception
{
    public LzoException(string message) : base(message) { }
}

/// <summary>
/// LZO1X decompressor and LZO1X-1 compressor, bit-exact with miniLZO 2.10 as linked by
/// the Ampero II editor (HTUSBTools.dll). The compressor reproduces the x64 build's
/// 64-bit match extension, so its output is byte-identical to the editor's.
/// </summary>
public static class Lzo1x
{
    public static byte[] Decompress(ReadOnlySpan<byte> src) => Decompress(src, out _);

    /// <summary>Decompresses a raw LZO1X stream up to its end marker (11 00 00).</summary>
    public static byte[] Decompress(ReadOnlySpan<byte> src, out int consumed)
    {
        var o = new Output(Math.Max(64, src.Length * 4));
        int ip = 0;
        int t;
        State state = State.Loop;

        try
        {
            if (src[0] > 17)
            {
                t = src[0] - 17;
                ip = 1;
                if (t < 4)
                {
                    o.Literals(src, ref ip, t);
                    t = src[ip++];
                    state = State.Match;
                    goto dispatch;
                }
                o.Literals(src, ref ip, t);
                state = State.FirstLiteralRun;
            }
            t = 0;

        dispatch:
            while (true)
            {
                switch (state)
                {
                    case State.Loop:
                        t = src[ip++];
                        if (t >= 16) { state = State.Match; break; }
                        if (t == 0) t = ExtendedLength(src, ref ip, 15);
                        o.Literals(src, ref ip, t + 3);
                        state = State.FirstLiteralRun;
                        break;

                    case State.FirstLiteralRun:
                        t = src[ip++];
                        if (t >= 16) { state = State.Match; break; }
                        o.Copy(o.Length - 0x801 - (t >> 2) - (src[ip++] << 2), 3);
                        state = State.MatchDone;
                        break;

                    case State.Match:
                        if (t >= 64)
                        {
                            int pos = o.Length - 1 - ((t >> 2) & 7) - (src[ip++] << 3);
                            o.Copy(pos, (t >> 5) + 1);
                        }
                        else if (t >= 32)
                        {
                            t &= 31;
                            if (t == 0) t = ExtendedLength(src, ref ip, 31);
                            int pos = o.Length - 1 - ((src[ip] | (src[ip + 1] << 8)) >> 2);
                            ip += 2;
                            o.Copy(pos, t + 2);
                        }
                        else if (t >= 16)
                        {
                            int pos = o.Length - ((t & 8) << 11);
                            t &= 7;
                            if (t == 0) t = ExtendedLength(src, ref ip, 7);
                            pos -= (src[ip] | (src[ip + 1] << 8)) >> 2;
                            ip += 2;
                            if (pos == o.Length)
                            {
                                consumed = ip;
                                return o.ToArray();   // end of stream
                            }
                            o.Copy(pos - 0x4000, t + 2);
                        }
                        else
                        {
                            int pos = o.Length - 1 - (t >> 2) - (src[ip++] << 2);
                            o.Copy(pos, 2);
                        }
                        state = State.MatchDone;
                        break;

                    case State.MatchDone:
                        t = src[ip - 2] & 3;
                        if (t == 0) { state = State.Loop; break; }
                        o.Literals(src, ref ip, t);
                        t = src[ip++];
                        state = State.Match;
                        break;
                }
            }
        }
        catch (IndexOutOfRangeException)
        {
            throw new LzoException($"input overrun at {ip} (output {o.Length} bytes)");
        }
    }

    enum State { Loop, FirstLiteralRun, Match, MatchDone }

    static int ExtendedLength(ReadOnlySpan<byte> src, ref int ip, int baseLen)
    {
        int t = 0;
        while (src[ip] == 0) { t += 255; ip++; }
        return t + baseLen + src[ip++];
    }

    sealed class Output
    {
        byte[] _buf;
        public int Length;
        public Output(int capacity) => _buf = new byte[capacity];

        void Ensure(int extra)
        {
            if (Length + extra <= _buf.Length) return;
            Array.Resize(ref _buf, Math.Max(_buf.Length * 2, Length + extra));
        }

        public void Literals(ReadOnlySpan<byte> src, ref int ip, int count)
        {
            if (ip + count > src.Length) throw new LzoException($"input overrun at {ip}");
            Ensure(count);
            src.Slice(ip, count).CopyTo(_buf.AsSpan(Length));
            Length += count;
            ip += count;
        }

        public void Copy(int pos, int count)
        {
            if (pos < 0) throw new LzoException($"lookbehind overrun at output {Length}");
            Ensure(count);
            for (int i = 0; i < count; i++) _buf[Length + i] = _buf[pos + i];   // overlapping copy
            Length += count;
        }

        public byte[] ToArray() => _buf.AsSpan(0, Length).ToArray();
    }

    // ------------------------------------------------------------------ compressor

    /// <summary>LZO1X-1 compression (raw stream including end marker).</summary>
    public static byte[] Compress(ReadOnlySpan<byte> src)
    {
        var op = new Writer(src.Length + src.Length / 16 + 64 + 3);
        int pos = 0, left = src.Length, t = 0;
        while (left > 20)
        {
            int ll = Math.Min(left, 49152);
            t = DoCompress(src, pos, ll, op, t);
            pos += ll;
            left -= ll;
        }
        t += left;
        if (t > 0)
        {
            int ii = src.Length - t;
            LiteralHeader(op, t, op.Length == 0);
            op.Write(src.Slice(ii, t));
        }
        op.Add(0x11); op.Add(0); op.Add(0);
        return op.ToArray();
    }

    static void LiteralHeader(Writer op, int t, bool first)
    {
        if (first && t <= 238) op.Add((byte)(17 + t));
        else if (t <= 3) op.OrAt(op.Length - 2, (byte)t);
        else if (t <= 18) op.Add((byte)(t - 3));
        else
        {
            int tt = t - 18;
            op.Add(0);
            while (tt > 255) { tt -= 255; op.Add(0); }
            op.Add((byte)tt);
        }
    }

    static ulong Read64(ReadOnlySpan<byte> src, int at)
    {
        if (at + 8 <= src.Length) return BinaryPrimitives.ReadUInt64LittleEndian(src.Slice(at));
        ulong v = 0;
        for (int i = 0; i < 8 && at + i < src.Length; i++) v |= (ulong)src[at + i] << (8 * i);
        return v;
    }

    static int DoCompress(ReadOnlySpan<byte> src, int b, int inLen, Writer op, int ti)
    {
        const int DBits = 14;
        int inEnd = b + inLen;
        int ipEnd = b + inLen - 20;
        var dict = new ushort[1 << DBits];
        int ip = b;
        int ii = ip;
        ip += ti < 4 ? 4 - ti : 0;

        while (true)
        {
            ip += 1 + ((ip - ii) >> 5);                       // literal:
            while (true)                                      // next:
            {
                if (ip >= ipEnd) return inEnd - (ii - ti);
                uint dv = BinaryPrimitives.ReadUInt32LittleEndian(src.Slice(ip));
                int dindex = (int)(((0x1824429du * dv) >> (32 - DBits)) & ((1 << DBits) - 1));
                int mPos = b + dict[dindex];
                dict[dindex] = (ushort)(ip - b);
                if (dv != BinaryPrimitives.ReadUInt32LittleEndian(src.Slice(mPos))) break;

                ii -= ti;
                ti = 0;
                int t = ip - ii;
                if (t != 0)
                {
                    LiteralHeader(op, t, false);
                    op.Write(src.Slice(ii, t));
                }

                int mLen = 4;
                ulong v = Read64(src, ip + mLen) ^ Read64(src, mPos + mLen);
                if (v == 0)
                {
                    while (true)
                    {
                        mLen += 8;
                        v = Read64(src, ip + mLen) ^ Read64(src, mPos + mLen);
                        if (ip + mLen >= ipEnd) goto lenDone;
                        if (v != 0) break;
                    }
                }
                mLen += System.Numerics.BitOperations.TrailingZeroCount(v) / 8;
            lenDone:
                int mOff = ip - mPos;
                ip += mLen;
                ii = ip;
                if (mLen <= 8 && mOff <= 0x800)
                {
                    mOff -= 1;
                    op.Add((byte)(((mLen - 1) << 5) | ((mOff & 7) << 2)));
                    op.Add((byte)(mOff >> 3));
                }
                else if (mOff <= 0x4000)
                {
                    mOff -= 1;
                    if (mLen <= 33) op.Add((byte)(32 | (mLen - 2)));
                    else
                    {
                        mLen -= 33;
                        op.Add(32);
                        while (mLen > 255) { mLen -= 255; op.Add(0); }
                        op.Add((byte)mLen);
                    }
                    op.Add((byte)(mOff << 2));
                    op.Add((byte)(mOff >> 6));
                }
                else
                {
                    mOff -= 0x4000;
                    if (mLen <= 9) op.Add((byte)(16 | ((mOff >> 11) & 8) | (mLen - 2)));
                    else
                    {
                        mLen -= 9;
                        op.Add((byte)(16 | ((mOff >> 11) & 8)));
                        while (mLen > 255) { mLen -= 255; op.Add(0); }
                        op.Add((byte)mLen);
                    }
                    op.Add((byte)(mOff << 2));
                    op.Add((byte)(mOff >> 6));
                }
            }
        }
    }

    sealed class Writer
    {
        byte[] _buf;
        public int Length;
        public Writer(int capacity) => _buf = new byte[capacity];
        void Ensure(int n) { if (Length + n > _buf.Length) Array.Resize(ref _buf, Math.Max(_buf.Length * 2, Length + n)); }
        public void Add(byte b) { Ensure(1); _buf[Length++] = b; }
        public void OrAt(int i, byte b) => _buf[i] |= b;
        public void Write(ReadOnlySpan<byte> s) { Ensure(s.Length); s.CopyTo(_buf.AsSpan(Length)); Length += s.Length; }
        public byte[] ToArray() => _buf.AsSpan(0, Length).ToArray();
    }
}
