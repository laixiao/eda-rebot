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
/** 读 XL9555 口上的 PWDN/RST 电平（开漏脚：低=XL 拉低，高=释放靠上拉） */
bool cameraCtrlLevels(XL9555 &xl, bool &pwdnHigh, bool &rstHigh);
/** 只做上电+复位并保持 PWDN 低，不跑 esp_camera_init（方便万用表排查） */
bool cameraHoldPower(XL9555 &xl);
