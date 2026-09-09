#ifndef BMI088_H
#define BMI088_H

#include "iface/device.h"
#include <stdint.h>

/* device-level control commands for the BMI088 driver */
#define BMI088_IOCTL_GET_WHO 0x01 /* arg: bmi088_who_t*  — 两芯片 WHO_AM_I 回读 */
#define BMI088_IOCTL_GET_RAW 0x02 /* arg: bmi088_raw_t*  — 加速度/陀螺原始 16bit 计数值 */
#define BMI088_IOCTL_GET_SI  0x03 /* arg: bmi088_si_t*   — 换算 SI：accel m/s² + gyro rad/s */

/* 芯片标识回读（模拟器/真机：ACCEL=0x1E、GYRO=0x0F） */
typedef struct {
    uint8_t accel;
    uint8_t gyro;
} bmi088_who_t;

/* 原始计数值（16bit 有符号，BMI088 灵敏度：accel ±3g=10920 LSB/g、gyro ±2000dps=16.4 LSB/dps） */
typedef struct {
    int16_t accel[3]; /* X/Y/Z */
    int16_t gyro[3];  /* X/Y/Z */
} bmi088_raw_t;

/* 换算 SI：accel m/s²（悬停 z≈+9.81）、gyro rad/s（悬停 ≈0） */
typedef struct {
    float accel[3];
    float gyro[3];
} bmi088_si_t;

/*
 * BMI088 双片选六轴 IMU（SPI 主 + 两个 GPIO 片选）。
 *
 * SPI 寄存器（7bit 地址，帧首字节 = reg<<1 | rw）：
 *   ACCEL WHO_AM_I 0x00=0x1E，数据 ACC_X_L 0x12..ACC_Z_H 0x17（6B, 16bit LE）
 *   GYRO  WHO_AM_I 0x00=0x0F，数据 GYR_X_L 0x02..GYR_Z_H 0x07（6B, 16bit LE）
 * 片选：拉低 ACCEL_CS 访问加速度计文件，拉低 GYRO_CS 访问陀螺仪文件。
 *
 * 实现统一 device 接口：open 做 WHO_AM_I 校验（失败即驱动层拒绝），
 * read 返回 12B 原始计数值（accel 6B + gyro 6B，LE），ioctl 取 WHO/RAW/SI。
 */
typedef struct _bmi088 bmi088;

/* 板级数据：设备名 + 依赖的 SPI 总线与两路片选 GPIO（device 名） */
typedef struct {
    const char *name;
    const char *spi;      /* e.g. "spi2"（SPI3 @ PB3/4/5） */
    const char *accel_cs; /* e.g. "bmi_accel_cs"（GPIOE_7 输出，默认高=不选中） */
    const char *gyro_cs;  /* e.g. "bmi_gyro_cs"（GPIOE_8 输出，默认高=不选中） */
} bmi088_config_t;

/* 工厂：读 config、解析依赖设备名（open 时解析并拉起依赖） */
device *bmi088_create(const void *config);

#endif /* BMI088_H */
