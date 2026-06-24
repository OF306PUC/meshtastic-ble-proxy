import argparse
import subprocess
import time
import secrets
import base64
import os
import sys

import meshtastic.serial_interface

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import param_node as node_params

MAX_RETRIES = 3
RETRY_DELAY = 10  # seconds to wait before retrying
SETTLE_DELAY = 10  # seconds to let the node settle after a successful command

# Keywords that indicate a transient serial error worth retrying
RETRYABLE_ERRORS = [
    "couldn't be opened",
    "Input/output error",
    "OS Error",
    "serial device",
    "write failed",
]


def is_retryable(stderr: str, stdout: str) -> bool:
    combined = (stderr + stdout).lower()
    return any(keyword.lower() in combined for keyword in RETRYABLE_ERRORS)


def run(cmd, retries=MAX_RETRIES) -> bool:
    """Run a meshtastic CLI command given as an argv list (no shell).

    Returns True on success, False if it still failed after `retries`
    attempts. Transient serial errors (RETRYABLE_ERRORS) are retried.
    """
    print(f"\nRunning: {' '.join(cmd)}")
    for attempt in range(1, retries + 1):
        result = subprocess.run(cmd, capture_output=True, text=True)
        print(result.stdout)
        if result.returncode == 0 and not is_retryable(result.stderr, result.stdout):
            time.sleep(SETTLE_DELAY)
            return True
        # Something went wrong.
        print(f"ERROR (attempt {attempt}/{retries}):", result.stderr or result.stdout)
        if attempt < retries:
            print(f"Retrying in {RETRY_DELAY}s...")
            time.sleep(RETRY_DELAY)
        else:
            print(f"Command failed after {retries} attempts.")
            time.sleep(2)
    return False


