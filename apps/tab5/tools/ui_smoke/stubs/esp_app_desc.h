#pragma once
struct esp_app_desc_t { const char *version; };
inline const esp_app_desc_t *esp_app_get_description() {
    static const esp_app_desc_t desc{"0.6.0 HOST"};
    return &desc;
}
