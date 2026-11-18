"""Pure-Python LZO1X decompressor (bit-exact with miniLZO 2.10 lzo1x_decompress).

HTUSBTools.dll (Ampero II editor) links miniLZO 2.10 ("Mar 01 2017"):
  toolStartDecompress -> lzo1x_decompress   @ RVA 0xd3660
  toolStartCompress   -> lzo1x_1_compress   @ RVA 0xd3a20
"""


class LZOError(Exception):
    pass


def decompress(src, return_consumed=False):
    """Decompress a raw LZO1X stream (no header) up to its EOS marker (11 00 00)."""
    src = memoryview(bytes(src))
    n = len(src)
    out = bytearray()
    ip = 0

    def ext(t, base):
        nonlocal ip
        while src[ip] == 0:
            t += 255
            ip += 1
        t += base + src[ip]
        ip += 1
        return t

    def lits(k):
        nonlocal ip
        if ip + k > n:
            raise LZOError('input overrun')
        out.extend(src[ip:ip + k])
        ip += k

    def copy(mpos, k):
        if mpos < 0:
            raise LZOError('lookbehind overrun at out=%d' % len(out))
        if len(out) - mpos >= k:
            out.extend(out[mpos:mpos + k])
        else:
            for i in range(k):
                out.append(out[mpos + i])

    try:
        state = 'loop'
        t = 0
        if src[0] > 17:
            t = src[0] - 17
            ip = 1
            if t < 4:
                state = 'match_next'
            else:
                lits(t)
                state = 'first_literal_run'
        while True:
            if state == 'loop':
                t = src[ip]; ip += 1
                if t >= 16:
                    state = 'match'
                    continue
                if t == 0:
                    t = ext(0, 15)
                lits(t + 3)
                state = 'first_literal_run'
                continue
            if state == 'first_literal_run':
                t = src[ip]; ip += 1
                if t >= 16:
                    state = 'match'
                    continue
                mpos = len(out) - 0x801 - (t >> 2) - (src[ip] << 2); ip += 1
                copy(mpos, 3)
                state = 'match_done'
                continue
            if state == 'match':
                if t >= 64:
                    mpos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3); ip += 1
                    copy(mpos, (t >> 5) + 1)
                elif t >= 32:
                    t &= 31
                    if t == 0:
                        t = ext(0, 31)
                    mpos = len(out) - 1 - ((src[ip] | (src[ip + 1] << 8)) >> 2); ip += 2
                    copy(mpos, t + 2)
                elif t >= 16:
                    mpos = len(out) - ((t & 8) << 11)
                    t &= 7
                    if t == 0:
                        t = ext(0, 7)
                    mpos -= (src[ip] | (src[ip + 1] << 8)) >> 2; ip += 2
                    if mpos == len(out):
                        break  # EOF marker
                    mpos -= 0x4000
                    copy(mpos, t + 2)
                else:
                    mpos = len(out) - 1 - (t >> 2) - (src[ip] << 2); ip += 1
                    copy(mpos, 2)
                state = 'match_done'
                continue
            if state == 'match_done':
                t = src[ip - 2] & 3
                if t == 0:
                    state = 'loop'
                    continue
                state = 'match_next'
                continue
            if state == 'match_next':
                lits(t)
                t = src[ip]; ip += 1
                state = 'match'
                continue
    except IndexError:
        raise LZOError('input overrun at %d (out=%d)' % (ip, len(out)))
    if return_consumed:
        return bytes(out), ip
    return bytes(out)


# ---------------------------------------------------------------------------
# Compressor: straight port of miniLZO 2.10 lzo1x_1_compress (D_BITS = 14),
# so the output is byte-identical to what the editor (HTUSBTools.dll) produces.
# ---------------------------------------------------------------------------

def _lit_header(op, t, first):
    if first and t <= 238:
        op.append(17 + t)
    elif t <= 3:
        op[-2] |= t
    elif t <= 18:
        op.append(t - 3)
    else:
        tt = t - 18
        op.append(0)
        while tt > 255:
            tt -= 255
            op.append(0)
        op.append(tt)


