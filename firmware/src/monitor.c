// MIT License
//
// Copyright (c) 2026 Kevin Thomas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Author:  Kevin Thomas
// Email:   kevin@mytechnotalent.com
// GitHub:  https://github.com/mytechnotalent/picokit-31-thermostat
// File:    monitor.c
// Desc:    Implements the hysteresis thermostat state machine that drives the
//          SG90 vent with a remote-adjustable setpoint.
// Created: 2026

#include "picokit_31_thermostat.h"
#include "monitor.h"
#include "radio.h"
#include "status_led.h"
#include "servo.h"
#include "ir_remote.h"
#include "sensor.h"
#include "ccm.h"
#include "envelope.h"
#include "field_secrets.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Module-ready flag.
 *
 * Set to true by monitor_init() once the peripherals are configured.
 * monitor_step() returns false while this flag is clear.
 */
static bool g_ready;

/**
 * @brief Current setpoint in tenths of a degree Celsius.
 */
static int16_t g_setpoint;

/**
 * @brief Most recent temperature in tenths of a degree Celsius.
 */
static int16_t g_temperature;

/**
 * @brief Current vent angle in degrees.
 */
static uint8_t g_vent;

/**
 * @brief True once a valid temperature reading has been seen.
 */
static bool g_valid;

/**
 * @brief Monotonic transmit sequence number.
 */
static uint16_t g_seq;

/**
 * @brief Absolute time in microseconds of the next DHT11 sample.
 */
static uint64_t g_next_read_us;

/**
 * @brief Absolute time in microseconds of the next authenticated transmit.
 */
static uint64_t g_next_tx_us;

/**
 * @brief Most recent decoded DHT11 reading.
 */
static dht_reading_t g_reading;

/**
 * @brief Inbound radio line accumulator.
 */
static char g_rx_line[RADIO_LINE_BUF_LEN];

/**
 * @brief Number of bytes currently held in the inbound line accumulator.
 */
static size_t g_rx_len;

/**
 * @brief AES-128 session key for telemetry.
 */
static uint8_t g_key[CCM_KEY_LEN];

/**
 * @brief True once the telemetry session key has been loaded.
 */
static bool g_key_ready;

/**
 * @brief Configure the onboard heartbeat LED as a dark output.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init_io(void) {
    gpio_init(PICOKIT_31_THERMOSTAT_LED_PIN);
    gpio_set_dir(PICOKIT_31_THERMOSTAT_LED_PIN, GPIO_OUT);
    gpio_put(PICOKIT_31_THERMOSTAT_LED_PIN, 0);
}

/**
 * @brief Reset the setpoint, temperature, and vent state.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_reset_control(void) {
    g_setpoint = (int16_t)MONITOR_SETPOINT_DEFAULT_TENTHS;
    g_temperature = 0;
    g_vent = MONITOR_VENT_CLOSED_DEGREES;
    g_valid = false;
}

/**
 * @brief Reset the control state, sequence, and transmit timing.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init(void) {
    uint64_t now_us = time_us_64();
    monitor_state_reset_control();
    memset(&g_reading, 0, sizeof(g_reading));
    g_seq = 0u;
    g_next_read_us = now_us;
    g_next_tx_us = now_us + (uint64_t)PICOKIT_31_THERMOSTAT_TX_INTERVAL_MS * 1000u;
    g_ready = true;
}

/**
 * @brief Load the telemetry session key from the field secret.
 *
 * LAB-ONLY: production must provision the session key through OTP rather
 * than embedding a committed key.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_load_key(void) {
    static const uint8_t key[CCM_KEY_LEN] = FIELD_SECRET_KEY;
    memcpy(g_key, key, CCM_KEY_LEN);
    g_key_ready = true;
}

/**
 * @brief Print the boot banner for the thermostat lesson.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_banner(void) {
    printf("=== PICOKIT-31 THERMOSTAT // HYSTERESIS VENT + AUTHENTICATED HEARTBEAT ===\n");
}

/**
 * @brief Derive the field key and announce a ready monitor.
 *
 * @param void No parameters.
 * @return bool true when the field key was derived and installed.
 */
static bool monitor_finish(void) {
    monitor_load_key();
    monitor_banner();
    return true;
}

/**
 * @brief Blink the onboard heartbeat LED exactly once.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_heartbeat(void) {
    gpio_put(PICOKIT_31_THERMOSTAT_LED_PIN, 1);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
    gpio_put(PICOKIT_31_THERMOSTAT_LED_PIN, 0);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
}

/**
 * @brief Drive the vent with hysteresis against the current setpoint.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_apply_hysteresis(void) {
    if (!g_valid) {
        servo_set_angle(g_vent);
        return;
    }
    if (g_temperature >= g_setpoint + MONITOR_HYSTERESIS_TENTHS) {
        g_vent = MONITOR_VENT_OPEN_DEGREES;
    } else if (g_temperature <= g_setpoint - MONITOR_HYSTERESIS_TENTHS) {
        g_vent = MONITOR_VENT_CLOSED_DEGREES;
    }
    servo_set_angle(g_vent);
}

/**
 * @brief Print the current temperature, setpoint, and vent position.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_log_state(void) {
    printf("TEMP %d SET %d VENT %u\n", (int)g_temperature, (int)g_setpoint, (unsigned)g_vent);
}

/**
 * @brief Sample the DHT11, drive the vent, and schedule the next read.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_read_tick(uint64_t now_us) {
    sensor_result_t rc = sensor_read(&g_reading);
    g_valid = (rc == SENSOR_RESULT_OK);
    if (g_valid) {
        g_temperature = g_reading.temperature_tenths;
    }
    monitor_apply_hysteresis();
    monitor_log_state();
    g_next_read_us = now_us + (uint64_t)MONITOR_READ_INTERVAL_MS * 1000u;
}

/**
 * @brief Shift the setpoint by a delta and clamp it to the valid range.
 *
 * @param delta Signed change in tenths of a degree Celsius.
 * @return void
 */