def load_or_generate_psks(psk_file: str) -> list:
    """Load channel PSKs from `psk_file`, or generate and persist them.

    Persisting means every node provisioned from the same file shares keys.
    The file holds secret key material, so it is created 0o600 and is
    git-ignored. Returns a list of raw 32-byte PSKs.
    """
    if os.path.exists(psk_file):
        psks_bytes = []
        with open(psk_file, "r") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue  # tolerate blank / trailing lines
                if ": " not in line:
                    print(f"WARNING: {psk_file}:{lineno} malformed, skipping: {line!r}")
                    continue
                try:
                    psks_bytes.append(base64.b64decode(line.split(": ", 1)[1]))
                except Exception as exc:
                    print(f"WARNING: {psk_file}:{lineno} bad base64, skipping: {exc}")
        print(f"PSKs loaded from {psk_file} ({len(psks_bytes)} keys)")
        if len(psks_bytes) != node_params.NUM_CHANNELS:
            print(
                f"WARNING: loaded {len(psks_bytes)} PSKs but NUM_CHANNELS="
                f"{node_params.NUM_CHANNELS}; channels without a key will be skipped."
            )
        return psks_bytes

    psks_bytes = [secrets.token_bytes(32) for _ in range(node_params.NUM_CHANNELS)]
    # Create restricted (0o600) up front so the secrets are never briefly
    # world-readable, then chmod again to defeat a permissive umask.
    fd = os.open(psk_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        for ch_idx, psk_bytes in enumerate(psks_bytes):
            f.write(
                f"{node_params.CHANNEL_BASE_NAME}{ch_idx}: "
                f"{base64.b64encode(psk_bytes).decode()}\n"
            )
    os.chmod(psk_file, 0o600)
    print(f"PSKs generated and written to {psk_file} (mode 0600)")
    return psks_bytes


def main() -> int:
    parser = argparse.ArgumentParser(description="Configure a Meshtastic node.")
    parser.add_argument("--port", required=True, help="Serial port of the device (e.g. /dev/ttyUSB0)")
    args = parser.parse_args()

    # Base argv for every meshtastic CLI invocation (no shell → no quoting issues).
    mesh = ["meshtastic", "--port", args.port]

    failures = []

    def step(label, cmd):
        """Run a CLI step and record the label if it ultimately fails."""
        if not run(cmd):
            failures.append(label)

    print(f"Starting node configuration on {args.port}...")

    # LoRa config: region, preset, hop limit
    step("LoRa (region/preset/hop_limit)", mesh + [
        "--set", "lora.region", node_params.LORA_REGION,
        "--set", "lora.modem_preset", node_params.LORA_PRESET,
        "--set", "lora.hop_limit", str(node_params.HOP_LIMIT),
    ])

    # Device config: rebroadcast mode and role (role varies by node position)
    step("device (rebroadcast/role)", mesh + [
        "--set", "device.rebroadcast_mode", node_params.REBROADCAST_MODE,
        "--set", "device.role", node_params.DEVICE_ROLE_CLIENT,
    ])

    # Bluetooth config
    ble = str(node_params.BLUETOOTH_ENABLE).lower()
    step("bluetooth", mesh + ["--set", "bluetooth.enabled", ble])

    # Load PSKs from file if it exists, otherwise generate and save them.
    # This ensures every node gets the same keys across separate runs.
    psk_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "channel_psks.txt")
    psks_bytes = load_or_generate_psks(psk_file)

    # Channel 0 (primary) already exists — rename it.
    # Channels 1+ must be created with --ch-add; the flag also sets the name and
    # makes subsequent --ch-set calls target the new channel automatically.
    step("channel 0 rename", mesh + [
        "--ch-index", "0", "--ch-set", "name", f"{node_params.CHANNEL_BASE_NAME}0",
    ])
    for ch_idx in range(1, node_params.NUM_CHANNELS):
        step(f"channel {ch_idx} add", mesh + ["--ch-add", f"{node_params.CHANNEL_BASE_NAME}{ch_idx}"])

    # Set PSKs via Python API — the CLI assigns the value as a str to a bytes
    # field, causing "expected bytes, str found". The API accepts bytes directly.
    try:
        iface = meshtastic.serial_interface.SerialInterface(devPath=args.port)
        try:
            for ch_idx, psk_bytes in enumerate(psks_bytes):
                iface.localNode.channels[ch_idx].settings.psk = psk_bytes
                iface.localNode.writeChannel(ch_idx)
                print(f"PSK set for channel {ch_idx}.")
                time.sleep(5)
        finally:
            iface.close()
    except Exception as exc:
        print(f"ERROR setting PSKs via API: {exc}")
        failures.append("channel PSKs (API)")

    # Telemetry config
    dev_meas = str(node_params.TELEMETRY_DEV_MEAS_ENABLED).lower()
    if node_params.TELEMETRY_DEV_MEAS_ENABLED:
        step("telemetry", mesh + [
            "--set", "telemetry.device_telemetry_enabled", dev_meas,
            "--set", "telemetry.device_update_interval", str(node_params.TELEMETRY_DEV_UPDATE_INTERVAL),
        ])
    else:
        step("telemetry", mesh + ["--set", "telemetry.device_telemetry_enabled", dev_meas])

    # Serial module config
    serial_en = str(node_params.SERIAL_MODULE_ENABLE).lower()
    if node_params.SERIAL_MODULE_ENABLE:
        step("serial module", mesh + [
            "--set", "serial.enabled", serial_en,
            "--set", "serial.mode", node_params.SERIAL_MODULE_MODE,
            "--set", "serial.txd", str(node_params.SERIAL_MODULE_TXD),
            "--set", "serial.rxd", str(node_params.SERIAL_MODULE_RXD),
            "--set", "serial.baud", node_params.SERIAL_MODULE_BAUDRATE,
            "--set", "serial.timeout", str(node_params.SERIAL_MODULE_TIMEOUT),
        ])
    else:
        step("serial module", mesh + ["--set", "serial.enabled", serial_en])

    # GPS config
    step("GPS", mesh + [
        "--set", "position.gps_mode", node_params.GPS_MODE,
        "--set", "position.gps_update_interval", str(node_params.GPS_UPDATE_INTERNAL_INTERVAL),
        "--set", "position.position_broadcast_secs", str(node_params.GPS_UPDATE_BROADCAST_INTERVAL),
    ])

    # Reboot to apply changes
    step("reboot", mesh + ["--reboot"])

    # Summary + exit status.
    if failures:
        print("\n=== Configuration completed with ERRORS ===")
        for label in failures:
            print(f"  FAILED: {label}")
        print(f"{len(failures)} step(s) failed; the node may be partially configured.")
        return 1

    print("\n=== Configuration completed successfully ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
