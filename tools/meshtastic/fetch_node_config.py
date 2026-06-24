#!/usr/bin/env python3
"""
fetch_node_config.py - Capture a Meshtastic node's want_config burst over a
DIRECT USB serial link (bypassing the GATT/mesh proxy).

Purpose
-------
1. Verify the FETCHING ground truth: trigger the firmware's `want_config`
   handshake and observe the exact sequence of FromRadio frames the node emits
   in response (the "config burst"), terminated by a FromRadio whose
   `config_complete_id` equals the nonce we sent.
2. MEASURE the real CONFIG_CACHE_ARENA_BYTES *without reflashing*: sum the
   protobuf payload bytes of every frame in the burst so the host-side config
   cache arena can be sized from ground-truth data instead of a guess.

This tool talks the Meshtastic
Stream API framing directly so it does NOT depend on the meshtastic Python
client being installed; the meshtastic package is used only as an *optional*
enhancement for pretty-printing decoded protobuf contents.

Stream API framing (exactly as the firmware implements it)
----------------------------------------------------------
Every frame on the wire is:

    [0x94][0xC3][len_hi][len_lo][... protobuf payload ...]

  - 0x94 0xC3 : 2-byte start magic (START1, START2).
  - len_hi/len_lo : 16-bit big-endian length of the protobuf payload.
  - payload : a serialized ToRadio (TX) or FromRadio (RX) protobuf.
  - Max payload length is 512 bytes; longer is a framing error.

Wake / resync preamble: before the first real frame we send 32 bytes of 0xC3.
The firmware's stream parser treats a run of 0xC3 as a harmless resync signal
that flushes any half-parsed frame, guaranteeing we start from a clean state.

Handshake
---------
We send a ToRadio{ want_config_id = <nonce> }. ToRadio.want_config_id is
field 3 (varint). The node replies with a burst of FromRadio frames
(my_info, node_info(s), config(s), moduleConfig(s), channel(s), metadata,
deviceuiConfig, ...) and finally a FromRadio{ config_complete_id = <nonce> }
which closes the burst. We stop on that frame (or on timeout).

Top-level variant detection (dependency-free)
---------------------------------------------
FromRadio is a protobuf `oneof`. Each variant is a distinct top-level field
number, so we can identify a frame WITHOUT a .proto by reading just the first
protobuf tag (key varint). field_number = tag >> 3. The known FromRadio
variant field numbers are encoded in FROMRADIO_VARIANTS below.

Usage
-----
  # Auto-detect port, random nonce:
  python3 fetch_node_config.py

  # Pin a port and nonce, dump raw frames for the host unit test:
  python3 fetch_node_config.py --port /dev/ttyACM0 \
      --nonce 305419896 --dump .tmp/config_burst.hex

  # Longer timeout for a slow / busy node:
  python3 fetch_node_config.py --timeout 30

Exit codes
----------
  0 : config_complete_id matching our nonce was received (success).
  1 : timeout before config_complete (or no port found / serial error).
  2 : bad arguments.

pip deps
--------
  pyserial        (REQUIRED)   pip install pyserial
  meshtastic      (OPTIONAL)   pip install meshtastic   # nicer decoded output
"""

from __future__ import annotations

import argparse
import glob
import os
import random
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover - environment guard
    sys.stderr.write(
        "ERROR: pyserial is required. Install with: pip install pyserial\n"
    )
    sys.exit(2)

# --- Optional meshtastic protobufs (enhancement only) ------------------------
# If importable, we use them to (a) build ToRadio canonically and (b) decode &
# pretty-print FromRadio frames. The core measurement path never needs them.
_MESH_PB = None
try:
    from meshtastic.protobuf import mesh_pb2 as _MESH_PB  # type: ignore
except Exception:  # broad: package layout has varied across versions
    try:
        from meshtastic import mesh_pb2 as _MESH_PB  # type: ignore
    except Exception:
        _MESH_PB = None

# --- Stream API constants (mirror the firmware) ------------------------------
START1 = 0x94
START2 = 0xC3
MAX_PAYLOAD = 512
WAKE_BYTE = 0xC3
WAKE_COUNT = 32
WAKE_SETTLE_S = 0.10  # ~100 ms after preamble before sending the request

