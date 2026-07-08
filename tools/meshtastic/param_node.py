# Channel settings
CHANNEL_BASE_NAME = "EasterNet" 
NUM_CHANNELS = 8

# LoRa settings: region / preset / frequency slot
LORA_REGION = "ANZ"
# MEDIUM_FAST: much higher air data rate than LONG_FAST -> far less airtime per
# packet and lower end-to-end latency (trade-off: shorter range). Switched from
# LONG_FAST after latency analysis showed multi-second, growing mesh delay.
LORA_PRESET = "LONG_TURBO"

# Rebroadcast mode: only rebroadcast packets from *your* configured channels
REBROADCAST_MODE = "LOCAL_ONLY"

# Enable Bluetooth: 
BLUETOOTH_ENABLE = False

# Telemetry settings
# device_update_interval: 0 = firmware default (frequent). Set explicitly to
# 15 min so device metrics don't flood the client/mesh. 900 s = 15 min.
TELEMETRY_DEV_MEAS_ENABLED = True
TELEMETRY_DEV_UPDATE_INTERVAL = 900    # [seconds] = 15 min

# Serial settings: 
SERIAL_MODULE_ENABLE = True
SERIAL_MODULE_MODE = "PROTO"
# We use U1TXD --> GPIO15 and U1RXD --> GPIO35 which are UART_DEV(1) routed pins for ESP32 chip. 
# USB Console: UART_DEV(0)
# GPS:         UART_DEV(2)
SERIAL_MODULE_TXD = 15
SERIAL_MODULE_RXD = 35
SERIAL_MODULE_BAUDRATE = "BAUD_115200"
SERIAL_MODULE_TIMEOUT = 20             # [mili-seconds]


# Sensing node role choice
DEVICE_ROLE_CLIENT = "CLIENT"

# Hop limit: must be (required_hops_to_gateway + 1)
REQUIRED_HOPS_TO_GATEWAY = 2            # <-- set this per node (e.g., node1=2, node2=1, node3=1)
HOP_LIMIT = REQUIRED_HOPS_TO_GATEWAY + 1

# GPS settings (optional)
# 0 = firmware default (broadcast default is ~15 min). Set explicitly:
#   gps_update_interval    (local fix cadence, no mesh airtime)
#   position_broadcast_secs(over-air position broadcast) -> 30 min
GPS_MODE = "ENABLED"
GPS_UPDATE_INTERNAL_INTERVAL = 1800              # [seconds] = 30 min (local GPS fix)
GPS_UPDATE_BROADCAST_INTERVAL = 1800             # [seconds] = 30 min (mesh broadcast)
