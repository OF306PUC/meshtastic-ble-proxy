"""check_node_info.py  –  Meshtastic node & mesh inspector
==========================================================
Connects to a node over serial and reports, in one pass:

  1. Local node        – identity, role, firmware, telemetry (battery, channel
                          & air utilization, uptime), position, SNR.
  2. Device config      – every setting the configure scripts touch, read back
                          from the node (device/role, BLE, LoRa, GPS + intervals,
                          telemetry intervals, serial module). Role-agnostic:
                          works for sensor, gateway and proxy nodes alike.
  3. Radio health-check – region / preset / rebroadcast / channels / PSKs
                          compared against radio_config.py. A mismatch here
                          means this node cannot talk to the rest of the mesh.
                          PSKs are compared but never printed.

Exit code is 1 when any drift / mismatch is found, 0 when the node is healthy,
so the tool doubles as a CI / smoke health-check.

Usage:
    python tools/meshtastic/check_node_info.py                 # auto-detect port
    python tools/meshtastic/check_node_info.py --port /dev/ttyACM0
"""
import argparse
import base64
import sys
from pathlib import Path

import meshtastic
import meshtastic.serial_interface

# Sibling tool modules live in this directory; make them importable when run
# directly (mirrors configure.py).
sys.path.insert(0, str(Path(__file__).resolve().parent))

import radio_config  # noqa: E402

# ── Small formatting helpers ──────────────────────────────────────────────────


def enum_name(msg, field: str) -> str:
    """Return the symbolic name of a protobuf enum field via reflection.

    Reflection keeps this working across meshtastic library versions without
    importing config_pb2 (whose module path has moved between releases).
    """
    try:
        value = getattr(msg, field)
        enum_type = msg.DESCRIPTOR.fields_by_name[field].enum_type
        return enum_type.values_by_number[value].name
    except Exception:
        return str(getattr(msg, field, "?"))


def decode_psk(psk_b64: str) -> bytes:
    """Decode a channel PSK as stored in .env: 'base64:<key>' or bare base64.

    Mirrors proxy/configure.py::decode_psk so the health-check compares against
    exactly what the configuration scripts write to the device.
    """
    if psk_b64 and psk_b64.startswith("base64:"):
        psk_b64 = psk_b64[len("base64:"):]
    return base64.b64decode(psk_b64)


def fmt(value, unit: str = "") -> str:
    """Format an optional metric, falling back to '?' when absent."""
    if value is None:
        return "?"
    return f"{value}{unit}"


def field(msg, name: str):
    """Read a protobuf field, returning 'n/a' if it doesn't exist in this
    firmware/library version (field sets drift across releases)."""
    try:
        return getattr(msg, name)
    except Exception:
        return "n/a"


# ── Port resolution ───────────────────────────────────────────────────────────


def resolve_port(explicit_port):
    """Return the serial port to use, auto-detecting when not given.

    Exits with a clear message when zero or several Meshtastic devices are
    found and no --port was supplied (we won't guess between candidates).
    """
    if explicit_port:
        return explicit_port
    try:
        from meshtastic.util import findPorts
        ports = findPorts()
    except Exception as exc:
        print(f"ERROR: could not auto-detect a port ({exc}). Pass --port explicitly.")
        sys.exit(2)
    if not ports:
        print("ERROR: no Meshtastic device found. Plug one in or pass --port.")
        sys.exit(2)
    if len(ports) > 1:
        print("Multiple Meshtastic devices found; pass --port to choose one:")
        for p in ports:
            print(f"  {p}")
        sys.exit(2)
    print(f"Auto-detected port: {ports[0]}")
    return ports[0]


# ── Section 1: local node ───────────────────────────────────────────────────


