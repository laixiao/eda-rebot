#pragma once

#include "xl9555.h"
#include <stdint.h>
#include <stddef.h>

bool cameraPower(XL9555 &xl, bool on);
bool cameraBegin(XL9555 &xl);
bool cameraOk();
void cameraEnd(XL9555 &xl);
bool cameraCaptureJpeg(uint8_t *&buf, size_t &len);
void cameraReleaseFrame();
