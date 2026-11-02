"""Reject OELFs that lose initialized writable sections during SELF conversion.

Checks actual loader segments rather than retained section headers: the latter
can still name bytes which are outside PT_LOAD.p_filesz and will be zeroed.
Uses only the Python standard library, on Windows and in the build rootfs.
"""
from pathlib import Path
import struct
import sys


def elf(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError(f'{path}: expected little-endian ELF64')
    h = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    ph = [struct.unpack_from('<IIQQQQQQ', data, h[5] + i * h[9]) for i in range(h[10])]
    sh = [struct.unpack_from('<IIQQQQIIQQ', data, h[6] + i * h[11]) for i in range(h[12])]
    names_section = sh[h[13]]
    names = data[names_section[4]:names_section[4] + names_section[5]]
    sections = [(names[s[0]:].split(b'\0', 1)[0].decode(), s) for s in sh]
    return data, ph, sections


def validate(source, converted):
    original, _, sections = elf(source)
    image, segments, _ = elf(converted)
    # OpenOrbis's SCE_RELRO segment is loaded from file as well as PT_LOAD.
    loads = [p for p in segments if p[0] in (1, 0x61000010)]
    errors = []
    checked = 0
    for name, s in sections:
        if (s[2] & 3) != 3 or s[1] == 8 or not s[5]:
            continue  # Only allocated, writable, initialized storage.
        if name == '.dynamic':
            continue  # create-fself deliberately replaces this metadata.
        address, size = s[3], s[5]
        segment = next((p for p in loads if p[3] <= address and address + size <= p[3] + p[5]), None)
        if segment is None:
            errors.append(f'{name}: 0x{address:x}..0x{address+size:x} is outside file-backed load segments')
            continue
        checked += 1
        if name == '.data' or name.startswith('.data.') or name.startswith('.init_array'):
            offset = segment[2] + address - segment[3]
            if original[s[4]:s[4]+size] != image[offset:offset+size]:
                errors.append(f'{name}: initialized bytes changed during SELF conversion')
    if errors:
        raise ValueError('Invalid PS4 load image:\n' + '\n'.join(errors))
    return checked


if __name__ == '__main__':
    try:
        checked = validate(sys.argv[1], sys.argv[2])
    except (ValueError, OSError, IndexError, struct.error) as error:
        raise SystemExit(str(error))
    print(f'PS4 load image: {checked} initialized writable sections preserved')
