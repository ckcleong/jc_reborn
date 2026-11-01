/*
 * Setup mode: the device hosts an access point and a web page for
 * setting the wall clock, then restarts into the story with the time
 * set (and persisted in NVS, so reboots restore it).
 *
 * Flow:
 *   - no valid clock in NVS (or the user held the boot button) ->
 *     panel shows a QR code encoding the WIFI-join URI for the setup
 *     access point (open network, captive DNS): a phone camera scan
 *     joins the AP and the OS pops the portal page automatically,
 *   - the page (http://192.168.4.1/) prefills the phone's own clock,
 *   - POST /set applies it: settimeofday + NVS {epoch, tzmin} +
 *     restart -> the story starts with the clock set.
 *
 * Without a real-time clock the time survives reboots via the NVS
 * snapshot (written every 10 minutes by the pacing shim in
 * platform_glue.c), so expect the clock to be late by the duration of
 * a power cut plus up to 10 minutes.
 * Re-enter setup any time: hold the boot button ~5 s.
 */
#pragma once

// Returns 1 when the device should enter setup mode (no stored time /
// stored time invalid), after restoring the clock otherwise.
int setup_needed(void);

// Restore the clock from NVS (call at every boot before the story).
void setup_restore_time(void);

// 1 when a plausible clock is stored (the story then follows it
// instead of the pinned baseline date).
int setup_clock_valid(void);

// The "Sound enabled" checkbox from the last setup (default: on).
int setup_sound_enabled(void);

// Run the setup mode (softAP + portal + QR screen); restarts the
// device after the clock is set. Never returns. (Task signature: run
// via xTaskCreate — WiFi init is too stack-deep for app_main.)
void setup_run(void *arg);

// Story-time NVS snapshot: the pacing shim calls this periodically so
// the clock survives power cuts (minus the gap + up to 10 minutes).
void setup_snapshot_time(void);

// The boot button (BOARD_BTN_GPIO) held low for ~5 s requests setup
// mode on the next boot. Polled from a low-priority task by main.c.
int setup_button_held(void);

// Flag setup mode for the next boot (the story's button task calls
// this before esp_restart()).
void setup_request(void);
