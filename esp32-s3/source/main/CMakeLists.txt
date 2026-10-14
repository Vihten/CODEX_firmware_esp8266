idf_component_register(
  SRCS "main.cpp" "runtime.cpp" "renderer.cpp" "network.cpp" "led_output.cpp"
  INCLUDE_DIRS "."
  EMBED_FILES "animation.bin" "effects.bin"
  REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client app_update
          esp_https_ota nvs_flash espressif__cjson espressif__led_strip mbedtls esp_driver_gpio esp_psram
)
