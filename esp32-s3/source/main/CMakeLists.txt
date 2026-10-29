if(NOT CMAKE_SCRIPT_MODE_FILE)
file(GLOB_RECURSE WEB_SOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../web/*")
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../build_web_bundle.py"
          "${CMAKE_CURRENT_LIST_DIR}/../web" "${CMAKE_CURRENT_LIST_DIR}/web_bundle.bin"
  COMMAND_ERROR_IS_FATAL ANY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_SOURCE_FILES}
             "${CMAKE_CURRENT_LIST_DIR}/../build_web_bundle.py")
endif()

idf_component_register(
  SRCS "main.cpp" "runtime.cpp" "renderer.cpp" "network.cpp" "led_output.cpp" "preview.cpp" "web_files.cpp" "web_auth.cpp" "air_alert.cpp" "external_light.cpp" "buzzer.cpp"
  INCLUDE_DIRS "."
  EMBED_FILES "animation.bin" "effects.bin" "web_bundle.bin"
  REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client app_update bootloader_support esp_timer
          esp_https_ota nvs_flash espressif__cjson espressif__led_strip mbedtls esp-tls http_parser esp_driver_gpio esp_driver_ledc esp_psram joltwallet__littlefs
)
