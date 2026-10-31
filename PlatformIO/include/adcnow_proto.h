#pragma once
// ---------------------------------------------------------------------------
// ADCN - adc2can over ESP-NOW (wire format v1). SHARED FILE: identical copies live in
//   can2gauge/include/adcnow_proto.h  and  adc2can/include/adcnow_proto.h
// Change both together and bump ADCN_PROTO on any layout change.
//
// The adc2can broadcasts the very CAN frames it sends (id = canBaseId + board * 16 + n, see the adc2can CAN protocol):
// values, connector volts, status, names / units, its config parts and the info frame. Everything sent in one pass of
// its loop goes out as one packet, so a gauge with power only - no CAN - decodes them exactly as if heard on the bus
// (can2gauge_ADC2CAN.cpp), names, units and the shared config included.
//   - One-way: config requests / edits from a gauge (n 12 / 13) still need CAN. Nothing over the air changes a setting.
//   - Same WiFi channel needed: both APs default to channel 1. A Home WiFi bridge on either side, or the gauge joining
//     a WiFi OBD adapter, moves that side's channel - the gauge then stops hearing the adc2can until they match again.
//   - A gauge that also hears the board on CAN uses CAN and ignores these.
// ---------------------------------------------------------------------------
#include <stdint.h>

#define ADCN_MAGIC0 'A'
#define ADCN_MAGIC1 'D'
#define ADCN_PROTO 1
#define ADCN_MAX_FRAMES 24 // per packet

struct __attribute__((packed)) AdcnHdr
{
  uint8_t magic[2];
  uint8_t proto;  // ADCN_PROTO
  uint8_t board;  // the adc2can's board id 0-3 (CAN: id offset board * 16)
  uint8_t seq;    // +1 per packet
  uint8_t count;  // AdcnFrame entries that follow
  uint8_t fw[2];  // firmware major, minor
};

struct __attribute__((packed)) AdcnFrame
{
  uint8_t n;    // frame offset 0-15 (CAN id - base - board * 16)
  uint8_t d[8]; // the CAN frame's 8 data bytes, unchanged
};

static_assert(sizeof(AdcnHdr) == 8 && sizeof(AdcnFrame) == 9, "ADCN layout");
static_assert(sizeof(AdcnHdr) + ADCN_MAX_FRAMES * sizeof(AdcnFrame) <= 250, "ADCN packet must fit one ESP-NOW v1 packet");
