# Channel settings
CHANNEL_BASE_NAME = "EasterNet" 
NUM_CHANNELS = 8

# LoRa settings: region / preset / frequency slot
LORA_REGION = "ANZ"
LORA_PRESET = "LONG_FAST"

# Rebroadcast mode: only rebroadcast packets from *your* configured channels
REBROADCAST_MODE = "LOCAL_ONLY"

# Enable Bluetooth: 
BLUETOOTH_ENABLE = False

# Telemetry settings
TELEMETRY_DEV_MEAS_ENABLED = True
TELEMETRY_DEV_UPDATE_INTERVAL = 300    # [seconds]

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
GPS_MODE = "ENABLED"
GPS_UPDATE_INTERNAL_INTERVAL = 300               # [seconds]  
GPS_UPDATE_BROADCAST_INTERVAL = 600              # [seconds] (default is 0, which means 15 min.)
