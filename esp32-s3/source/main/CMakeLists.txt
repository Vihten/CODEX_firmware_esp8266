if(NOT CMAKE_SCRIPT_MODE_FILE)
file(GLOB_RECURSE WEB_SOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../web/*")
file(GLOB_RECURSE PORTAL_SOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../portal/*")
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../build_web_bundle.py"
          "${CMAKE_CURRENT_LIST_DIR}/../web" "${CMAKE_CURRENT_LIST_DIR}/web_bundle.bin"
  COMMAND_ERROR_IS_FATAL ANY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_SOURCE_FILES} ${PORTAL_SOURCE_FILES}
             "${CMAKE_CURRENT_LIST_DIR}/../build_web_bundle.py")
endif()

idf_component_register(
  SRCS "main.cpp" "runtime.cpp" "renderer.cpp" "network.cpp" "network_memory.cpp" "daily_runtime.cpp" "led_output.cpp" "preview.cpp" "web_files.cpp" "web_auth.cpp" "air_alert.cpp" "external_light.cpp" "buzzer.cpp" "diagnostics.cpp" "diagnostic_http.cpp" "diagnostic_archive.cpp" "diagnostic_persistence.cpp" "json_memory.cpp"
  INCLUDE_DIRS "."
  EMBED_FILES "animation.bin" "effects.bin" "claude_animation.bin" "claude_effects.bin" "web_bundle.bin"
  REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client app_update bootloader_support esp_timer
          esp_https_ota nvs_flash espressif__cjson mbedtls esp-tls http_parser esp_driver_gpio esp_driver_ledc esp_driver_rmt esp_psram joltwallet__littlefs
)

# The renderer must prepare the next 1000-LED frame inside a 37 ms transport budget (27 Hz).
# Optimize rendering and the RMT encoder while retaining normal assertions.
set_source_files_properties(renderer.cpp led_output.cpp PROPERTIES COMPILE_OPTIONS "-O2")
