# ESP-IDF certificate bundle memory fix

`esp_crt_bundle.c` is ESP-IDF 6.0.1's bundled source with the exact upstream
Espressif fix applied from commit
`0d8ff68f8a47fc05a6b1d3a5bda24764a48e5fbd`.

The cross-signed CA callback formerly allocated copies of ASN.1 OIDs, values,
and subject bytes that the Mbed TLS certificate destructor did not own.
The upstream fix borrows persistent certificate-bundle and child-issuer bytes.
Allocated certificate structures and linked-list nodes keep their normal cleanup.

The root CMake file replaces only this translation unit in the mbedtls target.
It preserves certificate validation, host verification, and the cross-signed
verification callback. The installed PlatformIO SDK is not modified.

Source and patch retain Espressif's Apache-2.0 licensing.
https://github.com/espressif/esp-idf/commit/0d8ff68f8a47fc05a6b1d3a5bda24764a48e5fbd