def print_local_node(iface, info: dict) -> str:
    """Print identity + telemetry of the connected node; return its node id."""
    user = info.get("user", {}) or {}
    metrics = info.get("deviceMetrics", {}) or {}
    node_id = user.get("id", "?")

    print("=== Local node ===")
    print(f"  Node number : {info.get('num', '?')}")
    print(f"  Node ID     : {node_id}")
    print(f"  Long name   : {user.get('longName', '?')}")
    print(f"  Short name  : {user.get('shortName', '?')}")
    print(f"  Hardware    : {user.get('hwModel', '?')}")
    # role is omitted from NodeInfo when it is the default CLIENT(0).
    print(f"  Role        : {user.get('role', 'CLIENT')}")

    # Firmware version comes from device metadata, fetched at connect time.
    fw = "?"
    try:
        fw = iface.metadata.firmware_version or "?"
    except Exception:
        pass
    print(f"  Firmware    : {fw}")

    print("  Telemetry:")
    print(f"    Battery            : {fmt(metrics.get('batteryLevel'), '%')}")
    print(f"    Voltage            : {fmt(round(metrics['voltage'], 2) if metrics.get('voltage') is not None else None, ' V')}")
    print(f"    Channel util       : {fmt(round(metrics['channelUtilization'], 1) if metrics.get('channelUtilization') is not None else None, '%')}")
    print(f"    Air util (TX)      : {fmt(round(metrics['airUtilTx'], 1) if metrics.get('airUtilTx') is not None else None, '%')}")
    uptime = metrics.get("uptimeSeconds")
    print(f"    Uptime             : {fmt(uptime // 3600 if uptime is not None else None, 'h') if uptime else fmt(uptime)}")
    print(f"    SNR (last rx)      : {fmt(info.get('snr'), ' dB')}")

    pos = info.get("position", {}) or {}
    if pos.get("latitude") is not None and pos.get("longitude") is not None:
        print(f"    Position           : {pos['latitude']:.5f}, {pos['longitude']:.5f}"
              f" (alt {fmt(pos.get('altitude'), ' m')})")
    else:
        print("    Position           : (no fix)")
    print()
    return node_id


# ── Section 2: full on-device configuration (role-agnostic) ──────────────────


def print_device_config(iface) -> None:
    """Dump every setting the configuration scripts touch, as read back from
    the node — so any node (sensor / gateway / proxy) can be verified against
    what it was meant to be. Pure reporting: the expected values are
    role-specific (they live in each role's configure_params.py), so this
    section does not judge, it just shows the on-device truth.
    """
    print("=== Device configuration (as read from the node) ===")

    def show(label, value):
        print(f"  {label:<32}: {value}")

    try:
        cfg = iface.localNode.localConfig
    except Exception as exc:
        print(f"  Could not read localConfig: {exc}\n")
        return

    # device: role + rebroadcast (rebroadcast is a no-op for CLIENT_MUTE, but
    # we still show whatever the device reports).
    show("device.role", enum_name(cfg.device, "role"))
    show("device.rebroadcast_mode", enum_name(cfg.device, "rebroadcast_mode"))

    # LoRa: the mesh-wide trio (also health-checked below against radio_config).
    show("lora.region", enum_name(cfg.lora, "region"))
    show("lora.modem_preset", enum_name(cfg.lora, "modem_preset"))
    show("lora.hop_limit", field(cfg.lora, "hop_limit"))
    # SX126x-only; on an SX127x T-Beam this is stored but ignored (reported,
    # not health-checked, so those nodes don't show a false mismatch).
    show("lora.sx126x_rx_boosted_gain", field(cfg.lora, "sx126x_rx_boosted_gain"))

    # bluetooth: proxies disable it (nRF52840 serves BLE); others use default.
    show("bluetooth.enabled", field(cfg.bluetooth, "enabled"))

    # position / GPS: mode + both intervals.
    show("position.gps_mode", enum_name(cfg.position, "gps_mode"))
    show("position.gps_update_interval", fmt(field(cfg.position, "gps_update_interval"), " s"))
    show("position.position_broadcast_secs", fmt(field(cfg.position, "position_broadcast_secs"), " s"))

    # Telemetry + serial live in moduleConfig, not localConfig.
    try:
        mod = iface.localNode.moduleConfig
        show("telemetry.device_update_interval",
             fmt(field(mod.telemetry, "device_update_interval"), " s"))
        show("telemetry.environment_update_interval",
             fmt(field(mod.telemetry, "environment_update_interval"), " s"))
        show("telemetry.environment_measurement_enabled",
             field(mod.telemetry, "environment_measurement_enabled"))
        # Serial module: only meaningful on the proxy-attached nodes (Stream
        # API on UART1), but shown for every node for completeness.
        show("serial.enabled", field(mod.serial, "enabled"))
        show("serial.mode", enum_name(mod.serial, "mode"))
        show("serial.txd", field(mod.serial, "txd"))
        show("serial.rxd", field(mod.serial, "rxd"))
        show("serial.baud", enum_name(mod.serial, "baud"))
        show("serial.timeout", field(mod.serial, "timeout"))
    except Exception as exc:
        print(f"  Could not read moduleConfig: {exc}")
    print()