def _xor8(src, a, b):
    return int.from_bytes(src[a:a + 8].ljust(8, b'\0'), 'little') ^ \
        int.from_bytes(src[b:b + 8].ljust(8, b'\0'), 'little')


def _do_compress(src, base, in_len, op, ti):
    """Compress src[base:base+in_len]; returns number of trailing literals not yet emitted."""
    D_BITS = 14
    D_MASK = (1 << D_BITS) - 1
    in_end = base + in_len
    ip_end = base + in_len - 20
    dict_ = [0] * (1 << D_BITS)
    ip = base
    ii = ip
    ip += (4 - ti) if ti < 4 else 0
    while True:
        # literal:
        ip += 1 + ((ip - ii) >> 5)
        while True:  # next:
            if ip >= ip_end:
                return in_end - (ii - ti)
            dv = src[ip] | (src[ip + 1] << 8) | (src[ip + 2] << 16) | (src[ip + 3] << 24)
            dindex = (((0x1824429d * dv) & 0xffffffff) >> (32 - D_BITS)) & D_MASK
            m_pos = base + dict_[dindex]
            dict_[dindex] = ip - base
            if src[m_pos:m_pos + 4] != src[ip:ip + 4] or m_pos >= ip:
                break  # -> literal
            ii -= ti
            ti = 0
            t = ip - ii
            if t:
                _lit_header(op, t, False)
                op += src[ii:ii + t]
            # 64-bit match extension as in the x64 build (UA_GET_NE64 + ctz):
            # the ip_end bound is only checked after each 8-byte step
            m_len = 4
            v = _xor8(src, ip + m_len, m_pos + m_len)
            if v == 0:
                while True:
                    m_len += 8
                    v = _xor8(src, ip + m_len, m_pos + m_len)
                    if ip + m_len >= ip_end:
                        break
                    if v:
                        m_len += ((v & -v).bit_length() - 1) >> 3
                        break
            else:
                m_len += ((v & -v).bit_length() - 1) >> 3
            m_off = ip - m_pos
            ip += m_len
            ii = ip
            if m_len <= 8 and m_off <= 0x800:
                m_off -= 1
                op.append(((m_len - 1) << 5) | ((m_off & 7) << 2))
                op.append(m_off >> 3)
            elif m_off <= 0x4000:
                m_off -= 1
                if m_len <= 33:
                    op.append(32 | (m_len - 2))
                else:
                    m_len -= 33
                    op.append(32)
                    while m_len > 255:
                        m_len -= 255
                        op.append(0)
                    op.append(m_len)
                op.append((m_off << 2) & 0xff)
                op.append((m_off >> 6) & 0xff)
            else:
                m_off -= 0x4000
                if m_len <= 9:
                    op.append(16 | ((m_off >> 11) & 8) | (m_len - 2))
                else:
                    m_len -= 9
                    op.append(16 | ((m_off >> 11) & 8))
                    while m_len > 255:
                        m_len -= 255
                        op.append(0)
                    op.append(m_len)
                op.append((m_off << 2) & 0xff)
                op.append((m_off >> 6) & 0xff)


def compress(data):
    """LZO1X-1 compress (raw stream incl. EOS marker, no header)."""
    src = bytes(data)
    n = len(src)
    op = bytearray()
    pos = 0
    left = n
    t = 0
    while left > 20:
        ll = min(left, 49152)
        t = _do_compress(src, pos, ll, op, t)
        pos += ll
        left -= ll
    t += left
    if t > 0:
        ii = n - t
        _lit_header(op, t, len(op) == 0)
        op += src[ii:ii + t]
    op += b'\x11\x00\x00'
    return bytes(op)


if __name__ == '__main__':
    import sys
    if len(sys.argv) < 4 or sys.argv[1] not in ('d', 'c'):
        print('usage: lzo1x.py d|c <in> <out> [offset]')
        sys.exit(1)
    data = open(sys.argv[2], 'rb').read()
    if sys.argv[1] == 'd':
        off = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
        res, used = decompress(data[off:], return_consumed=True)
        print('in %d bytes -> out %d bytes' % (used, len(res)))
    else:
        res = compress(data)
        print('in %d bytes -> out %d bytes' % (len(data), len(res)))
    open(sys.argv[3], 'wb').write(res)
