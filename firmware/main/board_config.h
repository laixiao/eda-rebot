#pragma once

#include <stdint.h>
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#error "main/wifi_config.h required before build/flash/OTA — copy wifi_config.example.h and set SSID/PASS"
#endif

// 引脚与地址：2026-08-03 Bridge 实时网表
// 工程 AI通用机器人_v6-1 / Board V1.0.0 / 图页 1d774ca900623155
// U1 IO35/36/37 禁止使用（Octal PSRAM）。

static const int PIN_I2C_SDA = 12;   // OLED_SDA
static const int PIN_I2C_SCL = 13;   // OLED_SCL
static const int PIN_XL9555_INT = 2; // U13 INT#

// 60G 雷达 MS60：板载座 U6 UART / U7 电源+OUT
// ESP RX ← 雷达 TX；ESP TX → 雷达 RX
static const int PIN_RADAR_UART_RX = 9;  // Radar-RX
static const int PIN_RADAR_UART_TX = 10; // Radar-TX
static const uint8_t XL_RADAR_OUT = 0;   // IO0_0 ← Radar-OUT
static const uint8_t XL_RADAR_PWR = 1;   // IO0_1 → Q4（低=开 3V3 供电）

static const int PIN_I2S_MIC_SCK = 16;
static const int PIN_I2S_MIC_WS = 17;
static const int PIN_I2S_MIC_SD = 18;

static const int PIN_I2S_AMP_LRC = 38;
static const int PIN_I2S_AMP_BCLK = 39;
static const int PIN_I2S_AMP_DIN = 40;

// 扩展排针 U4（未占用时可悬空）
static const int PIN_EXP_IO4 = 4;
static const int PIN_EXP_IO5 = 5;
static const int PIN_EXP_IO6 = 6;
static const int PIN_EXP_IO7 = 7;
static const int PIN_EXP_IO41 = 41;
static const int PIN_EXP_IO42 = 42;

static const uint8_t ADDR_XL9555 = 0x20;
static const uint8_t ADDR_OLED = 0x3C;
static const uint8_t ADDR_PCA9685 = 0x40;

static const uint8_t XL_OE = 6;      // IO0_6 → U16 OE#（高=禁 PWM）
static const uint8_t XL_AMP_SD = 14; // IO1_6 → MAX98357 SD（高=开）

// U16：舵机 T3/T4 = LED11/12；探照灯 id0..2 = LED_1/LED_2/LED_ALL
static const uint8_t SERVO_CH[2] = {11, 12};
static const uint8_t SERVO_COUNT = 2;
// API id 0/1/2 → PCA ch 1/2/0（LED_1 / LED_2 / LED_ALL）
static const uint8_t SPOT_CH[3] = {1, 2, 0};
static const uint8_t SPOT_COUNT = 3;
// 扩展 PWM → U8：LED3/4/5
static const uint8_t EXP_LED_CH[3] = {3, 4, 5};

static const uint16_t SERVO_US_MIN = 500;
static const uint16_t SERVO_US_MAX = 2500;
static const uint16_t SERVO_US_MID = 1500;