# ── Section 3: radio health-check vs common/radio_config.py ──────────────────


def radio_health_check(iface) -> list:
    """Compare on-device radio settings against radio_config; return issues."""
    print("=== Radio health-check (vs common/radio_config.py) ===")
    issues = []

    def check(label, expected, actual):
        ok = expected == actual
        mark = "OK " if ok else "MISMATCH"
        # Never print raw PSK bytes; the caller passes a redacted 'actual'.
        print(f"  [{mark}] {label}: expected={expected} actual={actual}")
        if not ok:
            issues.append(f"radio: {label} (expected {expected}, got {actual})")

    try:
        cfg = iface.localNode.localConfig
    except Exception as exc:
        print(f"  Could not read localConfig: {exc}")
        return ["radio: localConfig unreadable"]

    check("region", radio_config.LORA_REGION, enum_name(cfg.lora, "region"))
    check("modem_preset", radio_config.LORA_PRESET, enum_name(cfg.lora, "modem_preset"))
    check("rebroadcast_mode", radio_config.REBROADCAST_MODE,
          enum_name(cfg.device, "rebroadcast_mode"))

    # Channels: name + PSK, by index. PSK is compared as bytes but reported
    # only as a match/mismatch verdict so the secret never hits the console.
    try:
        channels = iface.localNode.channels
    except Exception as exc:
        print(f"  Could not read channels: {exc}")
        issues.append("radio: channels unreadable")
        print()
        return issues

    expected_channels = [
        (radio_config.CHANNEL_TELEMETRY_IDX, radio_config.CHANNEL_TELEMETRY_NAME,
         radio_config.CHANNEL_TELEMETRY_PSK_B64),
        (radio_config.CHANNEL_MSG_IDX, radio_config.CHANNEL_MSG_NAME,
         radio_config.CHANNEL_MSG_PSK_B64),
    ]
    for idx, exp_name, exp_psk_b64 in expected_channels:
        if idx >= len(channels):
            print(f"  [MISMATCH] channel {idx}: missing on device (expected '{exp_name}')")
            issues.append(f"radio: channel {idx} missing (expected '{exp_name}')")
            continue
        settings = channels[idx].settings
        check(f"channel {idx} name", exp_name, settings.name)
        try:
            psk_ok = bytes(settings.psk) == decode_psk(exp_psk_b64)
        except Exception as exc:
            psk_ok = False
            print(f"  [MISMATCH] channel {idx} PSK: could not decode expected ({exc})")
        verdict = "OK " if psk_ok else "MISMATCH"
        print(f"  [{verdict}] channel {idx} PSK: {'matches .env' if psk_ok else 'differs from .env'}")
        if not psk_ok:
            issues.append(f"radio: channel {idx} PSK differs from .env")
    print()
    return issues


# ── Entry point ───────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect a Meshtastic node and health-check it against "
                    "radio_config.py.")
    parser.add_argument("--port", help="Serial port (e.g. /dev/ttyACM0). "
                                       "Auto-detected when omitted.")
    args = parser.parse_args()

    port = resolve_port(args.port)

    try:
        iface = meshtastic.serial_interface.SerialInterface(port)
    except Exception as exc:
        print(f"ERROR: could not open {port}: {exc}")
        return 2

    issues = []
    try:
        info = iface.getMyNodeInfo()
        print_local_node(iface, info)
        print_device_config(iface)
        issues += radio_health_check(iface)
    finally:
        iface.close()

    if issues:
        print("=== Issues found ===")
        for issue in issues:
            print(f"  ! {issue}")
        print(f"\n{len(issues)} issue(s) detected.")
        return 1

    print("=== Node healthy: no drift or mismatch detected. ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
