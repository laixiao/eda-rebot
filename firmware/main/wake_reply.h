#pragma once

#include <stddef.h>
#include <stdint.h>

/** 已嵌入的唤醒回复条数（firmware/assets 下 wav）。 */
size_t wake_reply_count();

/**
 * 随机解码一条唤醒回复为 16 kHz mono PCM16。
 * 成功时 *pcm 由调用方 free；*name 指向静态文件名。
 */
bool wake_reply_decode_random(int16_t **pcm, size_t *n_samples, const char **name);
