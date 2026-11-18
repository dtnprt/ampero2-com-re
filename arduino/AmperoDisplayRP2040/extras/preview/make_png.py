"""Converts the raw RGB565 frames written by preview.exe into PNG files (no dependencies)."""
import struct, sys, zlib, glob, os
def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
for f in glob.glob(os.path.join(sys.argv[1] if len(sys.argv) > 1 else '.', '*.rgb565')):
    d = open(f, 'rb').read(); w, h = struct.unpack_from('<HH', d)
    px = struct.unpack_from('<%dH' % (w*h), d, 4)
    rgb = bytearray()
    for v in px: rgb += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    png(f[:-7] + '.png', w, h, bytes(rgb)); print(f[:-7] + '.png')
