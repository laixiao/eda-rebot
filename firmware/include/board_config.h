#pragma once

// 引脚与地址来自 2026-07-15 Bridge 实时网表核对（工程 AI智能机器狗_lx_改版_v2）。
// U1 IO35/36/37 禁止使用（Octal PSRAM）。

static const char *WIFI_SSID = "lx";
static const char *WIFI_PASS = "13557857840";

static const int PIN_I2C_SDA = 12;   // OLED_SDA
static const int PIN_I2C_SCL = 13;   // OLED_SCL
static const int PIN_XL9555_INT = 2; // U13 INT#

static const int PIN_ENC1_A = 9;
static const int PIN_ENC1_B = 10;
static const int PIN_ENC2_A = 21;
static const int PIN_ENC2_B = 47;

static const int PIN_I2S_MIC_SCK = 16;  // MIC_SCK
static const int PIN_I2S_MIC_WS = 17;   // MIC_WS
static const int PIN_I2S_MIC_SD = 18;   // MIC_SD

static const int PIN_I2S_AMP_LRC = 38;   // SPK_LRC
static const int PIN_I2S_AMP_BCLK = 39;  // SPK_BCLK
static const int PIN_I2S_AMP_DIN = 40;   // SPK_DIN

static const uint8_t ADDR_XL9555 = 0x20;      // U13 A0/A1/A2=GND
static const uint8_t ADDR_OLED = 0x3C;        // U10
static const uint8_t ADDR_PCA_SERVO = 0x40;   // U16 A0..A5=GND
static const uint8_t ADDR_PCA_MOTOR = 0x41;   // U23 A0=3V3

// XL9555 位：0-7=IO0_x，8-15=IO1_x
static const uint8_t XL_ENC3_A = 0;     // IO0_0 ENC3_A
static const uint8_t XL_ENC3_B = 1;     // IO0_1 ENC3_B
static const uint8_t XL_ENC4_A = 2;     // IO0_2 ENC4_A
static const uint8_t XL_ENC4_B = 3;     // IO0_3 ENC4_B
static const uint8_t XL_CAM_PWDN = 4;   // IO0_4 CAM_PWDN（默认拉高掉电）
static const uint8_t XL_STBY = 5;       // IO0_5 电机 STBY，高=使能
static const uint8_t XL_OE = 6;         // IO0_6 PCA9685 OE#，低=输出使能
static const uint8_t XL_T_CS = 8;       // IO1_0
static const uint8_t XL_LCD_LED = 9;    // IO1_1
static const uint8_t XL_LCD_DC = 10;    // IO1_2
static const uint8_t XL_LCD_RST = 11;   // IO1_3
static const uint8_t XL_LCD_CS = 12;    // IO1_4
static const uint8_t XL_T_IRQ = 13;     // IO1_5 输入
static const uint8_t XL_AMP_SD = 14;    // IO1_6 SPK_SD，高=功放开

// U16 舵机：T3~T7 = SERVO_1~5 = LED11~15
static const uint8_t SERVO_CH[5] = {11, 12, 13, 14, 15};

// U23 电机（实时网表核对，非假定）：
// MOT1 U19-A: AIN1=LED14(MOTOR1_2) AIN2=LED15(MOTOR1_1)
// MOT2 U19-B: BIN1=LED13(MOTOR2_1) BIN2=LED12(MOTOR2_2)
// MOT3 U22-A: AIN1=LED10(MOTOR3_2) AIN2=LED11(MOTOR3_1)
// MOT4 U22-B: BIN1=LED9(MOTOR4_1)  BIN2=LED8(MOTOR4_2)
// dir>0: PWM on IN1, IN2=0；dir<0: 相反。PWMA/PWMB 已接 3V3。
static const uint8_t MOTOR_IN1[4] = {14, 13, 10, 9};
static const uint8_t MOTOR_IN2[4] = {15, 12, 11, 8};

// U23 探照灯：LED0=LED_1, LED1=LED_2, LED2=LED_ALL
static const uint8_t SPOT_CH[3] = {0, 1, 2};

static const uint16_t SERVO_US_MIN = 500;
static const uint16_t SERVO_US_MAX = 2500;
static const uint16_t SERVO_US_MID = 1500;
