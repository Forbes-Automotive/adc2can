// adc2can - version history (Forbes Automotive). FW_VERSION itself is in adc2can_defs.h.
// Nothing to compile here: this file is only the list of what changed in each version.
//
// V1.4.1 - Title Case buttons; the Display options help says the dial range is saved on the board; the rails and logs
//          tables fit a phone screen (CSV / Raw / Delete stacked, no Records column on a phone); ?v= cache-busting
//          baked into index.html; the simulator has the CAN relay setting and status; README rewritten with images,
//          plug / wiring image and the "how it all links together" section. First public release.
// V1.4   - each sensor's Dial Min / Max saved on the board (blank = Auto, worked out from the scaling) and sent to
//          can2gauge (two more config parts, 32 per sensor), so a gauge page uses the same range as the adc2can's own
//          dial. can2gauge 0.36.4+ uses it. Settings from 1.3 carry over (ranges start on Auto).
// V1.3   - every CAN frame also broadcast over ESP-NOW (include/adcnow_proto.h), so a can2gauge with power only shows
//          the sensors; "Wireless to can2gauge" on the Settings tab. Later 1.3 builds added the CAN relay (the car's
//          CAN frames to gauges over WiFi, include/cannow_tx.h - shared with OpenHaldex) and moved the pin map to the
//          revhb board. Settings from 1.2 carry over.
// V1.1   - every sensor's settings shared over CAN (protocol 2: config parts, info frame, requests / sets from a
//          gauge), so can2gauge shows and edits them.
// V1.0   - first firmware: 8 sensors (linear / table / NTC), switched 5 V rails with fault handling, CAN frames,
//          web UI, data logger and trends, shared OTA.
#pragma once
