cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(firmware)

# -----------------------------------------------------------------------------
# nanopb — generate C sources from Meshtastic .proto files
#
# firmware/proto is a symlink to the Meshtastic protobuf repo:
#   CPS-RTC/Decentralized_LoRa/nordic/proto
# (same set used by the reference project; .options files are co-located)
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

set(MTS_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(NANOPB_IMPORT_DIRS ${MTS_PROTO})
set(NANOPB_GENERATE_CPP_STANDALONE OFF)

nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${MTS_PROTO}
    ${MTS_PROTO}/meshtastic/mesh.proto
    ${MTS_PROTO}/meshtastic/portnums.proto
    ${MTS_PROTO}/meshtastic/channel.proto
    ${MTS_PROTO}/meshtastic/config.proto
    ${MTS_PROTO}/meshtastic/device_ui.proto
    ${MTS_PROTO}/meshtastic/module_config.proto
    ${MTS_PROTO}/meshtastic/atak.proto
    ${MTS_PROTO}/meshtastic/telemetry.proto
    ${MTS_PROTO}/meshtastic/xmodem.proto
)

# -----------------------------------------------------------------------------
# Application sources
# -----------------------------------------------------------------------------
target_sources(app PRIVATE
    ${proto_srcs}
    ${proto_hdrs}
    src/main.c
    src/ble_gatt.c
    src/uart_meshtastic.c
    src/proto_handler.c
    src/router.c
    src/proxy_protocol.c
    src/config_cache.c
    src/upstream_session.c
)

# nanopb generates headers into the build directory
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
