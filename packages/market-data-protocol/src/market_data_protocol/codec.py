from __future__ import annotations

import zlib
from collections.abc import Mapping
from typing import Any

import msgpack

from .model import ProtocolValidationError, validate_wire_message

_MAGIC = b"MDP1"
_FLAG_COMPRESSED = 0x01
_HEADER_SIZE = len(_MAGIC) + 1


class WireDecodeError(ValueError):
    pass


def encode_message(message: Mapping[str, Any], *, compression_threshold: int = 64 * 1024) -> bytes:
    validate_wire_message(message)
    raw = msgpack.packb(dict(message), use_bin_type=True, strict_types=False)
    flags = 0
    if compression_threshold >= 0 and len(raw) >= compression_threshold:
        raw = zlib.compress(raw, level=3)
        flags |= _FLAG_COMPRESSED
    return _MAGIC + bytes((flags,)) + raw


def decode_message(frame: bytes) -> dict[str, Any]:
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise WireDecodeError("frame must be bytes-like")
    frame = bytes(frame)
    if len(frame) < _HEADER_SIZE or not frame.startswith(_MAGIC):
        raise WireDecodeError("invalid market-data protocol header")
    flags = frame[len(_MAGIC)]
    payload = frame[_HEADER_SIZE:]
    if flags & _FLAG_COMPRESSED:
        try:
            payload = zlib.decompress(payload)
        except zlib.error as exc:
            raise WireDecodeError("invalid compressed payload") from exc
    try:
        message = msgpack.unpackb(payload, raw=False, strict_map_key=False)
    except (ValueError, msgpack.ExtraData, msgpack.FormatError, msgpack.StackError) as exc:
        raise WireDecodeError("invalid MessagePack payload") from exc
    if not isinstance(message, dict):
        raise WireDecodeError("wire payload must decode to a mapping")
    try:
        validate_wire_message(message)
    except ProtocolValidationError as exc:
        raise WireDecodeError(str(exc)) from exc
    return message