static void monitor_setpoint_shift(int delta) {
    int value = (int)g_setpoint + delta;
    if (value < MONITOR_SETPOINT_MIN_TENTHS) {
        value = MONITOR_SETPOINT_MIN_TENTHS;
    }
    if (value > MONITOR_SETPOINT_MAX_TENTHS) {
        value = MONITOR_SETPOINT_MAX_TENTHS;
    }
    g_setpoint = (int16_t)value;
    monitor_apply_hysteresis();
    printf("SETPOINT %d\n", (int)g_setpoint);
}

/**
 * @brief Apply one decoded infrared remote command to the setpoint.
 *
 * @param cmd Decoded eight-bit remote command code.
 * @return void
 */
static void monitor_ir_apply(uint8_t cmd) {
    printf("IR 0x%02X\n", (unsigned)cmd);
    if (cmd == MONITOR_KEY_SET_UP) {
        monitor_setpoint_shift(MONITOR_SETPOINT_STEP_TENTHS);
    } else if (cmd == MONITOR_KEY_SET_DOWN) {
        monitor_setpoint_shift(-MONITOR_SETPOINT_STEP_TENTHS);
    }
}

/**
 * @brief Poll the infrared eye for a setpoint command.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_poll_ir(void) {
    ir_command_t cmd;
    if (ir_remote_poll(&cmd)) {
        monitor_ir_apply(cmd.command);
    }
}

/**
 * @brief Format the heartbeat JSON body for the vent and temperature.
 *
 * @param frame Pointer to the mutable frame output buffer.
 * @param frame_len Capacity of the frame output buffer in bytes.
 * @return size_t Number of JSON bytes written, or zero on overflow.
 */
static size_t monitor_build_frame(char *frame, size_t frame_len) {
    int written = snprintf(frame, frame_len, "{\"n\":%u,\"s\":%u,\"v\":%u,\"t\":%d}", (unsigned)PACKET_NODE_ID, (unsigned)g_seq, (unsigned)g_vent, (int)g_temperature);
    return (written > 0 && (size_t)written < frame_len) ? (size_t)written : 0u;
}

/**
 * @brief Seal the current heartbeat body into a hex envelope.
 *
 * @param hex Pointer to the NUL-terminated hex output buffer.
 * @param hex_len Capacity of the hex output buffer in bytes.
 * @return bool true when the heartbeat was sealed and encoded.
 */
static bool monitor_seal_frame(char *hex, size_t hex_len) {
    char frame[PICOKIT_31_THERMOSTAT_FRAME_SIZE];
    uint8_t nonce[ENVELOPE_NONCE_LEN];
    uint8_t ad = (uint8_t)PACKET_NODE_ID;
    size_t frame_len = monitor_build_frame(frame, sizeof(frame));
    envelope_fill_nonce(nonce);
    return envelope_seal_hex(g_key, nonce, &ad, 1u, (const uint8_t *)frame, frame_len, hex, hex_len);
}

/**
 * @brief Build and transmit the authenticated heartbeat frame.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_transmit(void) {
    char hex[ENVELOPE_MAX_HEX_LEN];
    if (!g_key_ready) {
        return;
    }
    if (monitor_seal_frame(hex, sizeof(hex))) {
        radio_send_frame(PICOKIT_31_THERMOSTAT_UART, (const uint8_t *)hex, strlen(hex));
        g_seq += 1u;
    }
}

/**
 * @brief Transmit one heartbeat and schedule the next transmit.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_tx_tick(uint64_t now_us) {
    monitor_heartbeat();
    monitor_transmit();
    g_next_tx_us = now_us + (uint64_t)PICOKIT_31_THERMOSTAT_TX_INTERVAL_MS * 1000u;
}

/**
 * @brief Drain inbound radio lines and log every valid +RCV report.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_rx_tick(void) {
    radio_rcv_t rcv;
    while (radio_line_pump(PICOKIT_31_THERMOSTAT_UART, g_rx_line, &g_rx_len)) {
        if (radio_parse_rcv(g_rx_line, &rcv) == RADIO_RESULT_OK) {
            printf("RX from 0x%04X, %u bytes\n", (unsigned)rcv.sender, (unsigned)rcv.len);
        }
    }
}

/**
 * @brief Service the DHT11 sample and heartbeat transmit timers.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_service_timers(uint64_t now_us) {
    if (now_us >= g_next_read_us) {
        monitor_read_tick(now_us);
    }
    if (now_us >= g_next_tx_us) {
        monitor_tx_tick(now_us);
    }
}

bool monitor_init(void) {
    bool ok;
    ok = status_led_init() && radio_init(PICOKIT_31_THERMOSTAT_UART);
    ok = ok && ir_remote_init() && servo_init();
    ok = ok && sensor_init();
    monitor_state_init_io();
    monitor_state_init();
    return ok && monitor_finish();
}

void monitor_deinit(void) {
    g_ready = false;
}

bool monitor_step(void) {
    uint64_t now_us;
    if (!g_ready) {
        return false;
    }
    now_us = time_us_64();
    monitor_service_timers(now_us);
    monitor_poll_ir();
    monitor_rx_tick();
    return true;
}
