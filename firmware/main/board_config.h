#pragma once

#include <stdint.h>
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

// 引脚与地址来自 2026-07-18 Bridge 实时网表
// 工程 AI通用机器狗_v4 / Board V1.0.0 / 图页 1d774ca900623155
// U1 IO35/36/37 禁止使用（Octal PSRAM）。

static const int PIN_I2C_SDA = 12;   // OLED_SDA
static const int PIN_I2C_SCL = 13;   // OLED_SCL
static const int PIN_XL9555_INT = 2; // U13 INT#

static const int PIN_ENC1_A = 9;
static const int PIN_ENC1_B = 10;
static const int PIN_ENC2_A = 21;
static const int PIN_ENC2_B = 47;

static const int PIN_I2S_MIC_SCK = 16;
static const int PIN_I2S_MIC_WS = 17;
static const int PIN_I2S_MIC_SD = 18;

static const int PIN_I2S_AMP_LRC = 38;
static const int PIN_I2S_AMP_BCLK = 39;
static const int PIN_I2S_AMP_DIN = 40;

static const int PIN_CAM_XCLK = 15;
static const int PIN_CAM_PCLK = 1;
static const int PIN_CAM_VSYNC = 6;
static const int PIN_CAM_HREF = 7;
static const int PIN_CAM_SDA = 4;
static const int PIN_CAM_SCL = 5;
static const int PIN_CAM_D0 = 11;
static const int PIN_CAM_D1 = 14;
static const int PIN_CAM_D2 = 8;
static const int PIN_CAM_D3 = 20;
static const int PIN_CAM_D4 = 41;
static const int PIN_CAM_D5 = 42;
static const int PIN_CAM_D6 = 48;
static const int PIN_CAM_D7 = 19;

static const int PIN_LCD_SCK = 45;
static const int PIN_LCD_MOSI = 46;
static const int PIN_LCD_MISO = 3;

static const uint8_t ADDR_XL9555 = 0x20;
static const uint8_t ADDR_OLED = 0x3C;
static const uint8_t ADDR_PCA_SERVO = 0x40;
static const uint8_t ADDR_PCA_MOTOR = 0x41;

static const uint8_t XL_ENC3_A = 0;
static const uint8_t XL_ENC3_B = 1;
static const uint8_t XL_ENC4_A = 2;
static const uint8_t XL_ENC4_B = 3;
static const uint8_t XL_CAM_PWDN = 4;
static const uint8_t XL_STBY = 5;
static const uint8_t XL_OE = 6;
static const uint8_t XL_T_CS = 8;
static const uint8_t XL_LCD_LED = 9;
static const uint8_t XL_LCD_DC = 10;
static const uint8_t XL_LCD_RST = 11;
static const uint8_t XL_LCD_CS = 12;
static const uint8_t XL_T_IRQ = 13;
static const uint8_t XL_AMP_SD = 14;

static const uint8_t SERVO_CH[5] = {11, 12, 13, 14, 15};
static const uint8_t MOTOR_IN1[4] = {14, 13, 10, 9};
static const uint8_t MOTOR_IN2[4] = {15, 12, 11, 8};
static const uint8_t SPOT_CH[3] = {0, 1, 2};

static const uint16_t SERVO_US_MIN = 500;
static const uint16_t SERVO_US_MAX = 2500;
static const uint16_t SERVO_US_MID = 1500;

static const uint16_t LCD_WIDTH = 320;
static const uint16_t LCD_HEIGHT = 480;
