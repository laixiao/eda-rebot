#pragma once

#include <stdint.h>
#include <stddef.h>

/** Binary search CJK glyph; returns 32-byte column-major bitmap or nullptr. */
const uint8_t *font_cjk_lookup(uint32_t codepoint);
size_t font_cjk_count();
