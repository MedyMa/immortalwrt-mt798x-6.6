"""UniFi TNBU framing for the outbound AP inform connection."""
import json
import os
import struct
import zlib

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HEADER = struct.Struct("!4sI6sH16sII")
LIMIT = 1024 * 1024
FACTORY_KEY = "ba86f2bbe107c7c57eb5f2690775c712"


def packet(message, mac, key, gcm):
    data = zlib.compress(json.dumps(message, separators=(",", ":")).encode())
    iv = os.urandom(16)
    if gcm:
        head = HEADER.pack(b"TNBU", 1, mac, 11, iv, 1, len(data) + 16)
        return head + AESGCM(bytes.fromhex(key)).encrypt(iv, data, head)
    pad = padding.PKCS7(128).padder()
    cipher = Cipher(algorithms.AES(bytes.fromhex(key)), modes.CBC(iv)).encryptor()
    body = cipher.update(pad.update(data) + pad.finalize()) + cipher.finalize()
    return HEADER.pack(b"TNBU", 1, mac, 3, iv, 1, len(body)) + body


def snappy(data):
    # Raw Snappy block. Validate the advertised size before expanding any copies.
    size = pos = 0
    for shift in range(0, 35, 7):
        if pos == len(data):
            raise ValueError("short snappy length")
        byte = data[pos]
        pos += 1
        size |= (byte & 127) << shift
        if byte < 128:
            break
    else:
        raise ValueError("invalid snappy length")
    if size > LIMIT:
        raise ValueError("oversized snappy block")
    out = bytearray()
    while pos < len(data):
        tag = data[pos]
        pos += 1
        kind, n = tag & 3, tag >> 2
        if not kind:
            if n >= 60:
                count = n - 59
                if pos + count > len(data):
                    raise ValueError("short snappy literal length")
                n = int.from_bytes(data[pos:pos + count], "little")
                pos += count
            n += 1
            if pos + n > len(data) or len(out) + n > size:
                raise ValueError("invalid snappy literal")
            out.extend(data[pos:pos+n])
            pos += n
        else:
            count = (1, 2, 4)[kind - 1]
            if pos + count > len(data):
                raise ValueError("short snappy copy")
            offset = int.from_bytes(data[pos:pos+count], "little")
            pos += count
            if kind == 1:
                offset |= (tag & 224) << 3
                n = 4 + (n & 7)
            else:
                n += 1
            if not 0 < offset <= len(out) or len(out) + n > size:
                raise ValueError("invalid snappy copy")
            start = len(out) - offset
            chunk = out[start:start + min(offset, n)]
            out.extend((chunk * ((n + len(chunk) - 1) // len(chunk)))[:n])
    if len(out) != size:
        raise ValueError("snappy size mismatch")
    return bytes(out)


def unpack(data, mac, key, require_gcm=False):
    if not HEADER.size <= len(data) <= LIMIT + HEADER.size:
        raise ValueError("invalid inform size")
    magic, version, device, flags, iv, payload_version, size = HEADER.unpack_from(data)
    body = data[HEADER.size:]
    if (magic != b"TNBU" or version != 1 or device != mac or payload_version != 1 or
            size != len(body) or flags not in (1, 3, 5, 9, 11, 13)):
        raise ValueError("invalid inform header")
    if flags & 8:
        plain = AESGCM(bytes.fromhex(key)).decrypt(iv, body, data[:HEADER.size])
    else:
        if require_gcm:
            raise ValueError("GCM required")
        cipher = Cipher(algorithms.AES(bytes.fromhex(key)), modes.CBC(iv)).decryptor()
        unpad = padding.PKCS7(128).unpadder()
        plain = unpad.update(cipher.update(body) + cipher.finalize()) + unpad.finalize()
    if flags & 2:
        stream = zlib.decompressobj()
        plain = stream.decompress(plain, LIMIT + 1)
        if len(plain) > LIMIT or not stream.eof or stream.unused_data:
            raise ValueError("invalid compressed inform")
    elif flags & 4:
        plain = snappy(plain)
    msg = json.loads(plain)
    if not isinstance(msg, dict):
        raise ValueError("expected inform object")
    return msg
