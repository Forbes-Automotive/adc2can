// adc2can - ESP32-S3-WROOM-1 pin map (matches the revhb board, ProPrj_adc2can-revhb_2026-10-04: the same GPIOs since the
// rev4 "gpio" re-route of 2026-10-04. Up to 1.3.0 this held the older rev3 map, which put the battery, sensor bus,
// rail enables, CAN and LED on the wrong pins of a revhb board.)
#pragma once
#include <stdint.h>

#define NUM_CH 8

// ADC1 (the only ADC usable with WiFi on): rail monitors, sensor bus (47k/10k), battery (100k/10k).
static const uint8_t PIN_RMON[NUM_CH] = {3, 9, 10, 1, 4, 5, 6, 7}; // switched 5 V rail of sensor 1..8 (RMON1..8)
#define PIN_SBUS_MON 8                                            // sensor bus (BUCK_OUT through the SS54 isolation diode)
#define PIN_VBAT_MON 2                                            // 12 V after fuse + reverse diode

#define PIN_SDA 11 // 2x ADS1115 (0x48 ch1-4, 0x49 ch5-8) + TCA9554 (0x20) fault flags
#define PIN_SCL 12
#define PIN_CAN_TX 38 // TCAN1044V
#define PIN_CAN_RX 39

// TPS4H160A inputs via 4.7k (active high, internal pull-down: rails are OFF until the firmware enables them)
static const uint8_t PIN_EN[NUM_CH] = {14, 21, 47, 48, 15, 16, 17, 18}; // EN1..8 (sensor 1-4 = U10 IN4..IN1, 5-8 = U11 IN4..IN1)
#define PIN_DIAG_EN 13 // both TPS4H160A: high = ST# fault reporting enabled
#define PIN_EXP_INT 42 // TCA9554 #INT (open drain, 10k pull-up): a fault flag changed
#define PIN_CAN_TERM 40 // Q_TERM gate (AO3401A, R_TGU pull-up): LOW = 120R across CANH/CANL, released = off
#define PIN_STAT_LED 41 // status LED (active high)

// dividers
#define DIV_IN (21.0f / 11.0f)   // sensor input 10k / 11k -> ADS1115
#define DIV_RAIL (57.0f / 10.0f) // rail / sensor bus monitors 47k / 10k -> S3 ADC (12 V back-feed reads 2.1 V)
#define DIV_VBAT (110.0f / 10.0f) // battery monitor 100k / 10k -> S3 ADC

#define ADS_ADDR_LO 0x48 // ADDR = GND: ch1-4 on AIN0-3
#define ADS_ADDR_HI 0x49 // ADDR = VDD: ch5-8 on AIN0-3
#define EXP_ADDR 0x20    // TCA9554 P0-P7 = ST# of channel 1-8 (low = fault)
