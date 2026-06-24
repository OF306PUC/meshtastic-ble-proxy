import argparse
import subprocess
import time
import secrets
import base64
import os
import sys

import meshtastic.serial_interface

# Make `import param_node` resolve regardless of the current working directory,
# so this script can be run from anywhere (not just tools/meshtastic/).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import param_node as node_params

MAX_RETRIES = 3
RETRY_DELAY = 10  # seconds to wait before retrying

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

def run(cmd, retries=MAX_RETRIES):
    print(f"\nRunning: {cmd}")
    for attempt in range(1, retries + 1):
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        print(result.stdout)
        if result.returncode == 0 and not is_retryable(result.stderr, result.stdout):
            time.sleep(10)
            return  # success
        # Something went wrong
        print(f"ERROR (attempt {attempt}/{retries}):", result.stderr or result.stdout)
        if attempt < retries:
            print(f"Retrying in {RETRY_DELAY}s...")
            time.sleep(RETRY_DELAY)
        else:
            print(f"Command failed after {retries} attempts. Continuing...")
            time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description="Configure a Meshtastic node.")
    parser.add_argument("--port", required=True, help="Serial port of the device (e.g. /dev/ttyUSB0)")
    args = parser.parse_args()
    p = f"--port {args.port}"

    print(f"Starting node configuration on {args.port}...")

    # LoRa config: region, preset, hop limit
    run(
        f"meshtastic {p} --set lora.region {node_params.LORA_REGION}"
        f" --set lora.modem_preset {node_params.LORA_PRESET}"
        f" --set lora.hop_limit {node_params.HOP_LIMIT}"
    )

    # Device config: rebroadcast mode and role (role varies by node position)
    run(
        f"meshtastic {p} --set device.rebroadcast_mode {node_params.REBROADCAST_MODE}"
        f" --set device.role {node_params.DEVICE_ROLE_CLIENT}"
    )

    # Bluetooth config
    BLE = str(node_params.BLUETOOTH_ENABLE).lower()    
    run(f"meshtastic {p} --set bluetooth.enabled {BLE}")

    # Load PSKs from file if it exists, otherwise generate and save them.
    # This ensures both devices get the same keys across separate runs.
    psk_file = os.path.join(os.path.dirname(__file__), "channel_psks.txt")
    if os.path.exists(psk_file):
        with open(psk_file, "r") as f:
            psks_bytes = [base64.b64decode(line.split(": ")[1].strip()) for line in f]
        print(f"PSKs loaded from {psk_file}")
    else:
        psks_bytes = [secrets.token_bytes(32) for _ in range(node_params.NUM_CHANNELS)]
        with open(psk_file, "w") as f:
            for ch_idx, psk_bytes in enumerate(psks_bytes):
                f.write(f"{node_params.CHANNEL_BASE_NAME}{ch_idx}: {base64.b64encode(psk_bytes).decode()}\n")
        print(f"PSKs generated and written to {psk_file}")

    # Channel 0 (primary) already exists — rename it.
    # Channels 1+ must be created with --ch-add; the flag also sets the name and
    # makes subsequent --ch-set calls target the new channel automatically.
    run(f'meshtastic {p} --ch-index 0 --ch-set name {node_params.CHANNEL_BASE_NAME}0')
    for ch_idx in range(1, node_params.NUM_CHANNELS):
        run(f'meshtastic {p} --ch-add {node_params.CHANNEL_BASE_NAME}{ch_idx}')

    # Set PSKs via Python API — CLI assigns the value as a str to a bytes field,
    # causing "expected bytes, str found". The API accepts bytes directly.
    iface = meshtastic.serial_interface.SerialInterface(devPath=args.port)
    try:
        for ch_idx, psk_bytes in enumerate(psks_bytes):
            iface.localNode.channels[ch_idx].settings.psk = psk_bytes
            iface.localNode.writeChannel(ch_idx)
            print(f"PSK set for channel {ch_idx}.")
            time.sleep(5)
    finally:
        iface.close()

    # Telemetry config
    DEV_MEAS = str(node_params.TELEMETRY_DEV_MEAS_ENABLED).lower()
    if node_params.TELEMETRY_DEV_MEAS_ENABLED: 
        run(
            f"meshtastic {p} --set telemetry.device_telemetry_enabled {DEV_MEAS}"
            f" --set telemetry.device_update_interval {node_params.TELEMETRY_DEV_UPDATE_INTERVAL}"
        )
    else: 
        run(f"meshtastic {p} --set telemetry.device_telemetry_enabled {DEV_MEAS}")


    # Serial module config
    SERIAL_EN = str(node_params.SERIAL_MODULE_ENABLE).lower()
    if node_params.SERIAL_MODULE_ENABLE:
        run(
            f"meshtastic {p} --set serial.enabled {SERIAL_EN}"
            f" --set serial.mode {node_params.SERIAL_MODULE_MODE}"
            f" --set serial.txd {node_params.SERIAL_MODULE_TXD}"
            f" --set serial.rxd {node_params.SERIAL_MODULE_RXD}"
            f" --set serial.baud {node_params.SERIAL_MODULE_BAUDRATE}"
            f" --set serial.timeout {node_params.SERIAL_MODULE_TIMEOUT}"
        )
    else:
        run(f"meshtastic {p} --set serial.enabled {SERIAL_EN}")

    # GPS config
    run(
        f"meshtastic {p} --set position.gps_mode {node_params.GPS_MODE}"
        f" --set position.gps_update_interval {node_params.GPS_UPDATE_INTERNAL_INTERVAL}"
        f" --set position.position_broadcast_secs {node_params.GPS_UPDATE_BROADCAST_INTERVAL}"
    )

    # Reboot to apply changes
    run(f"meshtastic {p} --reboot")


if __name__ == "__main__":
    main()
