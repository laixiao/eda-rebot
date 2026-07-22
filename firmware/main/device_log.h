#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

void device_log_init();
std::string device_log_json(uint64_t after, size_t limit);
