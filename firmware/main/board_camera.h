#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool board_camera_init(void);
bool board_camera_deinit(void);
bool board_camera_ready(void);

/** Capture one JPEG frame. Returns pointer to JPEG data (caller must free with board_camera_fb_return). */
bool board_camera_capture_jpeg(uint8_t **jpg_buf, size_t *jpg_len);
void board_camera_fb_return(void);

/** Set resolution: 0=QVGA(320x240), 1=VGA(640x480), 2=SVGA(800x600), 3=XGA(1024x768), 4=SXGA(1280x1024) */
bool board_camera_set_resolution(int level);
int  board_camera_get_resolution(void);

#ifdef __cplusplus
}
#endif