# FromRadio oneof variant field numbers -> human name.
# (Field numbers are stable protocol contract, independent of the .proto file.)
FROMRADIO_VARIANTS = {
    2: "packet",
    3: "my_info",
    4: "node_info",
    5: "config",
    6: "log_record",
    7: "config_complete_id",
    9: "moduleConfig",
    10: "channel",
    11: "queueStatus",
    13: "metadata",
    15: "fileInfo",
    17: "deviceuiConfig",
}

# ToRadio.want_config_id is field 3, wire type 0 (varint).
TORADIO_WANT_CONFIG_FIELD = 3


# --- Tiny protobuf primitives (no dependency) --------------------------------
def encode_varint(value: int) -> bytes:
    """Encode an unsigned integer as a protobuf base-128 varint."""
    if value < 0:
        raise ValueError("varint must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)  # more bytes follow -> set continuation bit
        else:
            out.append(byte)
            break
    return bytes(out)


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    """Read a varint from buf at pos. Return (value, new_pos)."""
    result = 0
    shift = 0
    while pos < len(buf):
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return result, pos
        shift += 7
    raise ValueError("truncated varint")


def build_toradio_want_config(nonce: int) -> bytes:
    """
    Build a serialized ToRadio{ want_config_id = nonce }.

    Primary path: use meshtastic protobufs if present (authoritative encoding).
    Fallback path: hand-encode the single field. ToRadio.want_config_id is
    field 3, wire type 0 (varint), so the tag byte is (3 << 3) | 0 = 0x18.
    """
    if _MESH_PB is not None:
        to_radio = _MESH_PB.ToRadio()
        to_radio.want_config_id = nonce
        return to_radio.SerializeToString()

    # Hand-encoded fallback.
    tag = (TORADIO_WANT_CONFIG_FIELD << 3) | 0  # field 3, varint
    return encode_varint(tag) + encode_varint(nonce)


def identify_variant(payload: bytes) -> tuple[int, str]:
    """
    Identify a FromRadio frame's oneof variant by reading the first top-level
    protobuf tag. Returns (field_number, variant_name).

    We only need the very first tag: a FromRadio is a single oneof, so the
    first (and effectively only top-level) field number is the variant. This
    works without the .proto definition.
    """
    if not payload:
        return (0, "<empty>")
    try:
        tag, _ = read_varint(payload, 0)
    except ValueError:
        return (0, "<unparseable>")
    field_number = tag >> 3
    name = FROMRADIO_VARIANTS.get(field_number, f"<unknown:{field_number}>")
    return (field_number, name)


def extract_config_complete_id(payload: bytes) -> int | None:
    """
    If this FromRadio carries config_complete_id (field 7, varint), return its
    value; otherwise None. Dependency-free: we just check the leading tag.
    """
    if not payload:
        return None
    try:
        tag, pos = read_varint(payload, 0)
    except ValueError:
        return None
    if (tag >> 3) != 7 or (tag & 0x07) != 0:  # field 7, wire type 0 (varint)
        return None
    try:
        value, _ = read_varint(payload, pos)
    except ValueError:
        return None
    return value


def pretty_decode(payload: bytes) -> str:
    """Optional: decode a FromRadio with the meshtastic protobufs, if present."""
    if _MESH_PB is None:
        return ""
    try:
        from_radio = _MESH_PB.FromRadio()
        from_radio.ParseFromString(payload)
        text = str(from_radio).strip()
        # Keep the table readable: single line, truncated.
        flat = " ".join(text.split())
        return flat[:120] + ("..." if len(flat) > 120 else "")
    except Exception:
        return ""


# --- Serial framing ----------------------------------------------------------
def autodetect_port() -> str | None:
    """Return the first /dev/ttyACM* then /dev/ttyUSB* device, or None."""
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def send_frame(ser: "serial.Serial", payload: bytes) -> None:
    """Wrap a protobuf payload in the Stream API frame and write it."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds max {MAX_PAYLOAD}")
    header = bytes([START1, START2, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
    ser.write(header + payload)
    ser.flush()


def read_frames(ser: "serial.Serial", nonce: int, timeout_s: float):
    """
    Read framed FromRadio responses until a config_complete_id == nonce is
    seen or the overall timeout elapses.

    Returns (frames, complete_seen) where frames is a list of dicts:
        { "field": int, "name": str, "payload": bytes,
          "frame_len": int, "decoded": str }
    `frame_len` is the full on-wire frame size (4-byte header + payload),
    while payload length is what counts toward the protobuf cache arena.

    Implementation: a small state machine over a rolling byte buffer so we are
    robust to partial reads and to stray bytes / wake echoes between frames.
    """
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    frames = []
    complete_seen = False

    while time.monotonic() < deadline and not complete_seen:
        chunk = ser.read(256)  # blocks up to ser.timeout, then returns
        if chunk:
            buf.extend(chunk)

        # Drain as many complete frames as the buffer holds.
        while True:
            # Find the start magic; discard any leading noise (e.g. wake echo).
            idx = buf.find(bytes([START1, START2]))
            if idx == -1:
                # No magic yet. Keep a 1-byte tail in case 0x94 is split.
                if len(buf) > 1:
                    del buf[:-1]
                break
            if idx > 0:
                del buf[:idx]  # drop bytes before the magic
            if len(buf) < 4:
                break  # need the full header
            length = (buf[2] << 8) | buf[3]
            if length > MAX_PAYLOAD:
                # Bad length: not a real frame start, skip the magic and retry.
                del buf[:2]
                continue
            if len(buf) < 4 + length:
                break  # wait for the rest of the payload
            payload = bytes(buf[4:4 + length])
            del buf[:4 + length]

            field, name = identify_variant(payload)
            frames.append({
                "field": field,
                "name": name,
                "payload": payload,
                "frame_len": 4 + length,
                "decoded": pretty_decode(payload),
            })

            cc = extract_config_complete_id(payload)
            if cc is not None and cc == nonce:
                complete_seen = True
                break

    return frames, complete_seen


# --- Reporting ---------------------------------------------------------------
def write_dump(path: str, frames: list, nonce: int) -> None:
    """Write raw frames (hex + lengths) so the burst can be replayed into the
    host config_cache unit test."""
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"# Meshtastic want_config burst dump\n")
        fh.write(f"# nonce={nonce}\n")
        fh.write(f"# frame_count={len(frames)}\n")
        fh.write("# columns: index variant payload_len frame_len payload_hex\n")
        for i, fr in enumerate(frames):
            fh.write(
                f"{i}\t{fr['name']}\t{len(fr['payload'])}\t{fr['frame_len']}\t"
                f"{fr['payload'].hex()}\n"
            )


def print_report(frames: list, complete_seen: bool, nonce: int) -> None:
    """Print the per-variant table, totals, order, and the arena-sizing line."""
    # Aggregate per variant: count and total PAYLOAD bytes (protobuf-only).
    agg: dict[str, dict] = {}
    order: list[str] = []
    total_payload = 0
    total_frame = 0
    for fr in frames:
        name = fr["name"]
        plen = len(fr["payload"])
        total_payload += plen
        total_frame += fr["frame_len"]
        order.append(name)
        bucket = agg.setdefault(name, {"count": 0, "payload": 0})
        bucket["count"] += 1
        bucket["payload"] += plen

    print("=" * 64)
    print("Meshtastic want_config burst capture")
    print(f"  nonce (want_config_id): {nonce}")
    print(f"  meshtastic protobufs:   {'available' if _MESH_PB else 'not installed (raw mode)'}")
    print("=" * 64)

    print("\nPer-variant table:")
    print(f"  {'variant':<22}{'frames':>8}{'payload_bytes':>16}")
    print(f"  {'-' * 22}{'-' * 8:>8}{'-' * 16:>16}")
    for name in agg:  # dict preserves first-seen insertion order
        b = agg[name]
        print(f"  {name:<22}{b['count']:>8}{b['payload']:>16}")

    print(f"\n  TOTAL payload bytes (protobuf):  {total_payload}")
    print(f"  TOTAL on-wire bytes (w/ headers): {total_frame}")
    print(f"  Frame count:                      {len(frames)}")

    print("\nVariant ORDER as received:")
    print("  " + " -> ".join(order) if order else "  <none>")

    # Optional decoded contents (only when meshtastic is installed).
    if _MESH_PB is not None and frames:
        print("\nDecoded frames (meshtastic):")
        for i, fr in enumerate(frames):
            if fr["decoded"]:
                print(f"  [{i}] {fr['name']}: {fr['decoded']}")

    print()
    if complete_seen:
        # The arena must hold the full protobuf payload burst; add margin.
        print(
            f"Set CONFIG_CACHE_ARENA_BYTES >= {total_payload} (+ margin)."
        )
    else:
        print(
            "WARNING: config_complete_id was NOT seen before timeout; the "
            f"total below is a LOWER BOUND only ({total_payload} payload bytes). "
            "Do not size the arena from a partial burst."
        )


# --- Main --------------------------------------------------------------------
def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture a Meshtastic node's want_config burst over direct "
                    "USB serial and measure CONFIG_CACHE_ARENA_BYTES.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--port", default=None,
        help="Serial device. Default: auto-detect first /dev/ttyACM* then "
             "/dev/ttyUSB*.",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument(
        "--timeout", type=float, default=20.0,
        help="Overall capture timeout in seconds.",
    )
    parser.add_argument(
        "--nonce", type=int, default=None,
        help="want_config_id nonce (uint32). Default: random non-zero, non-one. "
             "0 and 1 are reserved/sentinel values in the protocol.",
    )
    parser.add_argument(
        "--dump", default=None,
        help="Optional path to write raw frames (hex + lengths) for replay "
             "into the host config_cache unit test.",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    # Resolve nonce: random non-zero, non-one uint32 if not provided.
    if args.nonce is None:
        nonce = random.randint(2, 0xFFFFFFFF)
    else:
        nonce = args.nonce & 0xFFFFFFFF
        if nonce in (0, 1):
            sys.stderr.write("ERROR: --nonce must be non-zero and non-one.\n")
            return 2

    # Resolve port.
    port = args.port or autodetect_port()
    if not port:
        sys.stderr.write(
            "ERROR: no serial port found (looked for /dev/ttyACM*, "
            "/dev/ttyUSB*). Pass --port explicitly.\n"
        )
        return 1

    # Per-read timeout: keep it short so the overall deadline stays responsive.
    read_timeout = min(0.5, max(0.05, args.timeout / 10.0))

    try:
        ser = serial.Serial(port, args.baud, timeout=read_timeout)
    except serial.SerialException as exc:
        sys.stderr.write(f"ERROR: could not open {port}: {exc}\n")
        return 1

    try:
        sys.stderr.write(f"[info] port={port} baud={args.baud} nonce={nonce}\n")

        # 1) Wake / resync preamble: 32x 0xC3, then settle ~100 ms.
        ser.reset_input_buffer()
        ser.write(bytes([WAKE_BYTE]) * WAKE_COUNT)
        ser.flush()
        time.sleep(WAKE_SETTLE_S)

        # 2) Send ToRadio{ want_config_id = nonce }.
        payload = build_toradio_want_config(nonce)
        send_frame(ser, payload)
        sys.stderr.write(
            f"[info] sent want_config ({len(payload)} payload bytes); reading "
            f"burst (timeout {args.timeout}s)...\n"
        )

        # 3) Read the burst until config_complete or timeout.
        frames, complete_seen = read_frames(ser, nonce, args.timeout)
    finally:
        ser.close()

    # 4) Optional raw dump for the host unit test.
    if args.dump:
        write_dump(args.dump, frames, nonce)
        sys.stderr.write(f"[info] wrote raw frames to {args.dump}\n")

    # 5) Report.
    print_report(frames, complete_seen, nonce)

    if complete_seen:
        print("\nPASS: config_complete_id matched nonce.")
        return 0
    print("\nFAIL: timeout before config_complete_id.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
