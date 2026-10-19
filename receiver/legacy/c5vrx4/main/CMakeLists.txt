idf_component_register(
    SRCS
        "main.c"
        "bs_relative_worker_probe.c"
        "bs_relative_middle_probe.c"
        "phy_phase_tap_probe.c"
        "bs_addctia_probe.c"
        "arc_phy.c"
        "arc_v3_controller.c"
        "arc_v5_autotune.c"
        "rx_auto_lab.c"
        "rf.c"
        "phy_rx_lab.c"
        "video.c"
        "direct_gain.c"
        "direct_gain_v2.c"
        "direct_gain_v3.c"
        "analog_video_detect.c"
        "menu_raster.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_phy
        nvs_flash
        esp_event
        esp_netif
        esp_driver_parlio
        esp_driver_gpio
        esp_driver_bitscrambler
        esp_driver_usb_serial_jtag
        vfs
        esp_rom
        esp_mm
        esp_timer
        freertos
        heap
        hal
        esp_hw_support
        esp_driver_gptimer
)

# Proven default plus opt-in experimental demod/output paths.
target_bitscrambler_add_src("fm.bsasm")
target_bitscrambler_add_src("fm_relative_golden.bsasm")
target_bitscrambler_add_src("fm_phase5_360.bsasm")
target_bitscrambler_add_src("bs_relative_worker_probe.bsasm")
target_bitscrambler_add_src("bs_relative_middle_probe.bsasm")
target_bitscrambler_add_src("fm_phase5_fsm_capture.bsasm")
target_bitscrambler_add_src("fm4.bsasm")
target_bitscrambler_add_src("bs_addctia_probe.bsasm")
target_bitscrambler_add_src("fm_phase8_hr_live.bsasm")
target_bitscrambler_add_src("fm_hc.bsasm")
if(C5VRX4_EXPERIMENT)
    include("${PROJECT_DIR}/component.cmake")
endif()

# Recovered MMIO overrides are valid only for this exact C5 archive. Unknown
# library versions retain observation but refuse all lab writes/I2C helpers.
set(C5_PHY_ARCHIVE "$ENV{IDF_PATH}/components/esp_phy/lib/esp32c5/libphy.a")
if(EXISTS "${C5_PHY_ARCHIVE}")
    file(SHA256 "${C5_PHY_ARCHIVE}" C5_PHY_SHA256)
    if(C5_PHY_SHA256 STREQUAL "dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE C5VRX_PHY_RX_LAB_PINNED=1)
    else()
        message(WARNING "Unverified C5 PHY: RX lab writes disabled (${C5_PHY_SHA256})")
    endif()
endif()
