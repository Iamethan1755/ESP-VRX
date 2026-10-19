idf_component_register(
    SRCS
        "main.c" "rf.c" "phy.c" "arc_phy.c" "transport.c" "gain.c" "receiver.c"
        "control.c" "calibration.c" "menu.c" "menu_raster.c" "analog_video_detect.c"
        "settings.c" "console.c" "profile.c" "diag.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_phy nvs_flash esp_event esp_netif esp_driver_parlio
             esp_driver_gpio esp_driver_bitscrambler esp_driver_usb_serial_jtag
             esp_driver_tsens esp_timer esp_mm hal heap esp_hw_support esp_app_format)

target_bitscrambler_add_src("u85q.bsasm")
target_bitscrambler_add_src("u85q_lp.bsasm")

# Filter capacitor and DC DAC access uses PHY internals recovered from this
# exact ESP-IDF 6.0.2 libphy; any other PHY binary builds with them refused.
set(C5_PHY_ARCHIVE "$ENV{IDF_PATH}/components/esp_phy/lib/esp32c5/libphy.a")
if(EXISTS "${C5_PHY_ARCHIVE}")
    file(SHA256 "${C5_PHY_ARCHIVE}" C5_PHY_SHA256)
    if(C5_PHY_SHA256 STREQUAL "dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE C5VRX_PHY_PINNED=1)
    else()
        message(WARNING "Unverified C5 PHY: filter and DC correction disabled (${C5_PHY_SHA256})")
    endif()
endif()
