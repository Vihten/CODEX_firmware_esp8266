/* Stable across OTA slots. The bootloader must not load or zero this mailbox. */
ota_rejection_mailbox = 0x50001f80;
ASSERT(_rtc_force_slow_end <= ota_rejection_mailbox, "RTC slow data overlaps OTA rejection mailbox")
ASSERT(ota_rejection_mailbox >= ORIGIN(rtc_slow_seg) &&
       ota_rejection_mailbox + 128 == ORIGIN(rtc_slow_seg) + LENGTH(rtc_slow_seg),
       "OTA rejection mailbox outside RTC slow memory")
