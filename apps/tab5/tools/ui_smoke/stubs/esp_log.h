#pragma once
#include <cstdio>
#define ESP_LOGI(tag, ...) do { std::printf("%s: ", tag); std::printf(__VA_ARGS__); std::puts(""); } while (0)
