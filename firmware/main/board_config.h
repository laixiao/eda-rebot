#pragma once

#include <stdint.h>
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#error "main/wifi_config.h required before build/flash/OTA — copy wifi_config.example.h and set SSID/PASS"
#endif

// 引脚与地址：AI通用机器人_v5 Bridge 实时网表（2026-08-02 / 2026-08-20）
// Board V1.0.0 / 图页 1d774ca900623155
// U1 IO35/36/37 禁止使用（Octal PSRAM）。
// 固件：FAN-Circle 功能（语音/风扇/手势/雷达增强）跑在 v5 硬件上。

static const int PIN_I2C_SDA = 12;   // OLED_SDA
static const int PIN_I2C_SCL = 13;   // OLED_SCL
static const int PIN_XL9555_INT = 2; // U13 INT#

static const int PIN_ENC1_A = 9;
static const int PIN_ENC1_B = 10;
static const int PIN_ENC2_A = 21;
static const int PIN_ENC2_B = 47;

// MS60 飞线：占 ENC1 作 UART；GPIO-OUT→ENC3_A(XL IO0_0)
// v5：ESP RX←雷达 TX=IO9；ESP TX→雷达 RX=IO10（无板载电源 MOSFET）
static const int PIN_RADAR_UART_RX = 9;  // ENC1_A
static const int PIN_RADAR_UART_TX = 10; // ENC1_B
static const uint8_t XL_RADAR_OUT = 0;   // ENC3_A = XL9555 IO0_0
// v5 无雷达供电脚：IO0_1=ENC3_B，禁止当电源使能用
#ifndef BOARD_HAS_RADAR_PWR
#define BOARD_HAS_RADAR_PWR 0
#endif

static const int PIN_I2S_MIC_SCK = 16;
static const int PIN_I2S_MIC_WS = 17;
static const int PIN_I2S_MIC_SD = 18;

static const int PIN_I2S_AMP_LRC = 38;
static const int PIN_I2S_AMP_BCLK = 39;
static const int PIN_I2S_AMP_DIN = 40;

// 摄像头：DC-5M21-5640-B（OV5640）；U7=CAM_2V8、U8=CAM_1V5；SCCB 上拉到 2.8V
// PWDN/RESETB 经 XL9555 开漏控制（只拉低，高电平靠 10k→CAM_2V8）
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
static const uint8_t ADDR_PCA_SERVO = 0x40; // U16 舵机
static const uint8_t ADDR_PCA_MOTOR = 0x41; // U23 电机方向 + 探照灯
// 兼容旧单 PCA 符号名（指向舵机地址；探照灯/风扇必须用 pcaMotor）
static const uint8_t ADDR_PCA9685 = ADDR_PCA_SERVO;

static const uint8_t XL_ENC3_A = 0;
static const uint8_t XL_ENC3_B = 1;
static const uint8_t XL_ENC4_A = 2;
static const uint8_t XL_ENC4_B = 3;
static const uint8_t XL_CAM_PWDN = 4; // IO0_4，开漏：低=工作，高阻=掉电(R22→2.8V)
static const uint8_t XL_STBY = 5;
static const uint8_t XL_OE = 6;
static const uint8_t XL_CAM_RST = 7;  // IO0_7，开漏：低=复位，高阻=释放(R21→2.8V)
static const uint8_t XL_T_CS = 8;
static const uint8_t XL_LCD_LED = 9;
static const uint8_t XL_LCD_DC = 10;
static const uint8_t XL_LCD_RST = 11;
static const uint8_t XL_LCD_CS = 12;
static const uint8_t XL_T_IRQ = 13;
static const uint8_t XL_AMP_SD = 14;

static const uint8_t SERVO_CH[5] = {11, 12, 13, 14, 15};
static const uint8_t SERVO_COUNT = 5;
static const uint8_t MOTOR_IN1[4] = {14, 13, 10, 9};
static const uint8_t MOTOR_IN2[4] = {15, 12, 11, 8};
// API id 0/1/2 → U23 LED0/1/2 = LED_1 / LED_2 / LED_ALL（风扇控 id0×id2）
static const uint8_t SPOT_CH[3] = {0, 1, 2};
static const uint8_t SPOT_COUNT = 3;

static const uint16_t SERVO_US_MIN = 500;
static const uint16_t SERVO_US_MAX = 2500;
static const uint16_t SERVO_US_MID = 1500;

static const uint16_t LCD_WIDTH = 320;
static const uint16_t LCD_HEIGHT = 480;
