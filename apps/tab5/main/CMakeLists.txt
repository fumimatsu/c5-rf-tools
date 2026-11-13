idf_component_register(SRCS "main.cpp" "ui.cpp" "rf_self_test.cpp" "fonts/rf_jp20.c" "fonts/rf_jp24.c" INCLUDE_DIRS "." REQUIRES m5stack_tab5 usb_host_cdc_acm esp_timer nvs_flash)
