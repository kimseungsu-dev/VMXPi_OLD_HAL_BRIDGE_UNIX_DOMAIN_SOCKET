/*
 * SPDX-License-Identifier: MIT
 *
 * Unofficial, independently developed Unix Domain Socket controller.
 * Requires the separately installed VMX-pi HAL 1.1.257 ARMHF package.
 */

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "VMXPi.h"

namespace {

constexpr const char* SOCKET_PATH = "/run/vmx-controller.sock";

volatile std::sig_atomic_t stop_requested = 0;
int server_fd = -1;

constexpr std::size_t MAX_VMX_CHANNELS = 34;
constexpr std::size_t MAX_COMMAND_LINE_BYTES = 4096;
constexpr std::size_t MAX_UART_TRANSFER_BYTES = 1024;
constexpr std::size_t MAX_SPI_TRANSFER_BYTES = 1024;
constexpr std::size_t MAX_I2C_TRANSFER_BYTES = 1024;
constexpr int MAX_LED_ARRAY_PIXELS = 1024;
constexpr std::size_t MAX_CAN_RECEIVE_STREAMS = 32;

struct ControllerState
{
    bool dio_output_active[MAX_VMX_CHANNELS];
    VMXResourceHandle dio_output_handles[MAX_VMX_CHANNELS];

    bool dio_input_active[MAX_VMX_CHANNELS];
    VMXResourceHandle dio_input_handles[MAX_VMX_CHANNELS];

    bool analog_active[MAX_VMX_CHANNELS];
    VMXResourceHandle analog_handles[MAX_VMX_CHANNELS];
    bool analog_counter_enabled[MAX_VMX_CHANNELS];

    bool analog_trigger_active[MAX_VMX_CHANNELS];
    VMXResourceHandle analog_trigger_handles[MAX_VMX_CHANNELS];

    bool encoder_active[MAX_VMX_CHANNELS];
    VMXResourceHandle encoder_handles[MAX_VMX_CHANNELS];
    int encoder_partner[MAX_VMX_CHANNELS];
    bool encoder_is_a[MAX_VMX_CHANNELS];

    bool input_capture_active[MAX_VMX_CHANNELS];
    VMXResourceHandle input_capture_handles[MAX_VMX_CHANNELS];
    int input_capture_partner[MAX_VMX_CHANNELS];
    bool input_capture_is_primary[MAX_VMX_CHANNELS];

    bool pwm_capture_active[MAX_VMX_CHANNELS];
    VMXResourceHandle pwm_capture_handles[MAX_VMX_CHANNELS];

    bool uart_active[MAX_VMX_CHANNELS];
    VMXResourceHandle uart_handles[MAX_VMX_CHANNELS];
    int uart_partner[MAX_VMX_CHANNELS];
    bool uart_is_tx[MAX_VMX_CHANNELS];
    uint32_t uart_baudrate[MAX_VMX_CHANNELS];

    bool spi_active[MAX_VMX_CHANNELS];
    VMXResourceHandle spi_handles[MAX_VMX_CHANNELS];
    int spi_members[MAX_VMX_CHANNELS][4];
    uint8_t spi_role[MAX_VMX_CHANNELS];
    uint32_t spi_bitrate[MAX_VMX_CHANNELS];
    uint8_t spi_mode[MAX_VMX_CHANNELS];

    bool i2c_active[MAX_VMX_CHANNELS];
    VMXResourceHandle i2c_handles[MAX_VMX_CHANNELS];
    int i2c_partner[MAX_VMX_CHANNELS];
    bool i2c_is_sda[MAX_VMX_CHANNELS];

    bool led_array_active[MAX_VMX_CHANNELS];
    VMXResourceHandle led_array_handles[MAX_VMX_CHANNELS];
    LEDArrayBufferHandle led_array_buffers[MAX_VMX_CHANNELS];
    int led_array_pixels[MAX_VMX_CHANNELS];
    uint8_t led_array_format[MAX_VMX_CHANNELS];

    bool pwm_active[MAX_VMX_CHANNELS];
    VMXResourceHandle pwm_handles[MAX_VMX_CHANNELS];
    uint32_t pwm_frequency_hz[MAX_VMX_CHANNELS];

    // TITAN_BRIDGE_V1
    bool titan_configured = false;
    uint8_t titan_can_id = 42;
    bool titan_enabled = false;
    double titan_speed[4] = {0.0, 0.0, 0.0, 0.0};

    // VMX_CAN_BRIDGE_V1
    std::vector<VMXCANReceiveStreamHandle>
        can_receive_streams;

    ControllerState()
    {
        for (std::size_t i = 0; i < MAX_VMX_CHANNELS; ++i) {
            dio_output_active[i] = false;
            dio_output_handles[i] = 0;

            dio_input_active[i] = false;
            dio_input_handles[i] = 0;

            analog_active[i] = false;
            analog_handles[i] = 0;
            analog_counter_enabled[i] = false;

            analog_trigger_active[i] = false;
            analog_trigger_handles[i] = 0;

            encoder_active[i] = false;
            encoder_handles[i] = 0;
            encoder_partner[i] = -1;
            encoder_is_a[i] = false;

            input_capture_active[i] = false;
            input_capture_handles[i] = 0;
            input_capture_partner[i] = -1;
            input_capture_is_primary[i] = false;

            pwm_capture_active[i] = false;
            pwm_capture_handles[i] = 0;

            uart_active[i] = false;
            uart_handles[i] = 0;
            uart_partner[i] = -1;
            uart_is_tx[i] = false;
            uart_baudrate[i] = 0;

            spi_active[i] = false;
            spi_handles[i] = 0;
            spi_role[i] = 255;
            spi_bitrate[i] = 0;
            spi_mode[i] = 0;

            for (std::size_t member = 0;
                 member < 4;
                 ++member) {
                spi_members[i][member] = -1;
            }

            i2c_active[i] = false;
            i2c_handles[i] = 0;
            i2c_partner[i] = -1;
            i2c_is_sda[i] = false;

            led_array_active[i] = false;
            led_array_handles[i] = 0;
            led_array_buffers[i] = 0;
            led_array_pixels[i] = 0;
            led_array_format[i] = 255;

            pwm_active[i] = false;
            pwm_handles[i] = 0;
            pwm_frequency_hz[i] = 0;
        }
    }
};


struct RTCDateTime
{
    int year;
    int month;
    int day;
    int weekday;
    int hour;
    int minute;
    int second;
    int millisecond;
};

int days_in_month(int year, int month)
{
    static const int days[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) {
        return 0;
    }

    if (month == 2) {
        const bool leap =
            (year % 400 == 0) ||
            ((year % 4 == 0) &&
             (year % 100 != 0));

        return leap ? 29 : 28;
    }

    return days[month - 1];
}

bool valid_rtc_datetime(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second
)
{
    if (year < 2020 || year > 2099) {
        return false;
    }

    if (month < 1 || month > 12) {
        return false;
    }

    if (day < 1 ||
        day > days_in_month(year, month)) {
        return false;
    }

    if (hour < 0 || hour > 23) {
        return false;
    }

    if (minute < 0 || minute > 59) {
        return false;
    }

    if (second < 0 || second > 59) {
        return false;
    }

    return true;
}

uint8_t linux_weekday_to_vmx(int weekday)
{
    return weekday == 0
        ? static_cast<uint8_t>(7)
        : static_cast<uint8_t>(weekday);
}

bool read_rtc_time(
    VMXPi& vmx,
    RTCDateTime& value,
    VMXErrorCode& error
)
{
    uint8_t weekday = 0;
    uint8_t day = 0;
    uint8_t month = 0;
    uint8_t year = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint32_t millisecond = 0;

    error = 0;

    if (!vmx.time.GetRTCDate(
            weekday,
            day,
            month,
            year,
            &error)) {
        return false;
    }

    error = 0;

    if (!vmx.time.GetRTCTime(
            hour,
            minute,
            second,
            millisecond,
            &error)) {
        return false;
    }

    value.year =
        2000 + static_cast<int>(year);

    value.month =
        static_cast<int>(month);

    value.day =
        static_cast<int>(day);

    value.weekday =
        static_cast<int>(weekday);

    value.hour =
        static_cast<int>(hour);

    value.minute =
        static_cast<int>(minute);

    value.second =
        static_cast<int>(second);

    value.millisecond =
        static_cast<int>(millisecond);

    return true;
}

bool set_rtc_time(
    VMXPi& vmx,
    int year,
    int month,
    int day,
    int hour,
    int minute,
    VMXErrorCode& error
)
{
    if (!valid_rtc_datetime(
            year,
            month,
            day,
            hour,
            minute,
            0)) {
        error = 0;
        return false;
    }

    std::tm requested {};
    requested.tm_year = year - 1900;
    requested.tm_mon = month - 1;
    requested.tm_mday = day;
    requested.tm_hour = hour;
    requested.tm_min = minute;
    requested.tm_sec = 0;
    requested.tm_isdst = -1;

    const std::time_t epoch =
        std::mktime(&requested);

    if (epoch == static_cast<std::time_t>(-1)) {
        error = 0;
        return false;
    }

    std::tm normalized {};

    if (localtime_r(
            &epoch,
            &normalized) == nullptr) {
        error = 0;
        return false;
    }

    if (normalized.tm_year != year - 1900 ||
        normalized.tm_mon != month - 1 ||
        normalized.tm_mday != day ||
        normalized.tm_hour != hour ||
        normalized.tm_min != minute) {
        error = 0;
        return false;
    }

    error = 0;

    if (!vmx.time.SetRTCDaylightSavingsAdjustment(
            VMXTime::DaylightSavingsAdjustment::
                DSAdjustmentNone,
            &error)) {
        return false;
    }

    error = 0;

    if (!vmx.time.SetRTCDate(
            linux_weekday_to_vmx(
                normalized.tm_wday
            ),
            static_cast<uint8_t>(day),
            static_cast<uint8_t>(month),
            static_cast<uint8_t>(year - 2000),
            &error)) {
        return false;
    }

    error = 0;

    if (!vmx.time.SetRTCTime(
            static_cast<uint8_t>(hour),
            static_cast<uint8_t>(minute),
            static_cast<uint8_t>(0),
            &error)) {
        return false;
    }

    return true;
}

bool sync_linux_time_from_rtc(
    VMXPi& vmx,
    RTCDateTime& rtc,
    VMXErrorCode& error,
    int& system_error
)
{
    system_error = 0;

    if (!read_rtc_time(
            vmx,
            rtc,
            error)) {
        return false;
    }

    if (!valid_rtc_datetime(
            rtc.year,
            rtc.month,
            rtc.day,
            rtc.hour,
            rtc.minute,
            rtc.second)) {
        error = 0;
        system_error = EINVAL;
        return false;
    }

    if (rtc.weekday < 1 ||
        rtc.weekday > 7 ||
        rtc.millisecond < 0 ||
        rtc.millisecond > 999) {
        error = 0;
        system_error = EINVAL;
        return false;
    }

    std::tm value {};
    value.tm_year = rtc.year - 1900;
    value.tm_mon = rtc.month - 1;
    value.tm_mday = rtc.day;
    value.tm_hour = rtc.hour;
    value.tm_min = rtc.minute;
    value.tm_sec = rtc.second;
    value.tm_isdst = -1;

    const std::time_t epoch =
        std::mktime(&value);

    if (epoch == static_cast<std::time_t>(-1)) {
        error = 0;
        system_error = EINVAL;
        return false;
    }

    const timespec target {
        epoch,
        static_cast<long>(
            rtc.millisecond * 1000000L
        )
    };

    if (clock_settime(
            CLOCK_REALTIME,
            &target) != 0) {
        error = 0;
        system_error = errno;
        return false;
    }

    return true;
}

std::string make_vmx_error(VMXErrorCode error)
{
    const char* description = GetVMXErrorString(error);

    if (description == nullptr) {
        description = "UNKNOWN";
    }

    char response[256];

    std::snprintf(
        response,
        sizeof(response),
        "ERR VMX=%d MESSAGE=%s\n",
        static_cast<int>(error),
        description
    );

    return std::string(response);
}


constexpr uint32_t TITAN_CAN_BASE = 0x020C0000U;
constexpr uint32_t TITAN_CAN_OFFSET = 0x40U;
constexpr uint32_t TITAN_DISABLED_INDEX = 0U;
constexpr uint32_t TITAN_ENABLED_INDEX = 1U;
constexpr uint32_t TITAN_SET_SPEED_INDEX = 2U;

uint32_t titan_message_id(uint8_t can_id, uint32_t index)
{
    return TITAN_CAN_BASE +
        (TITAN_CAN_OFFSET * index) +
        static_cast<uint32_t>(can_id);
}

bool send_titan_frame(
    VMXPi& vmx,
    uint8_t can_id,
    uint32_t index,
    const uint8_t data[8],
    int32_t period_ms,
    VMXErrorCode& error
)
{
    VMXCANMessage message {};
    message.messageID = titan_message_id(can_id, index);
    message.dataSize = 8;
    message.setData(data, 8);

    error = 0;
    return vmx.can.SendMessage(message, period_ms, &error);
}

bool titan_enable(
    VMXPi& vmx,
    ControllerState& state,
    VMXErrorCode& last_error
)
{
    const uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (int attempt = 0; attempt < 3; ++attempt) {
        VMXErrorCode error = 0;

        if (!send_titan_frame(
                vmx,
                state.titan_can_id,
                TITAN_ENABLED_INDEX,
                data,
                0,
                error)) {
            last_error = error;
            return false;
        }

        vmx.time.DelayMilliseconds(20);
    }

    state.titan_enabled = true;
    last_error = 0;
    return true;
}

bool titan_brake_all(
    VMXPi& vmx,
    ControllerState& state,
    VMXErrorCode& last_error
)
{
    bool success = true;
    last_error = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        for (uint8_t motor = 0; motor < 4; ++motor) {
            const uint8_t data[8] = {
                motor, 0, 1, 1, 0, 0, 0, 0
            };

            VMXErrorCode error = 0;

            if (!send_titan_frame(
                    vmx,
                    state.titan_can_id,
                    TITAN_SET_SPEED_INDEX,
                    data,
                    0,
                    error)) {
                success = false;
                last_error = error;
            }
        }

        vmx.time.DelayMilliseconds(20);
    }

    for (int motor = 0; motor < 4; ++motor) {
        state.titan_speed[motor] = 0.0;
    }

    return success;
}

bool titan_stop_and_disable(
    VMXPi& vmx,
    ControllerState& state,
    VMXErrorCode& last_error
)
{
    const uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool success = true;
    last_error = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        VMXErrorCode error = 0;

        if (!send_titan_frame(
                vmx,
                state.titan_can_id,
                TITAN_DISABLED_INDEX,
                data,
                0,
                error)) {
            success = false;
            last_error = error;
        }

        vmx.time.DelayMilliseconds(50);
    }

    state.titan_enabled = false;

    for (int motor = 0; motor < 4; ++motor) {
        state.titan_speed[motor] = 0.0;
    }

    return success;
}

bool safe_stop_outputs(
    VMXPi& vmx,
    ControllerState& state,
    VMXErrorCode& last_error
)
{
    bool success = true;
    last_error = 0;

    for (std::size_t channel = 0;
         channel < MAX_VMX_CHANNELS;
         ++channel) {

        if (!state.dio_output_active[channel]) {
            continue;
        }

        VMXErrorCode error = 0;

        if (!vmx.io.DIO_Set(
                state.dio_output_handles[channel],
                false,
                &error)) {
            success = false;
            last_error = error;
        }
    }

    for (std::size_t channel = 0;
         channel < MAX_VMX_CHANNELS;
         ++channel) {

        if (!state.pwm_active[channel]) {
            continue;
        }

        VMXErrorCode error = 0;

        if (!vmx.io.PWMGenerator_SetDutyCycle(
                state.pwm_handles[channel],
                static_cast<VMXResourcePortIndex>(0),
                0,
                &error)) {
            success = false;
            last_error = error;
        }
    }

    for (std::size_t channel = 0;
         channel < MAX_VMX_CHANNELS;
         ++channel) {

        if (!state.led_array_active[channel]) {
            continue;
        }

        const LEDArrayBufferHandle buffer =
            state.led_array_buffers[channel];

        for (int index = 0;
             index < state.led_array_pixels[channel];
             ++index) {

            VMXErrorCode error = 0;

            if (!vmx.io.LEDArrayBuffer_SetRGBValue(
                    buffer,
                    index,
                    0,
                    0,
                    0,
                    &error)) {
                success = false;
                last_error = error;
            }
        }

        VMXErrorCode error = 0;

        if (!vmx.io.LEDArray_Render(
                state.led_array_handles[channel],
                &error)) {
            success = false;
            last_error = error;
        }
    }


    if (state.titan_configured) {
        VMXErrorCode error = 0;

        if (!titan_stop_and_disable(vmx, state, error)) {
            success = false;
            last_error = error;
        }
    }

    return success;
}


bool can_stream_is_open(
    const ControllerState& state,
    VMXCANReceiveStreamHandle handle
)
{
    for (std::size_t index = 0;
         index < state.can_receive_streams.size();
         ++index) {
        if (state.can_receive_streams[index] == handle) {
            return true;
        }
    }

    return false;
}

void remove_can_stream(
    ControllerState& state,
    VMXCANReceiveStreamHandle handle
)
{
    for (std::size_t index = 0;
         index < state.can_receive_streams.size();
         ++index) {
        if (state.can_receive_streams[index] != handle) {
            continue;
        }

        state.can_receive_streams.erase(
            state.can_receive_streams.begin() + index
        );

        return;
    }
}

bool close_all_can_streams(
    VMXPi& vmx,
    ControllerState& state,
    VMXErrorCode& last_error,
    unsigned int& closed_count
)
{
    bool success = true;
    last_error = 0;
    closed_count = 0;

    for (std::size_t index = 0;
         index < state.can_receive_streams.size();
         ++index) {

        VMXErrorCode error = 0;

        if (!vmx.can.CloseReceiveStream(
                state.can_receive_streams[index],
                &error)) {
            success = false;
            last_error = error;
            continue;
        }

        ++closed_count;
    }

    state.can_receive_streams.clear();
    return success;
}

const char* can_mode_name(
    VMXCAN::VMXCANMode mode
)
{
    switch (mode) {
    case VMXCAN::VMXCAN_LISTEN:
        return "LISTEN";
    case VMXCAN::VMXCAN_LOOPBACK:
        return "LOOPBACK";
    case VMXCAN::VMXCAN_NORMAL:
        return "NORMAL";
    case VMXCAN::VMXCAN_CONFIG:
        return "CONFIG";
    case VMXCAN::VMXCAN_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

bool configure_watchdog(
    VMXPi& vmx,
    uint16_t timeout_ms,
    bool enabled,
    VMXErrorCode& last_error
)
{
    last_error = 0;

    if (!vmx.io.SetWatchdogEnabled(
            false,
            &last_error)) {
        return false;
    }

    last_error = 0;

    if (!vmx.io.SetWatchdogManagedOutputs(
            true,
            true,
            true,
            &last_error)) {
        return false;
    }

    last_error = 0;

    if (!vmx.io.SetWatchdogTimeoutPeriodMS(
            timeout_ms,
            &last_error)) {
        return false;
    }

    last_error = 0;

    if (!vmx.io.FeedWatchdog(&last_error)) {
        return false;
    }

    last_error = 0;

    if (!vmx.io.SetWatchdogEnabled(
            enabled,
            &last_error)) {
        return false;
    }

    return true;
}

void handle_signal(int)
{
    stop_requested = 1;

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

bool send_all(int fd, const std::string& data)
{
    std::size_t total_sent = 0;

    while (total_sent < data.size()) {
        const ssize_t sent = send(
            fd,
            data.data() + total_sent,
            data.size() - total_sent,
            MSG_NOSIGNAL
        );

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

bool read_line(int fd, std::string& line)
{
    line.clear();
    char ch = '\0';

    while (line.size() < MAX_COMMAND_LINE_BYTES) {
        const ssize_t received = recv(fd, &ch, 1, 0);

        if (received == 0) {
            return false;
        }

        if (received < 0) {
            if (errno == EINTR) {
                if (stop_requested) {
                    return false;
                }

                continue;
            }

            return false;
        }

        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            return true;
        }

        line.push_back(ch);
    }

    return false;
}


bool release_channel(
    VMXPi& vmx,
    ControllerState& state,
    std::size_t channel,
    VMXErrorCode& last_error,
    unsigned int& released_count
)
{
    last_error = 0;
    released_count = 0;

    if (state.dio_output_active[channel]) {
        VMXErrorCode error = 0;

        if (!vmx.io.DIO_Set(
                state.dio_output_handles[channel],
                false,
                &error)) {
            last_error = error;
            return false;
        }

        error = 0;

        vmx.io.DeallocateResource(
            state.dio_output_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.dio_output_active[channel] = false;
        state.dio_output_handles[channel] = 0;
        ++released_count;
    }

    if (state.dio_input_active[channel]) {
        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(
            state.dio_input_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.dio_input_active[channel] = false;
        state.dio_input_handles[channel] = 0;
        ++released_count;
    }

    if (state.pwm_active[channel]) {
        VMXErrorCode error = 0;

        if (!vmx.io.PWMGenerator_SetDutyCycle(
                state.pwm_handles[channel],
                static_cast<VMXResourcePortIndex>(0),
                0,
                &error)) {
            last_error = error;
            return false;
        }

        error = 0;

        vmx.io.DeallocateResource(
            state.pwm_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.pwm_active[channel] = false;
        state.pwm_handles[channel] = 0;
        state.pwm_frequency_hz[channel] = 0;
        ++released_count;
    }

    if (state.analog_active[channel]) {
        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(
            state.analog_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.analog_active[channel] = false;
        state.analog_handles[channel] = 0;
        state.analog_counter_enabled[channel] = false;
        ++released_count;
    }

    if (state.analog_trigger_active[channel]) {
        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(
            state.analog_trigger_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.analog_trigger_active[channel] = false;
        state.analog_trigger_handles[channel] = 0;
        ++released_count;
    }

    if (state.encoder_active[channel]) {
        const VMXResourceHandle handle =
            state.encoder_handles[channel];

        const int partner =
            state.encoder_partner[channel];

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(handle, &error);

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.encoder_active[channel] = false;
        state.encoder_handles[channel] = 0;
        state.encoder_partner[channel] = -1;
        state.encoder_is_a[channel] = false;

        if (partner >= 0 &&
            partner < static_cast<int>(MAX_VMX_CHANNELS)) {

            state.encoder_active[partner] = false;
            state.encoder_handles[partner] = 0;
            state.encoder_partner[partner] = -1;
            state.encoder_is_a[partner] = false;
        }

        ++released_count;
    }

    if (state.input_capture_active[channel]) {
        const VMXResourceHandle handle =
            state.input_capture_handles[channel];

        const int partner =
            state.input_capture_partner[channel];

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(handle, &error);

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.input_capture_active[channel] = false;
        state.input_capture_handles[channel] = 0;
        state.input_capture_partner[channel] = -1;
        state.input_capture_is_primary[channel] = false;

        if (partner >= 0 &&
            partner < static_cast<int>(MAX_VMX_CHANNELS)) {

            state.input_capture_active[partner] = false;
            state.input_capture_handles[partner] = 0;
            state.input_capture_partner[partner] = -1;
            state.input_capture_is_primary[partner] = false;
        }

        ++released_count;
    }

    if (state.pwm_capture_active[channel]) {
        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(
            state.pwm_capture_handles[channel],
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.pwm_capture_active[channel] = false;
        state.pwm_capture_handles[channel] = 0;
        ++released_count;
    }

    if (state.uart_active[channel]) {
        const VMXResourceHandle handle =
            state.uart_handles[channel];

        const int partner =
            state.uart_partner[channel];

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(handle, &error);

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.uart_active[channel] = false;
        state.uart_handles[channel] = 0;
        state.uart_partner[channel] = -1;
        state.uart_is_tx[channel] = false;
        state.uart_baudrate[channel] = 0;

        if (partner >= 0 &&
            partner < static_cast<int>(MAX_VMX_CHANNELS)) {

            state.uart_active[partner] = false;
            state.uart_handles[partner] = 0;
            state.uart_partner[partner] = -1;
            state.uart_is_tx[partner] = false;
            state.uart_baudrate[partner] = 0;
        }

        ++released_count;
    }

    if (state.spi_active[channel]) {
        const VMXResourceHandle handle =
            state.spi_handles[channel];

        int members[4] = {-1, -1, -1, -1};

        for (std::size_t index = 0;
             index < 4;
             ++index) {
            members[index] =
                state.spi_members[channel][index];
        }

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(handle, &error);

        if (error != 0) {
            last_error = error;
            return false;
        }

        for (std::size_t index = 0;
             index < 4;
             ++index) {

            const int member = members[index];

            if (member < 0 ||
                member >= static_cast<int>(
                    MAX_VMX_CHANNELS
                )) {
                continue;
            }

            if (state.spi_active[member] &&
                state.spi_handles[member] == handle) {

                state.spi_active[member] = false;
                state.spi_handles[member] = 0;
                state.spi_role[member] = 255;
                state.spi_bitrate[member] = 0;
                state.spi_mode[member] = 0;

                for (std::size_t clear_index = 0;
                     clear_index < 4;
                     ++clear_index) {
                    state.spi_members[member][clear_index] =
                        -1;
                }
            }
        }

        ++released_count;
    }

    if (state.i2c_active[channel]) {
        const VMXResourceHandle handle =
            state.i2c_handles[channel];

        const int partner =
            state.i2c_partner[channel];

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(handle, &error);

        if (error != 0) {
            last_error = error;
            return false;
        }

        state.i2c_active[channel] = false;
        state.i2c_handles[channel] = 0;
        state.i2c_partner[channel] = -1;
        state.i2c_is_sda[channel] = false;

        if (partner >= 0 &&
            partner < static_cast<int>(MAX_VMX_CHANNELS)) {

            state.i2c_active[partner] = false;
            state.i2c_handles[partner] = 0;
            state.i2c_partner[partner] = -1;
            state.i2c_is_sda[partner] = false;
        }

        ++released_count;
    }

    if (state.led_array_active[channel]) {
        const VMXResourceHandle handle =
            state.led_array_handles[channel];

        const LEDArrayBufferHandle buffer =
            state.led_array_buffers[channel];

        const int pixel_count =
            state.led_array_pixels[channel];

        if (buffer != 0) {
            for (int index = 0;
                 index < pixel_count;
                 ++index) {

                VMXErrorCode clear_error = 0;

                vmx.io.LEDArrayBuffer_SetRGBValue(
                    buffer,
                    index,
                    0,
                    0,
                    0,
                    &clear_error
                );
            }

            VMXErrorCode render_error = 0;

            vmx.io.LEDArray_Render(
                handle,
                &render_error
            );
        }

        VMXErrorCode error = 0;

        vmx.io.DeallocateResource(
            handle,
            &error
        );

        if (error != 0) {
            last_error = error;
            return false;
        }

        // HAL owns the buffer after LEDArray_SetBuffer succeeds.
        state.led_array_active[channel] = false;
        state.led_array_handles[channel] = 0;
        state.led_array_buffers[channel] = 0;
        state.led_array_pixels[channel] = 0;
        state.led_array_format[channel] = 255;

        ++released_count;
    }

    return true;
}

int hex_character_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'A' && character <= 'F') {
        return 10 + character - 'A';
    }

    if (character >= 'a' && character <= 'f') {
        return 10 + character - 'a';
    }

    return -1;
}

bool decode_hex(
    const std::string& text,
    std::vector<uint8_t>& output
)
{
    output.clear();

    if (text.empty() || (text.size() % 2) != 0) {
        return false;
    }

    if ((text.size() / 2) > MAX_UART_TRANSFER_BYTES) {
        return false;
    }

    output.reserve(text.size() / 2);

    for (std::size_t index = 0;
         index < text.size();
         index += 2) {

        const int high =
            hex_character_value(text[index]);

        const int low =
            hex_character_value(text[index + 1]);

        if (high < 0 || low < 0) {
            output.clear();
            return false;
        }

        output.push_back(
            static_cast<uint8_t>((high << 4) | low)
        );
    }

    return true;
}

std::string encode_hex(
    const uint8_t* data,
    std::size_t size
)
{
    static const char digits[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(size * 2);

    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(digits[(data[index] >> 4) & 0x0F]);
        result.push_back(digits[data[index] & 0x0F]);
    }

    return result;
}

const char* spi_role_name(uint8_t role)
{
    switch (role) {
    case 0:
        return "CLK";
    case 1:
        return "MISO";
    case 2:
        return "MOSI";
    case 3:
        return "CS";
    default:
        return "UNKNOWN";
    }
}

const char* i2c_role_name(bool is_sda)
{
    return is_sda ? "SDA" : "SCL";
}

bool parse_led_array_format(
    const std::string& text,
    LEDArray_OneWireConfig::PixelFormat& format,
    uint8_t& format_code
)
{
    if (text == "RGB") {
        format = LEDArray_OneWireConfig::RGB;
        format_code = 0;
        return true;
    }

    if (text == "RBG") {
        format = LEDArray_OneWireConfig::RBG;
        format_code = 1;
        return true;
    }

    if (text == "GRB") {
        format = LEDArray_OneWireConfig::GRB;
        format_code = 2;
        return true;
    }

    if (text == "GBR") {
        format = LEDArray_OneWireConfig::GBR;
        format_code = 3;
        return true;
    }

    if (text == "BRG") {
        format = LEDArray_OneWireConfig::BRG;
        format_code = 4;
        return true;
    }

    if (text == "BGR") {
        format = LEDArray_OneWireConfig::BGR;
        format_code = 5;
        return true;
    }

    return false;
}

bool led_array_rgb_valid(int red, int green, int blue)
{
    return
        red >= 0 && red <= 255 &&
        green >= 0 && green <= 255 &&
        blue >= 0 && blue <= 255;
}

std::string process_command(
    const std::string& command,
    VMXPi& vmx,
    ControllerState& state,
    bool& close_client
)
{
    close_client = false;

    const bool automatic_watchdog_feed =
        command != "WATCHDOG_STATUS" &&
        command != "WATCHDOG_FEED" &&
        command != "WATCHDOG_EXPIRE" &&
        command.rfind("WATCHDOG_CONFIG", 0) != 0;

    if (automatic_watchdog_feed) {
        VMXErrorCode watchdog_error = 0;

        if (!vmx.io.FeedWatchdog(
                &watchdog_error)) {
            return make_vmx_error(watchdog_error);
        }
    }

    if (command == "GET_PROTOCOL") {
        return "OK PROTOCOL=1\n";
    }

    if (command == "PING") {
        return "OK PONG\n";
    }

    if (command == "GET_INFO") {
        std::string response = "OK HAL=";
        response += vmx.version.GetHALVersion();
        response += " FW=";
        response += vmx.version.GetFirmwareVersion();
        response += "\n";

        return response;
    }

    if (command == "POWER_VOLTAGE") {
        float voltage = 0.0f;
        VMXErrorCode error = 0;

        if (!vmx.power.GetSystemVoltage(voltage, &error)) {
            return make_vmx_error(error);
        }

        char response[64];
        std::snprintf(
            response,
            sizeof(response),
            "OK VOLTAGE=%.3f\n",
            static_cast<double>(voltage)
        );
        return std::string(response);
    }

    if (command == "READ_ADC_SCALE") {
        VMXErrorCode error = 0;
        float volts = 0.0f;

        if (!vmx.io.Accumulator_GetFullScaleVoltage(volts, &error)) {
            char response[64];

            std::snprintf(
                response,
                sizeof(response),
                "ERR VMX=%d\n",
                static_cast<int>(error)
            );

            return std::string(response);
        }

        char response[64];
        std::snprintf(response, sizeof(response), "OK VOLTS=%.3f\n", volts);
        return std::string(response);
    }

    if (command.rfind("CHANNEL_INFO", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "CHANNEL_INFO %d",
                &channel) != 1) {
            return "ERR USAGE=CHANNEL_INFO_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        VMXChannelType channel_type;
        VMXChannelCapability capabilities;

        if (!vmx.io.GetChannelCapabilities(
                static_cast<VMXChannelIndex>(channel),
                channel_type,
                capabilities)) {
            return "ERR CHANNEL_INFO_FAILED\n";
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d TYPE=%u CAPS=0x%08lX\n",
            channel,
            static_cast<unsigned int>(channel_type),
            static_cast<unsigned long>(capabilities)
        );

        return std::string(response);
    }

    if (command.rfind("INPUT_CAPTURE_OPEN", 0) == 0) {
        int channel_a = -1;
        int channel_b = -1;

        if (std::sscanf(
                command.c_str(),
                "INPUT_CAPTURE_OPEN %d %d",
                &channel_a,
                &channel_b) != 2) {
            return "ERR USAGE=INPUT_CAPTURE_OPEN_<channel_a>_<channel_b>\n";
        }

        if (channel_a < 0 ||
            channel_a >= static_cast<int>(MAX_VMX_CHANNELS) ||
            channel_b < 0 ||
            channel_b >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (channel_a == channel_b) {
            return "ERR CAPTURE_CHANNELS_MUST_DIFFER\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel_a),
                VMXChannelCapability::InputCaptureInput)) {
            return "ERR CHANNEL_A_NOT_CAPTURE_CH1_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel_b),
                VMXChannelCapability::InputCaptureInput2)) {
            return "ERR CHANNEL_B_NOT_CAPTURE_CH2_CAPABLE\n";
        }

        if (state.input_capture_active[channel_a] ||
            state.input_capture_active[channel_b]) {

            if (state.input_capture_active[channel_a] &&
                state.input_capture_active[channel_b] &&
                state.input_capture_handles[channel_a] ==
                    state.input_capture_handles[channel_b] &&
                state.input_capture_partner[channel_a] == channel_b &&
                state.input_capture_partner[channel_b] == channel_a &&
                state.input_capture_is_primary[channel_a] &&
                !state.input_capture_is_primary[channel_b]) {

                char response[144];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL_A=%d CHANNEL_B=%d "
                    "MODE=INPUT_CAPTURE ALREADY_OPEN=1\n",
                    channel_a,
                    channel_b
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        const bool channel_a_busy =
            state.dio_output_active[channel_a] ||
            state.dio_input_active[channel_a] ||
            state.pwm_active[channel_a] ||
            state.analog_active[channel_a] ||
            state.encoder_active[channel_a] ||
            state.pwm_capture_active[channel_a];

        const bool channel_b_busy =
            state.dio_output_active[channel_b] ||
            state.dio_input_active[channel_b] ||
            state.pwm_active[channel_b] ||
            state.analog_active[channel_b] ||
            state.encoder_active[channel_b] ||
            state.pwm_capture_active[channel_b];

        if (channel_a_busy) {
            return "ERR CHANNEL_A_BUSY\n";
        }

        if (channel_b_busy) {
            return "ERR CHANNEL_B_BUSY\n";
        }

        InputCaptureConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateDualchannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel_a),
                    VMXChannelCapability::InputCaptureInput),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel_b),
                    VMXChannelCapability::InputCaptureInput2),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.InputCapture_Reset(handle, &error)) {
            VMXErrorCode cleanup_error = 0;
            vmx.io.DeallocateResource(handle, &cleanup_error);
            return make_vmx_error(error);
        }

        state.input_capture_active[channel_a] = true;
        state.input_capture_handles[channel_a] = handle;
        state.input_capture_partner[channel_a] = channel_b;
        state.input_capture_is_primary[channel_a] = true;

        state.input_capture_active[channel_b] = true;
        state.input_capture_handles[channel_b] = handle;
        state.input_capture_partner[channel_b] = channel_a;
        state.input_capture_is_primary[channel_b] = false;

        char response[160];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL_A=%d CHANNEL_B=%d MODE=INPUT_CAPTURE "
            "TICK_US=%lu COUNT=0\n",
            channel_a,
            channel_b,
            static_cast<unsigned long>(
                config.GetMicrosecondsPerTick()
            )
        );

        return std::string(response);
    }

    if (command.rfind("INPUT_CAPTURE_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "INPUT_CAPTURE_READ %d",
                &channel) != 1) {
            return "ERR USAGE=INPUT_CAPTURE_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.input_capture_active[channel]) {
            return "ERR INPUT_CAPTURE_NOT_OPEN\n";
        }

        uint32_t channel_1_count = 0;
        uint32_t channel_2_count = 0;
        int32_t count = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.InputCapture_GetChannelCounts(
                state.input_capture_handles[channel],
                channel_1_count,
                channel_2_count,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.InputCapture_GetCount(
                state.input_capture_handles[channel],
                count,
                &error)) {
            return make_vmx_error(error);
        }

        char response[176];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "CH1=%lu CH2=%lu COUNT=%ld\n",
            channel,
            state.input_capture_is_primary[channel]
                ? "CH1"
                : "CH2",
            state.input_capture_partner[channel],
            static_cast<unsigned long>(channel_1_count),
            static_cast<unsigned long>(channel_2_count),
            static_cast<long>(count)
        );

        return std::string(response);
    }

    if (command.rfind("INPUT_CAPTURE_STATUS", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "INPUT_CAPTURE_STATUS %d",
                &channel) != 1) {
            return "ERR USAGE=INPUT_CAPTURE_STATUS_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.input_capture_active[channel]) {
            return "ERR INPUT_CAPTURE_NOT_OPEN\n";
        }

        bool forward_direction = false;
        bool active = false;
        VMXErrorCode error = 0;

        if (!vmx.io.InputCapture_InputStatus(
                state.input_capture_handles[channel],
                forward_direction,
                active,
                &error)) {
            return make_vmx_error(error);
        }

        char response[144];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d PARTNER=%d DIRECTION=%s ACTIVE=%d\n",
            channel,
            state.input_capture_partner[channel],
            forward_direction ? "FORWARD" : "REVERSE",
            active ? 1 : 0
        );

        return std::string(response);
    }

    if (command.rfind("INPUT_CAPTURE_RESET", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "INPUT_CAPTURE_RESET %d",
                &channel) != 1) {
            return "ERR USAGE=INPUT_CAPTURE_RESET_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.input_capture_active[channel]) {
            return "ERR INPUT_CAPTURE_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.InputCapture_Reset(
                state.input_capture_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d PARTNER=%d COUNT=0\n",
            channel,
            state.input_capture_partner[channel]
        );

        return std::string(response);
    }

    if (command.rfind("PWM_CAPTURE_OPEN", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "PWM_CAPTURE_OPEN %d",
                &channel) != 1) {
            return "ERR USAGE=PWM_CAPTURE_OPEN_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (state.pwm_capture_active[channel]) {
            char response[112];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=PWM_CAPTURE ALREADY_OPEN=1\n",
                channel
            );

            return std::string(response);
        }

        const bool channel_busy =
            state.dio_output_active[channel] ||
            state.dio_input_active[channel] ||
            state.pwm_active[channel] ||
            state.analog_active[channel] ||
            state.encoder_active[channel] ||
            state.input_capture_active[channel];

        if (channel_busy) {
            return "ERR CHANNEL_BUSY\n";
        }

        VMXChannelCapability capability =
            VMXChannelCapability::NoCapabilities;

        if (vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::InputCaptureInput)) {
            capability = VMXChannelCapability::InputCaptureInput;
        } else if (vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::InputCaptureInput2)) {
            capability = VMXChannelCapability::InputCaptureInput2;
        } else {
            return "ERR CHANNEL_NOT_PWM_CAPTURE_CAPABLE\n";
        }

        PWMCaptureConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    capability),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.InputCapture_Reset(handle, &error)) {
            VMXErrorCode cleanup_error = 0;
            vmx.io.DeallocateResource(handle, &cleanup_error);
            return make_vmx_error(error);
        }

        state.pwm_capture_active[channel] = true;
        state.pwm_capture_handles[channel] = handle;

        char response[160];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=PWM_CAPTURE "
            "TICK_US=%lu TIMEOUT_20MS=%u\n",
            channel,
            static_cast<unsigned long>(
                config.GetMicrosecondsPerTick()
            ),
            static_cast<unsigned int>(
                config.GetStallTimeout20MsPeriods()
            )
        );

        return std::string(response);
    }

    if (command.rfind("PWM_CAPTURE_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "PWM_CAPTURE_READ %d",
                &channel) != 1) {
            return "ERR USAGE=PWM_CAPTURE_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.pwm_capture_active[channel]) {
            return "ERR PWM_CAPTURE_NOT_OPEN\n";
        }

        uint32_t frequency_microseconds = 0;
        uint32_t duty_cycle_microseconds = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.PWMCapture_GetCount(
                state.pwm_capture_handles[channel],
                frequency_microseconds,
                duty_cycle_microseconds,
                &error)) {
            return make_vmx_error(error);
        }

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d FREQUENCY_US=%lu DUTY_US=%lu\n",
            channel,
            static_cast<unsigned long>(
                frequency_microseconds
            ),
            static_cast<unsigned long>(
                duty_cycle_microseconds
            )
        );

        return std::string(response);
    }

    if (command.rfind("PWM_CAPTURE_RESET", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "PWM_CAPTURE_RESET %d",
                &channel) != 1) {
            return "ERR USAGE=PWM_CAPTURE_RESET_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.pwm_capture_active[channel]) {
            return "ERR PWM_CAPTURE_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.InputCapture_Reset(
                state.pwm_capture_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d RESET=1\n",
            channel
        );

        return std::string(response);
    }

    if (command.rfind("ENCODER_OPEN", 0) == 0) {
        int channel_a = -1;
        int channel_b = -1;

        if (std::sscanf(
                command.c_str(),
                "ENCODER_OPEN %d %d",
                &channel_a,
                &channel_b) != 2) {
            return "ERR USAGE=ENCODER_OPEN_<channel_a>_<channel_b>\n";
        }

        if (channel_a < 0 ||
            channel_a >= static_cast<int>(MAX_VMX_CHANNELS) ||
            channel_b < 0 ||
            channel_b >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (channel_a == channel_b) {
            return "ERR ENCODER_CHANNELS_MUST_DIFFER\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel_a),
                VMXChannelCapability::EncoderAInput)) {
            return "ERR CHANNEL_A_NOT_ENCODER_A_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel_b),
                VMXChannelCapability::EncoderBInput)) {
            return "ERR CHANNEL_B_NOT_ENCODER_B_CAPABLE\n";
        }

        if (state.encoder_active[channel_a] ||
            state.encoder_active[channel_b]) {

            if (state.encoder_active[channel_a] &&
                state.encoder_active[channel_b] &&
                state.encoder_handles[channel_a] ==
                    state.encoder_handles[channel_b] &&
                state.encoder_partner[channel_a] == channel_b &&
                state.encoder_partner[channel_b] == channel_a &&
                state.encoder_is_a[channel_a] &&
                !state.encoder_is_a[channel_b]) {

                char response[128];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL_A=%d CHANNEL_B=%d "
                    "MODE=ENCODER EDGES=X4 ALREADY_OPEN=1\n",
                    channel_a,
                    channel_b
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        const bool channel_a_busy =
            state.dio_output_active[channel_a] ||
            state.dio_input_active[channel_a] ||
            state.pwm_active[channel_a] ||
            state.analog_active[channel_a] ||
            state.input_capture_active[channel_a] ||
            state.pwm_capture_active[channel_a];

        const bool channel_b_busy =
            state.dio_output_active[channel_b] ||
            state.dio_input_active[channel_b] ||
            state.pwm_active[channel_b] ||
            state.analog_active[channel_b] ||
            state.input_capture_active[channel_b] ||
            state.pwm_capture_active[channel_b];

        if (channel_a_busy) {
            return "ERR CHANNEL_A_BUSY\n";
        }

        if (channel_b_busy) {
            return "ERR CHANNEL_B_BUSY\n";
        }

        EncoderConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateDualchannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel_a),
                    VMXChannelCapability::EncoderAInput),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel_b),
                    VMXChannelCapability::EncoderBInput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Encoder_Reset(handle, &error)) {
            VMXErrorCode cleanup_error = 0;
            vmx.io.DeallocateResource(handle, &cleanup_error);
            return make_vmx_error(error);
        }

        state.encoder_active[channel_a] = true;
        state.encoder_handles[channel_a] = handle;
        state.encoder_partner[channel_a] = channel_b;
        state.encoder_is_a[channel_a] = true;

        state.encoder_active[channel_b] = true;
        state.encoder_handles[channel_b] = handle;
        state.encoder_partner[channel_b] = channel_a;
        state.encoder_is_a[channel_b] = false;

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL_A=%d CHANNEL_B=%d "
            "MODE=ENCODER EDGES=X4 COUNT=0\n",
            channel_a,
            channel_b
        );

        return std::string(response);
    }

    if (command.rfind("ENCODER_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ENCODER_READ %d",
                &channel) != 1) {
            return "ERR USAGE=ENCODER_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.encoder_active[channel]) {
            return "ERR ENCODER_NOT_OPEN\n";
        }

        int32_t count = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.Encoder_GetCount(
                state.encoder_handles[channel],
                count,
                &error)) {
            return make_vmx_error(error);
        }

        char response[144];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%c PARTNER=%d COUNT=%ld\n",
            channel,
            state.encoder_is_a[channel] ? 'A' : 'B',
            state.encoder_partner[channel],
            static_cast<long>(count)
        );

        return std::string(response);
    }

    if (command.rfind("ENCODER_DIRECTION", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ENCODER_DIRECTION %d",
                &channel) != 1) {
            return "ERR USAGE=ENCODER_DIRECTION_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.encoder_active[channel]) {
            return "ERR ENCODER_NOT_OPEN\n";
        }

        VMXIO::EncoderDirection direction =
            VMXIO::EncoderForward;

        VMXErrorCode error = 0;

        if (!vmx.io.Encoder_GetDirection(
                state.encoder_handles[channel],
                direction,
                &error)) {
            return make_vmx_error(error);
        }

        const char* direction_text =
            direction == VMXIO::EncoderForward
                ? "FORWARD"
                : "REVERSE";

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%c PARTNER=%d DIRECTION=%s\n",
            channel,
            state.encoder_is_a[channel] ? 'A' : 'B',
            state.encoder_partner[channel],
            direction_text
        );

        return std::string(response);
    }

    if (command.rfind("ENCODER_PERIOD", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ENCODER_PERIOD %d",
                &channel) != 1) {
            return "ERR USAGE=ENCODER_PERIOD_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.encoder_active[channel]) {
            return "ERR ENCODER_NOT_OPEN\n";
        }

        uint16_t period_microseconds = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.Encoder_GetLastPulsePeriodMicroseconds(
                state.encoder_handles[channel],
                period_microseconds,
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%c PARTNER=%d PERIOD_US=%u\n",
            channel,
            state.encoder_is_a[channel] ? 'A' : 'B',
            state.encoder_partner[channel],
            static_cast<unsigned int>(period_microseconds)
        );

        return std::string(response);
    }

    if (command.rfind("ENCODER_RESET", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ENCODER_RESET %d",
                &channel) != 1) {
            return "ERR USAGE=ENCODER_RESET_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.encoder_active[channel]) {
            return "ERR ENCODER_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.Encoder_Reset(
                state.encoder_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d PARTNER=%d COUNT=0\n",
            channel,
            state.encoder_partner[channel]
        );

        return std::string(response);
    }

    if (command == "GET_RTC_TIME") {
        RTCDateTime rtc {};
        VMXErrorCode error = 0;

        if (!read_rtc_time(
                vmx,
                rtc,
                error)) {
            return make_vmx_error(error);
        }

        if (!valid_rtc_datetime(
                rtc.year,
                rtc.month,
                rtc.day,
                rtc.hour,
                rtc.minute,
                rtc.second) ||
            rtc.weekday < 1 ||
            rtc.weekday > 7 ||
            rtc.millisecond < 0 ||
            rtc.millisecond > 999) {
            return "ERR INVALID_RTC_VALUE\n";
        }

        char response[224];

        std::snprintf(
            response,
            sizeof(response),
            "OK RTC YEAR=%d MONTH=%d DAY=%d "
            "WEEKDAY=%d HOUR=%d MINUTE=%d "
            "SECOND=%d MILLISECOND=%d\n",
            rtc.year,
            rtc.month,
            rtc.day,
            rtc.weekday,
            rtc.hour,
            rtc.minute,
            rtc.second,
            rtc.millisecond
        );

        return std::string(response);
    }

    if (command.rfind(
            "SET_RTC_TIME",
            0) == 0) {

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;

        if (std::sscanf(
                command.c_str(),
                "SET_RTC_TIME %d %d %d %d %d",
                &year,
                &month,
                &day,
                &hour,
                &minute) != 5) {
            return
                "ERR USAGE=SET_RTC_TIME_"
                "<year>_<month>_<day>_"
                "<hour>_<minute>\n";
        }

        if (!valid_rtc_datetime(
                year,
                month,
                day,
                hour,
                minute,
                0)) {
            return "ERR INVALID_RTC_DATE_TIME\n";
        }

        VMXErrorCode error = 0;

        if (!set_rtc_time(
                vmx,
                year,
                month,
                day,
                hour,
                minute,
                error)) {

            if (error != 0) {
                return make_vmx_error(error);
            }

            return "ERR RTC_TIME_CONVERSION_FAILED\n";
        }

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK RTC_SET YEAR=%d MONTH=%d DAY=%d "
            "HOUR=%d MINUTE=%d SECOND=0\n",
            year,
            month,
            day,
            hour,
            minute
        );

        return std::string(response);
    }

    if (command == "SYNC_LINUX_TIME_FROM_RTC") {
        RTCDateTime rtc {};
        VMXErrorCode error = 0;
        int system_error = 0;

        if (!sync_linux_time_from_rtc(
                vmx,
                rtc,
                error,
                system_error)) {

            if (error != 0) {
                return make_vmx_error(error);
            }

            char response[160];

            std::snprintf(
                response,
                sizeof(response),
                "ERR LINUX_TIME_SYNC_FAILED "
                "ERRNO=%d MESSAGE=%s\n",
                system_error,
                std::strerror(system_error)
            );

            return std::string(response);
        }

        char response[256];

        std::snprintf(
            response,
            sizeof(response),
            "OK LINUX_TIME_SYNCED_FROM_RTC "
            "YEAR=%d MONTH=%d DAY=%d "
            "HOUR=%d MINUTE=%d SECOND=%d "
            "MILLISECOND=%d\n",
            rtc.year,
            rtc.month,
            rtc.day,
            rtc.hour,
            rtc.minute,
            rtc.second,
            rtc.millisecond
        );

        return std::string(response);
    }

    if (command == "IMU_STATUS") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const bool connected =
            ahrs.IsConnected();

        const bool calibrating =
            ahrs.IsCalibrating();

        const bool moving =
            ahrs.IsMoving();

        const bool rotating =
            ahrs.IsRotating();

        const bool magnetometer_calibrated =
            ahrs.IsMagnetometerCalibrated();

        const bool magnetic_disturbance =
            ahrs.IsMagneticDisturbance();

        const bool altitude_valid =
            ahrs.IsAltitudeValid();

        const int actual_rate =
            ahrs.GetActualUpdateRate();

        const int requested_rate =
            ahrs.GetRequestedUpdateRate();

        const long sensor_timestamp =
            ahrs.GetLastSensorTimestamp();

        const double update_count =
            ahrs.GetUpdateCount();

        const double byte_count =
            ahrs.GetByteCount();

        const std::string firmware =
            ahrs.GetFirmwareVersion();

        const BoardYawAxis yaw_axis =
            ahrs.GetBoardYawAxis();

        const char* axis_name = "UNKNOWN";

        switch (yaw_axis.board_axis) {
        case kBoardAxisX:
            axis_name = "X";
            break;
        case kBoardAxisY:
            axis_name = "Y";
            break;
        case kBoardAxisZ:
            axis_name = "Z";
            break;
        default:
            axis_name = "UNKNOWN";
            break;
        }

        char response[512];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_STATUS CONNECTED=%d "
            "CALIBRATING=%d MOVING=%d ROTATING=%d "
            "MAG_CALIBRATED=%d "
            "MAG_DISTURBANCE=%d ALTITUDE_VALID=%d "
            "ACTUAL_RATE_HZ=%d REQUESTED_RATE_HZ=%d "
            "SENSOR_TIMESTAMP=%ld UPDATE_COUNT=%.0f "
            "BYTE_COUNT=%.0f FW=%s "
            "YAW_AXIS=%s YAW_AXIS_UP=%d\n",
            connected ? 1 : 0,
            calibrating ? 1 : 0,
            moving ? 1 : 0,
            rotating ? 1 : 0,
            magnetometer_calibrated ? 1 : 0,
            magnetic_disturbance ? 1 : 0,
            altitude_valid ? 1 : 0,
            actual_rate,
            requested_rate,
            sensor_timestamp,
            update_count,
            byte_count,
            firmware.c_str(),
            axis_name,
            yaw_axis.up ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "IMU_ORIENTATION") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float yaw =
            ahrs.GetYaw();

        const float pitch =
            ahrs.GetPitch();

        const float roll =
            ahrs.GetRoll();

        const float compass =
            ahrs.GetCompassHeading();

        const float fused =
            ahrs.GetFusedHeading();

        const double angle =
            ahrs.GetAngle();

        const double rate =
            ahrs.GetRate();

        char response[320];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_ORIENTATION "
            "YAW=%.6f PITCH=%.6f ROLL=%.6f "
            "COMPASS=%.6f FUSED=%.6f "
            "ANGLE=%.6f RATE=%.6f\n",
            yaw,
            pitch,
            roll,
            compass,
            fused,
            angle,
            rate
        );

        return std::string(response);
    }

    if (command == "IMU_QUATERNION") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float w =
            ahrs.GetQuaternionW();

        const float x =
            ahrs.GetQuaternionX();

        const float y =
            ahrs.GetQuaternionY();

        const float z =
            ahrs.GetQuaternionZ();

        char response[224];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_QUATERNION "
            "W=%.6f X=%.6f Y=%.6f Z=%.6f\n",
            w,
            x,
            y,
            z
        );

        return std::string(response);
    }

    if (command == "IMU_LINEAR_ACCEL") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float x =
            ahrs.GetWorldLinearAccelX();

        const float y =
            ahrs.GetWorldLinearAccelY();

        const float z =
            ahrs.GetWorldLinearAccelZ();

        const bool moving =
            ahrs.IsMoving();

        const bool rotating =
            ahrs.IsRotating();

        char response[224];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_LINEAR_ACCEL "
            "X_G=%.6f Y_G=%.6f Z_G=%.6f "
            "MOVING=%d ROTATING=%d\n",
            x,
            y,
            z,
            moving ? 1 : 0,
            rotating ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "IMU_RAW") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float gyro_x =
            ahrs.GetRawGyroX();

        const float gyro_y =
            ahrs.GetRawGyroY();

        const float gyro_z =
            ahrs.GetRawGyroZ();

        const float accel_x =
            ahrs.GetRawAccelX();

        const float accel_y =
            ahrs.GetRawAccelY();

        const float accel_z =
            ahrs.GetRawAccelZ();

        const float mag_x =
            ahrs.GetRawMagX();

        const float mag_y =
            ahrs.GetRawMagY();

        const float mag_z =
            ahrs.GetRawMagZ();

        const float temperature =
            ahrs.GetTempC();

        char response[448];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_RAW "
            "GYRO_X=%.6f GYRO_Y=%.6f GYRO_Z=%.6f "
            "ACCEL_X=%.6f ACCEL_Y=%.6f ACCEL_Z=%.6f "
            "MAG_X=%.6f MAG_Y=%.6f MAG_Z=%.6f "
            "TEMP_C=%.6f\n",
            gyro_x,
            gyro_y,
            gyro_z,
            accel_x,
            accel_y,
            accel_z,
            mag_x,
            mag_y,
            mag_z,
            temperature
        );

        return std::string(response);
    }

    if (command == "IMU_VELOCITY") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float x =
            ahrs.GetVelocityX();

        const float y =
            ahrs.GetVelocityY();

        const float z =
            ahrs.GetVelocityZ();

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_VELOCITY "
            "X_MPS=%.6f Y_MPS=%.6f Z_MPS=%.6f\n",
            x,
            y,
            z
        );

        return std::string(response);
    }

    if (command == "IMU_DISPLACEMENT") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float x =
            ahrs.GetDisplacementX();

        const float y =
            ahrs.GetDisplacementY();

        const float z =
            ahrs.GetDisplacementZ();

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_DISPLACEMENT "
            "X_M=%.6f Y_M=%.6f Z_M=%.6f\n",
            x,
            y,
            z
        );

        return std::string(response);
    }

    if (command == "IMU_ENVIRONMENT") {
        vmx::AHRS& ahrs = vmx.getAHRS();

        const float altitude =
            ahrs.GetAltitude();

        const float barometric_pressure =
            ahrs.GetBarometricPressure();

        const float pressure =
            ahrs.GetPressure();

        const float temperature =
            ahrs.GetTempC();

        const bool altitude_valid =
            ahrs.IsAltitudeValid();

        const bool magnetometer_calibrated =
            ahrs.IsMagnetometerCalibrated();

        const bool magnetic_disturbance =
            ahrs.IsMagneticDisturbance();

        char response[320];

        std::snprintf(
            response,
            sizeof(response),
            "OK IMU_ENVIRONMENT "
            "ALTITUDE_M=%.6f ALTITUDE_VALID=%d "
            "BARO_PRESSURE=%.6f PRESSURE=%.6f "
            "TEMP_C=%.6f MAG_CALIBRATED=%d "
            "MAG_DISTURBANCE=%d\n",
            altitude,
            altitude_valid ? 1 : 0,
            barometric_pressure,
            pressure,
            temperature,
            magnetometer_calibrated ? 1 : 0,
            magnetic_disturbance ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "IMU_ZERO_YAW") {
        vmx.getAHRS().ZeroYaw();

        return "OK IMU_ZERO_YAW=1\n";
    }

    if (command == "IMU_RESET_DISPLACEMENT") {
        vmx.getAHRS().ResetDisplacement();

        return "OK IMU_RESET_DISPLACEMENT=1\n";
    }

    if (command.rfind("LED_ARRAY_OPEN", 0) == 0) {
        int channel = -1;
        int pixel_count = 0;
        char format_text[32] = {0};

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_OPEN %d %d %31s",
                &channel,
                &pixel_count,
                format_text) != 3) {
            return
                "ERR USAGE=LED_ARRAY_OPEN_"
                "<channel>_<pixels>_"
                "<RGB|RBG|GRB|GBR|BRG|BGR>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (pixel_count < 1 ||
            pixel_count > MAX_LED_ARRAY_PIXELS) {
            return "ERR PIXEL_COUNT_RANGE=1..1024\n";
        }

        const std::string requested_format(format_text);

        if (
            requested_format == "RGBW" ||
            requested_format == "RBGW" ||
            requested_format == "GRBW" ||
            requested_format == "GBRW" ||
            requested_format == "BRGW" ||
            requested_format == "BGRW"
        ) {
            return
                "ERR RGBW_FORMAT_NOT_SUPPORTED_"
                "BY_HAL_BUFFER_API\n";
        }

        LEDArray_OneWireConfig::PixelFormat pixel_format =
            LEDArray_OneWireConfig::GRB;

        uint8_t format_code = 255;

        if (!parse_led_array_format(
                requested_format,
                pixel_format,
                format_code)) {
            return "ERR INVALID_PIXEL_FORMAT\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::LEDArray_OneWire)) {
            return "ERR CHANNEL_NOT_LED_ARRAY_CAPABLE\n";
        }

        if (state.led_array_active[channel]) {
            if (
                state.led_array_pixels[channel] ==
                    pixel_count &&
                state.led_array_format[channel] ==
                    format_code
            ) {
                char response[192];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL=%d MODE=LED_ARRAY "
                    "PIXELS=%d FORMAT=%s "
                    "FREQUENCY_HZ=800000 "
                    "ALREADY_OPEN=1\n",
                    channel,
                    pixel_count,
                    format_text
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_LED_ARRAY\n";
        }

        const bool channel_busy =
            state.dio_output_active[channel] ||
            state.dio_input_active[channel] ||
            state.pwm_active[channel] ||
            state.analog_active[channel] ||
            state.analog_trigger_active[channel] ||
            state.encoder_active[channel] ||
            state.input_capture_active[channel] ||
            state.pwm_capture_active[channel] ||
            state.uart_active[channel] ||
            state.spi_active[channel] ||
            state.i2c_active[channel];

        if (channel_busy) {
            return "ERR LED_ARRAY_CHANNEL_BUSY\n";
        }

        LEDArrayBufferHandle buffer = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.LEDArrayBuffer_Create(
                pixel_count,
                buffer,
                &error)) {
            return make_vmx_error(error);
        }

        LEDArray_OneWireConfig config(
            pixel_count,
            800000
        );

        config.SetPixelFormat(pixel_format);

        VMXResourceHandle handle = 0;
        error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::LEDArray_OneWire),
                &config,
                handle,
                &error)) {

            VMXErrorCode cleanup_error = 0;

            vmx.io.LEDArrayBuffer_Delete(
                buffer,
                &cleanup_error
            );

            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.LEDArray_SetBuffer(
                handle,
                buffer,
                &error)) {

            VMXErrorCode cleanup_error = 0;

            vmx.io.DeallocateResource(
                handle,
                &cleanup_error
            );

            cleanup_error = 0;

            vmx.io.LEDArrayBuffer_Delete(
                buffer,
                &cleanup_error
            );

            return make_vmx_error(error);
        }

        for (int index = 0;
             index < pixel_count;
             ++index) {

            error = 0;

            if (!vmx.io.LEDArrayBuffer_SetRGBValue(
                    buffer,
                    index,
                    0,
                    0,
                    0,
                    &error)) {

                VMXErrorCode cleanup_error = 0;

                vmx.io.DeallocateResource(
                    handle,
                    &cleanup_error
                );


                return make_vmx_error(error);
            }
        }

        error = 0;

        if (!vmx.io.LEDArray_Render(
                handle,
                &error)) {

            VMXErrorCode cleanup_error = 0;

            vmx.io.DeallocateResource(
                handle,
                &cleanup_error
            );


            return make_vmx_error(error);
        }

        state.led_array_active[channel] = true;
        state.led_array_handles[channel] = handle;
        state.led_array_buffers[channel] = buffer;
        state.led_array_pixels[channel] = pixel_count;
        state.led_array_format[channel] = format_code;

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=LED_ARRAY "
            "PIXELS=%d FORMAT=%s "
            "FREQUENCY_HZ=800000\n",
            channel,
            pixel_count,
            format_text
        );

        return std::string(response);
    }

    if (command.rfind(
            "LED_ARRAY_SET_PIXEL",
            0) == 0) {

        int channel = -1;
        int index = -1;
        int red = -1;
        int green = -1;
        int blue = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_SET_PIXEL %d %d %d %d %d",
                &channel,
                &index,
                &red,
                &green,
                &blue) != 5) {
            return
                "ERR USAGE=LED_ARRAY_SET_PIXEL_"
                "<channel>_<index>_<r>_<g>_<b>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        if (index < 0 ||
            index >= state.led_array_pixels[channel]) {
            return "ERR PIXEL_INDEX_OUT_OF_RANGE\n";
        }

        if (!led_array_rgb_valid(
                red,
                green,
                blue)) {
            return "ERR RGB_RANGE=0..255\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.LEDArrayBuffer_SetRGBValue(
                state.led_array_buffers[channel],
                index,
                red,
                green,
                blue,
                &error)) {
            return make_vmx_error(error);
        }

        char response[144];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d INDEX=%d "
            "R=%d G=%d B=%d UPDATED=1\n",
            channel,
            index,
            red,
            green,
            blue
        );

        return std::string(response);
    }

    if (command.rfind(
            "LED_ARRAY_GET_PIXEL",
            0) == 0) {

        int channel = -1;
        int index = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_GET_PIXEL %d %d",
                &channel,
                &index) != 2) {
            return
                "ERR USAGE=LED_ARRAY_GET_PIXEL_"
                "<channel>_<index>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        if (index < 0 ||
            index >= state.led_array_pixels[channel]) {
            return "ERR PIXEL_INDEX_OUT_OF_RANGE\n";
        }

        int red = 0;
        int green = 0;
        int blue = 0;
        VMXErrorCode error = 0;

        // GetRBGValue is the exported HAL method name.
        if (!vmx.io.LEDArrayBuffer_GetRBGValue(
                state.led_array_buffers[channel],
                index,
                red,
                green,
                blue,
                &error)) {
            return make_vmx_error(error);
        }

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d INDEX=%d "
            "R=%d G=%d B=%d\n",
            channel,
            index,
            red,
            green,
            blue
        );

        return std::string(response);
    }

    if (command.rfind("LED_ARRAY_FILL", 0) == 0) {
        int channel = -1;
        int red = -1;
        int green = -1;
        int blue = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_FILL %d %d %d %d",
                &channel,
                &red,
                &green,
                &blue) != 4) {
            return
                "ERR USAGE=LED_ARRAY_FILL_"
                "<channel>_<r>_<g>_<b>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        if (!led_array_rgb_valid(
                red,
                green,
                blue)) {
            return "ERR RGB_RANGE=0..255\n";
        }

        for (int index = 0;
             index < state.led_array_pixels[channel];
             ++index) {

            VMXErrorCode error = 0;

            if (!vmx.io.LEDArrayBuffer_SetRGBValue(
                    state.led_array_buffers[channel],
                    index,
                    red,
                    green,
                    blue,
                    &error)) {
                return make_vmx_error(error);
            }
        }

        char response[144];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d PIXELS=%d "
            "R=%d G=%d B=%d FILLED=1\n",
            channel,
            state.led_array_pixels[channel],
            red,
            green,
            blue
        );

        return std::string(response);
    }

    if (command.rfind("LED_ARRAY_SHOW", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_SHOW %d",
                &channel) != 1) {
            return "ERR USAGE=LED_ARRAY_SHOW_<channel>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.LEDArray_Render(
                state.led_array_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d RENDERED=1\n",
            channel
        );

        return std::string(response);
    }

    if (command.rfind("LED_ARRAY_CLEAR", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_CLEAR %d",
                &channel) != 1) {
            return "ERR USAGE=LED_ARRAY_CLEAR_<channel>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        for (int index = 0;
             index < state.led_array_pixels[channel];
             ++index) {

            VMXErrorCode error = 0;

            if (!vmx.io.LEDArrayBuffer_SetRGBValue(
                    state.led_array_buffers[channel],
                    index,
                    0,
                    0,
                    0,
                    &error)) {
                return make_vmx_error(error);
            }
        }

        VMXErrorCode error = 0;

        if (!vmx.io.LEDArray_Render(
                state.led_array_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d PIXELS=%d CLEARED=1\n",
            channel,
            state.led_array_pixels[channel]
        );

        return std::string(response);
    }

    if (command.rfind(
            "LED_ARRAY_RELEASE",
            0) == 0) {

        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "LED_ARRAY_RELEASE %d",
                &channel) != 1) {
            return
                "ERR USAGE=LED_ARRAY_RELEASE_<channel>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.led_array_active[channel]) {
            return "ERR LED_ARRAY_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;
        unsigned int released = 0;

        if (!release_channel(
                vmx,
                state,
                static_cast<std::size_t>(channel),
                error,
                released)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d LED_ARRAY_RELEASED=%u\n",
            channel,
            released
        );

        return std::string(response);
    }

    if (command.rfind("I2C_OPEN", 0) == 0) {
        int sda_channel = -1;
        int scl_channel = -1;

        if (std::sscanf(
                command.c_str(),
                "I2C_OPEN %d %d",
                &sda_channel,
                &scl_channel) != 2) {
            return
                "ERR USAGE=I2C_OPEN_"
                "<sda_channel>_<scl_channel>\n";
        }

        if (sda_channel < 0 ||
            sda_channel >=
                static_cast<int>(MAX_VMX_CHANNELS) ||
            scl_channel < 0 ||
            scl_channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (sda_channel == scl_channel) {
            return "ERR I2C_CHANNELS_MUST_DIFFER\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    sda_channel
                ),
                VMXChannelCapability::I2C_SDA)) {
            return "ERR CHANNEL_NOT_I2C_SDA_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    scl_channel
                ),
                VMXChannelCapability::I2C_SCL)) {
            return "ERR CHANNEL_NOT_I2C_SCL_CAPABLE\n";
        }

        if (state.i2c_active[sda_channel] ||
            state.i2c_active[scl_channel]) {

            if (state.i2c_active[sda_channel] &&
                state.i2c_active[scl_channel] &&
                state.i2c_handles[sda_channel] ==
                    state.i2c_handles[scl_channel] &&
                state.i2c_partner[sda_channel] ==
                    scl_channel &&
                state.i2c_partner[scl_channel] ==
                    sda_channel &&
                state.i2c_is_sda[sda_channel] &&
                !state.i2c_is_sda[scl_channel]) {

                char response[160];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL_SDA=%d CHANNEL_SCL=%d "
                    "MODE=I2C ALREADY_OPEN=1\n",
                    sda_channel,
                    scl_channel
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_I2C\n";
        }

        const bool sda_busy =
            state.dio_output_active[sda_channel] ||
            state.dio_input_active[sda_channel] ||
            state.pwm_active[sda_channel] ||
            state.analog_active[sda_channel] ||
            state.analog_trigger_active[sda_channel] ||
            state.encoder_active[sda_channel] ||
            state.input_capture_active[sda_channel] ||
            state.pwm_capture_active[sda_channel] ||
            state.uart_active[sda_channel] ||
            state.spi_active[sda_channel] ||
            state.led_array_active[sda_channel];

        const bool scl_busy =
            state.dio_output_active[scl_channel] ||
            state.dio_input_active[scl_channel] ||
            state.pwm_active[scl_channel] ||
            state.analog_active[scl_channel] ||
            state.analog_trigger_active[scl_channel] ||
            state.encoder_active[scl_channel] ||
            state.input_capture_active[scl_channel] ||
            state.pwm_capture_active[scl_channel] ||
            state.uart_active[scl_channel] ||
            state.spi_active[scl_channel] ||
            state.led_array_active[scl_channel];

        if (sda_busy) {
            return "ERR I2C_SDA_CHANNEL_BUSY\n";
        }

        if (scl_busy) {
            return "ERR I2C_SCL_CHANNEL_BUSY\n";
        }

        I2CConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateDualchannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        sda_channel
                    ),
                    VMXChannelCapability::I2C_SDA),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        scl_channel
                    ),
                    VMXChannelCapability::I2C_SCL),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        state.i2c_active[sda_channel] = true;
        state.i2c_handles[sda_channel] = handle;
        state.i2c_partner[sda_channel] = scl_channel;
        state.i2c_is_sda[sda_channel] = true;

        state.i2c_active[scl_channel] = true;
        state.i2c_handles[scl_channel] = handle;
        state.i2c_partner[scl_channel] = sda_channel;
        state.i2c_is_sda[scl_channel] = false;

        char response[160];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL_SDA=%d CHANNEL_SCL=%d "
            "MODE=I2C\n",
            sda_channel,
            scl_channel
        );

        return std::string(response);
    }

    if (command.rfind("I2C_WRITE_HEX", 0) == 0) {
        int channel = -1;
        unsigned int device_address = 0;
        unsigned int register_address = 0;

        char hex_text[
            (MAX_I2C_TRANSFER_BYTES * 2) + 1
        ] = {0};

        if (std::sscanf(
                command.c_str(),
                "I2C_WRITE_HEX %d %u %u %2048s",
                &channel,
                &device_address,
                &register_address,
                hex_text) != 4) {
            return
                "ERR USAGE=I2C_WRITE_HEX_"
                "<channel>_<device_address>_"
                "<register_address>_<hex_data>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (device_address > 127) {
            return "ERR DEVICE_ADDRESS_RANGE=0..127\n";
        }

        if (register_address > 255) {
            return "ERR REGISTER_ADDRESS_RANGE=0..255\n";
        }

        if (!state.i2c_active[channel]) {
            return "ERR I2C_NOT_OPEN\n";
        }

        std::vector<uint8_t> data;

        if (!decode_hex(hex_text, data) ||
            data.size() > MAX_I2C_TRANSFER_BYTES) {
            return
                "ERR INVALID_HEX_OR_SIZE_"
                "MAX_BYTES=1024\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.I2C_Write(
                state.i2c_handles[channel],
                static_cast<uint8_t>(device_address),
                static_cast<uint8_t>(register_address),
                data.data(),
                static_cast<int32_t>(data.size()),
                &error)) {
            return make_vmx_error(error);
        }

        char response[160];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "DEVICE=%u REGISTER=%u WRITTEN=%u\n",
            channel,
            i2c_role_name(state.i2c_is_sda[channel]),
            state.i2c_partner[channel],
            device_address,
            register_address,
            static_cast<unsigned int>(data.size())
        );

        return std::string(response);
    }

    if (command.rfind("I2C_READ_HEX", 0) == 0) {
        int channel = -1;
        unsigned int device_address = 0;
        unsigned int register_address = 0;
        unsigned int byte_count = 0;

        if (std::sscanf(
                command.c_str(),
                "I2C_READ_HEX %d %u %u %u",
                &channel,
                &device_address,
                &register_address,
                &byte_count) != 4) {
            return
                "ERR USAGE=I2C_READ_HEX_"
                "<channel>_<device_address>_"
                "<register_address>_<byte_count>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (device_address > 127) {
            return "ERR DEVICE_ADDRESS_RANGE=0..127\n";
        }

        if (register_address > 255) {
            return "ERR REGISTER_ADDRESS_RANGE=0..255\n";
        }

        if (byte_count == 0 ||
            byte_count > MAX_I2C_TRANSFER_BYTES) {
            return "ERR BYTE_COUNT_RANGE=1..1024\n";
        }

        if (!state.i2c_active[channel]) {
            return "ERR I2C_NOT_OPEN\n";
        }

        std::vector<uint8_t> data(byte_count, 0);
        VMXErrorCode error = 0;

        if (!vmx.io.I2C_Read(
                state.i2c_handles[channel],
                static_cast<uint8_t>(device_address),
                static_cast<uint8_t>(register_address),
                data.data(),
                static_cast<int32_t>(data.size()),
                &error)) {
            return make_vmx_error(error);
        }

        const std::string hex =
            encode_hex(data.data(), data.size());

        char prefix[192];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "DEVICE=%u REGISTER=%u SIZE=%u HEX=",
            channel,
            i2c_role_name(state.i2c_is_sda[channel]),
            state.i2c_partner[channel],
            device_address,
            register_address,
            byte_count
        );

        return std::string(prefix) + hex + "\n";
    }

    if (command.rfind(
            "I2C_TRANSACTION_HEX",
            0) == 0) {

        int channel = -1;
        unsigned int device_address = 0;
        unsigned int receive_count = 0;

        char hex_text[
            (MAX_I2C_TRANSFER_BYTES * 2) + 1
        ] = {0};

        if (std::sscanf(
                command.c_str(),
                "I2C_TRANSACTION_HEX %d %u %2048s %u",
                &channel,
                &device_address,
                hex_text,
                &receive_count) != 4) {
            return
                "ERR USAGE=I2C_TRANSACTION_HEX_"
                "<channel>_<device_address>_"
                "<tx_hex>_<rx_count>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (device_address > 127) {
            return "ERR DEVICE_ADDRESS_RANGE=0..127\n";
        }

        if (receive_count == 0 ||
            receive_count > MAX_I2C_TRANSFER_BYTES) {
            return "ERR RX_COUNT_RANGE=1..1024\n";
        }

        if (!state.i2c_active[channel]) {
            return "ERR I2C_NOT_OPEN\n";
        }

        std::vector<uint8_t> transmit;

        if (!decode_hex(hex_text, transmit) ||
            transmit.size() > MAX_I2C_TRANSFER_BYTES) {
            return
                "ERR INVALID_HEX_OR_SIZE_"
                "MAX_BYTES=1024\n";
        }

        std::vector<uint8_t> receive(
            receive_count,
            0
        );

        VMXErrorCode error = 0;

        if (!vmx.io.I2C_Transaction(
                state.i2c_handles[channel],
                static_cast<uint8_t>(device_address),
                transmit.data(),
                static_cast<uint16_t>(transmit.size()),
                receive.data(),
                static_cast<uint16_t>(receive.size()),
                &error)) {
            return make_vmx_error(error);
        }

        const std::string receive_hex =
            encode_hex(
                receive.data(),
                receive.size()
            );

        char prefix[192];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "DEVICE=%u TX_SIZE=%u RX_SIZE=%u HEX=",
            channel,
            i2c_role_name(state.i2c_is_sda[channel]),
            state.i2c_partner[channel],
            device_address,
            static_cast<unsigned int>(transmit.size()),
            receive_count
        );

        return std::string(prefix) +
            receive_hex +
            "\n";
    }

    if (command.rfind("I2C_RELEASE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "I2C_RELEASE %d",
                &channel) != 1) {
            return "ERR USAGE=I2C_RELEASE_<channel>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.i2c_active[channel]) {
            return "ERR I2C_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;
        unsigned int released = 0;

        if (!release_channel(
                vmx,
                state,
                static_cast<std::size_t>(channel),
                error,
                released)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d I2C_RELEASED=%u\n",
            channel,
            released
        );

        return std::string(response);
    }

    if (command.rfind("SPI_OPEN", 0) == 0) {
        int clk_channel = -1;
        int miso_channel = -1;
        int mosi_channel = -1;
        int cs_channel = -1;
        unsigned long bitrate = 0;
        unsigned int mode = 0;

        if (std::sscanf(
                command.c_str(),
                "SPI_OPEN %d %d %d %d %lu %u",
                &clk_channel,
                &miso_channel,
                &mosi_channel,
                &cs_channel,
                &bitrate,
                &mode) != 6) {
            return
                "ERR USAGE=SPI_OPEN_"
                "<clk>_<miso>_<mosi>_<cs>_"
                "<bitrate>_<mode>\n";
        }

        const int channels[4] = {
            clk_channel,
            miso_channel,
            mosi_channel,
            cs_channel
        };

        for (std::size_t index = 0;
             index < 4;
             ++index) {

            if (channels[index] < 0 ||
                channels[index] >=
                    static_cast<int>(
                        MAX_VMX_CHANNELS
                    )) {
                return "ERR INVALID_CHANNEL\n";
            }
        }

        for (std::size_t first = 0;
             first < 4;
             ++first) {

            for (std::size_t second = first + 1;
                 second < 4;
                 ++second) {

                if (channels[first] ==
                    channels[second]) {
                    return
                        "ERR SPI_CHANNELS_MUST_DIFFER\n";
                }
            }
        }

        if (bitrate == 0) {
            return "ERR INVALID_BITRATE\n";
        }

        if (mode > 3) {
            return "ERR SPI_MODE_RANGE=0..3\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    clk_channel
                ),
                VMXChannelCapability::SPI_CLK)) {
            return "ERR CHANNEL_NOT_SPI_CLK_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    miso_channel
                ),
                VMXChannelCapability::SPI_MISO)) {
            return "ERR CHANNEL_NOT_SPI_MISO_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    mosi_channel
                ),
                VMXChannelCapability::SPI_MOSI)) {
            return "ERR CHANNEL_NOT_SPI_MOSI_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(
                    cs_channel
                ),
                VMXChannelCapability::SPI_CS)) {
            return "ERR CHANNEL_NOT_SPI_CS_CAPABLE\n";
        }

        bool any_spi_active = false;

        for (std::size_t index = 0;
             index < 4;
             ++index) {
            if (state.spi_active[channels[index]]) {
                any_spi_active = true;
            }
        }

        if (any_spi_active) {
            const VMXResourceHandle handle =
                state.spi_handles[clk_channel];

            bool same_resource =
                state.spi_active[clk_channel] &&
                state.spi_active[miso_channel] &&
                state.spi_active[mosi_channel] &&
                state.spi_active[cs_channel] &&
                state.spi_handles[miso_channel] ==
                    handle &&
                state.spi_handles[mosi_channel] ==
                    handle &&
                state.spi_handles[cs_channel] ==
                    handle &&
                state.spi_role[clk_channel] == 0 &&
                state.spi_role[miso_channel] == 1 &&
                state.spi_role[mosi_channel] == 2 &&
                state.spi_role[cs_channel] == 3 &&
                state.spi_bitrate[clk_channel] ==
                    bitrate &&
                state.spi_mode[clk_channel] == mode;

            if (same_resource) {
                char response[224];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CLK=%d MISO=%d MOSI=%d CS=%d "
                    "MODE=SPI BITRATE=%lu SPI_MODE=%u "
                    "CS_ACTIVE_LOW=1 MSB_FIRST=1 "
                    "ALREADY_OPEN=1\n",
                    clk_channel,
                    miso_channel,
                    mosi_channel,
                    cs_channel,
                    bitrate,
                    mode
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_SPI\n";
        }

        for (std::size_t index = 0;
             index < 4;
             ++index) {

            const int channel = channels[index];

            const bool busy =
                state.dio_output_active[channel] ||
                state.dio_input_active[channel] ||
                state.pwm_active[channel] ||
                state.analog_active[channel] ||
                state.analog_trigger_active[channel] ||
                state.encoder_active[channel] ||
                state.input_capture_active[channel] ||
                state.pwm_capture_active[channel] ||
                state.uart_active[channel] ||
                state.i2c_active[channel] ||
                state.led_array_active[channel];

            if (busy) {
                char response[96];

                std::snprintf(
                    response,
                    sizeof(response),
                    "ERR SPI_CHANNEL_BUSY=%d\n",
                    channel
                );

                return std::string(response);
            }
        }

        SPIConfig config(
            static_cast<uint32_t>(bitrate),
            static_cast<uint8_t>(mode),
            true,
            true
        );

        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateQuadchannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        clk_channel
                    ),
                    VMXChannelCapability::SPI_CLK),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        miso_channel
                    ),
                    VMXChannelCapability::SPI_MISO),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        mosi_channel
                    ),
                    VMXChannelCapability::SPI_MOSI),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(
                        cs_channel
                    ),
                    VMXChannelCapability::SPI_CS),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        for (std::size_t index = 0;
             index < 4;
             ++index) {

            const int channel = channels[index];

            state.spi_active[channel] = true;
            state.spi_handles[channel] = handle;
            state.spi_role[channel] =
                static_cast<uint8_t>(index);
            state.spi_bitrate[channel] =
                static_cast<uint32_t>(bitrate);
            state.spi_mode[channel] =
                static_cast<uint8_t>(mode);

            for (std::size_t member = 0;
                 member < 4;
                 ++member) {
                state.spi_members[channel][member] =
                    channels[member];
            }
        }

        char response[224];

        std::snprintf(
            response,
            sizeof(response),
            "OK CLK=%d MISO=%d MOSI=%d CS=%d "
            "MODE=SPI BITRATE=%lu SPI_MODE=%u "
            "CS_ACTIVE_LOW=1 MSB_FIRST=1\n",
            clk_channel,
            miso_channel,
            mosi_channel,
            cs_channel,
            bitrate,
            mode
        );

        return std::string(response);
    }

    if (command.rfind("SPI_WRITE_HEX", 0) == 0) {
        int channel = -1;

        char hex_text[
            (MAX_SPI_TRANSFER_BYTES * 2) + 1
        ] = {0};

        if (std::sscanf(
                command.c_str(),
                "SPI_WRITE_HEX %d %2048s",
                &channel,
                hex_text) != 2) {
            return
                "ERR USAGE=SPI_WRITE_HEX_"
                "<channel>_<hex_data>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(
                    MAX_VMX_CHANNELS
                )) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.spi_active[channel]) {
            return "ERR SPI_NOT_OPEN\n";
        }

        std::vector<uint8_t> data;

        if (!decode_hex(hex_text, data)) {
            return
                "ERR INVALID_HEX_OR_SIZE_"
                "MAX_BYTES=1024\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.SPI_Write(
                state.spi_handles[channel],
                data.data(),
                static_cast<uint16_t>(data.size()),
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%s WRITTEN=%u\n",
            channel,
            spi_role_name(state.spi_role[channel]),
            static_cast<unsigned int>(data.size())
        );

        return std::string(response);
    }

    if (command.rfind("SPI_READ_HEX", 0) == 0) {
        int channel = -1;
        unsigned int byte_count = 0;

        if (std::sscanf(
                command.c_str(),
                "SPI_READ_HEX %d %u",
                &channel,
                &byte_count) != 2) {
            return
                "ERR USAGE=SPI_READ_HEX_"
                "<channel>_<byte_count>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(
                    MAX_VMX_CHANNELS
                )) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (byte_count == 0 ||
            byte_count > MAX_SPI_TRANSFER_BYTES) {
            return "ERR BYTE_COUNT_RANGE=1..1024\n";
        }

        if (!state.spi_active[channel]) {
            return "ERR SPI_NOT_OPEN\n";
        }

        std::vector<uint8_t> data(byte_count, 0);
        VMXErrorCode error = 0;

        if (!vmx.io.SPI_Read(
                state.spi_handles[channel],
                data.data(),
                static_cast<uint16_t>(data.size()),
                &error)) {
            return make_vmx_error(error);
        }

        const std::string hex =
            encode_hex(data.data(), data.size());

        char prefix[112];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CHANNEL=%d ROLE=%s SIZE=%u HEX=",
            channel,
            spi_role_name(state.spi_role[channel]),
            byte_count
        );

        return std::string(prefix) + hex + "\n";
    }

    if (command.rfind(
            "SPI_TRANSACTION_HEX",
            0) == 0) {

        int channel = -1;

        char hex_text[
            (MAX_SPI_TRANSFER_BYTES * 2) + 1
        ] = {0};

        if (std::sscanf(
                command.c_str(),
                "SPI_TRANSACTION_HEX %d %2048s",
                &channel,
                hex_text) != 2) {
            return
                "ERR USAGE=SPI_TRANSACTION_HEX_"
                "<channel>_<hex_data>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(
                    MAX_VMX_CHANNELS
                )) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.spi_active[channel]) {
            return "ERR SPI_NOT_OPEN\n";
        }

        std::vector<uint8_t> transmit;

        if (!decode_hex(hex_text, transmit)) {
            return
                "ERR INVALID_HEX_OR_SIZE_"
                "MAX_BYTES=1024\n";
        }

        std::vector<uint8_t> receive(
            transmit.size(),
            0
        );

        VMXErrorCode error = 0;

        if (!vmx.io.SPI_Transaction(
                state.spi_handles[channel],
                transmit.data(),
                receive.data(),
                static_cast<uint16_t>(
                    transmit.size()
                ),
                &error)) {
            return make_vmx_error(error);
        }

        const std::string receive_hex =
            encode_hex(
                receive.data(),
                receive.size()
            );

        char prefix[128];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CHANNEL=%d ROLE=%s SIZE=%u HEX=",
            channel,
            spi_role_name(state.spi_role[channel]),
            static_cast<unsigned int>(
                receive.size()
            )
        );

        return std::string(prefix) +
            receive_hex +
            "\n";
    }

    if (command.rfind("SPI_RELEASE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "SPI_RELEASE %d",
                &channel) != 1) {
            return "ERR USAGE=SPI_RELEASE_<channel>\n";
        }

        if (channel < 0 ||
            channel >=
                static_cast<int>(
                    MAX_VMX_CHANNELS
                )) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.spi_active[channel]) {
            return "ERR SPI_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;
        unsigned int released = 0;

        if (!release_channel(
                vmx,
                state,
                static_cast<std::size_t>(channel),
                error,
                released)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d SPI_RELEASED=%u\n",
            channel,
            released
        );

        return std::string(response);
    }

    if (command.rfind("UART_OPEN", 0) == 0) {
        int tx_channel = -1;
        int rx_channel = -1;
        unsigned long baudrate = 0;

        if (std::sscanf(
                command.c_str(),
                "UART_OPEN %d %d %lu",
                &tx_channel,
                &rx_channel,
                &baudrate) != 3) {
            return
                "ERR USAGE=UART_OPEN_"
                "<tx_channel>_<rx_channel>_<baudrate>\n";
        }

        if (tx_channel < 0 ||
            tx_channel >= static_cast<int>(MAX_VMX_CHANNELS) ||
            rx_channel < 0 ||
            rx_channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (tx_channel == rx_channel) {
            return "ERR UART_CHANNELS_MUST_DIFFER\n";
        }

        if (baudrate == 0) {
            return "ERR INVALID_BAUDRATE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(tx_channel),
                VMXChannelCapability::UART_TX)) {
            return "ERR CHANNEL_NOT_UART_TX_CAPABLE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(rx_channel),
                VMXChannelCapability::UART_RX)) {
            return "ERR CHANNEL_NOT_UART_RX_CAPABLE\n";
        }

        if (state.uart_active[tx_channel] ||
            state.uart_active[rx_channel]) {

            if (state.uart_active[tx_channel] &&
                state.uart_active[rx_channel] &&
                state.uart_handles[tx_channel] ==
                    state.uart_handles[rx_channel] &&
                state.uart_partner[tx_channel] == rx_channel &&
                state.uart_partner[rx_channel] == tx_channel &&
                state.uart_is_tx[tx_channel] &&
                !state.uart_is_tx[rx_channel] &&
                state.uart_baudrate[tx_channel] == baudrate &&
                state.uart_baudrate[rx_channel] == baudrate) {

                char response[160];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL_TX=%d CHANNEL_RX=%d "
                    "MODE=UART BAUD=%lu ALREADY_OPEN=1\n",
                    tx_channel,
                    rx_channel,
                    baudrate
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_UART\n";
        }

        const bool tx_busy =
            state.dio_output_active[tx_channel] ||
            state.dio_input_active[tx_channel] ||
            state.pwm_active[tx_channel] ||
            state.analog_active[tx_channel] ||
            state.analog_trigger_active[tx_channel] ||
            state.encoder_active[tx_channel] ||
            state.input_capture_active[tx_channel] ||
            state.pwm_capture_active[tx_channel] ||
            state.spi_active[tx_channel] ||
            state.i2c_active[tx_channel] ||
            state.led_array_active[tx_channel];

        const bool rx_busy =
            state.dio_output_active[rx_channel] ||
            state.dio_input_active[rx_channel] ||
            state.pwm_active[rx_channel] ||
            state.analog_active[rx_channel] ||
            state.analog_trigger_active[rx_channel] ||
            state.encoder_active[rx_channel] ||
            state.input_capture_active[rx_channel] ||
            state.pwm_capture_active[rx_channel] ||
            state.spi_active[rx_channel] ||
            state.i2c_active[rx_channel] ||
            state.led_array_active[rx_channel];

        if (tx_busy) {
            return "ERR UART_TX_CHANNEL_BUSY\n";
        }

        if (rx_busy) {
            return "ERR UART_RX_CHANNEL_BUSY\n";
        }

        UARTConfig config(
            static_cast<uint32_t>(baudrate)
        );

        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateDualchannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(tx_channel),
                    VMXChannelCapability::UART_TX),
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(rx_channel),
                    VMXChannelCapability::UART_RX),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        state.uart_active[tx_channel] = true;
        state.uart_handles[tx_channel] = handle;
        state.uart_partner[tx_channel] = rx_channel;
        state.uart_is_tx[tx_channel] = true;
        state.uart_baudrate[tx_channel] =
            static_cast<uint32_t>(baudrate);

        state.uart_active[rx_channel] = true;
        state.uart_handles[rx_channel] = handle;
        state.uart_partner[rx_channel] = tx_channel;
        state.uart_is_tx[rx_channel] = false;
        state.uart_baudrate[rx_channel] =
            static_cast<uint32_t>(baudrate);

        char response[160];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL_TX=%d CHANNEL_RX=%d "
            "MODE=UART BAUD=%lu\n",
            tx_channel,
            rx_channel,
            baudrate
        );

        return std::string(response);
    }

    if (command.rfind("UART_AVAILABLE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "UART_AVAILABLE %d",
                &channel) != 1) {
            return "ERR USAGE=UART_AVAILABLE_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.uart_active[channel]) {
            return "ERR UART_NOT_OPEN\n";
        }

        uint16_t available = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.UART_GetBytesAvailable(
                state.uart_handles[channel],
                available,
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "AVAILABLE=%u\n",
            channel,
            state.uart_is_tx[channel] ? "TX" : "RX",
            state.uart_partner[channel],
            static_cast<unsigned int>(available)
        );

        return std::string(response);
    }

    if (command.rfind("UART_WRITE_HEX", 0) == 0) {
        int channel = -1;
        char hex_text[
            (MAX_UART_TRANSFER_BYTES * 2) + 1
        ] = {0};

        if (std::sscanf(
                command.c_str(),
                "UART_WRITE_HEX %d %2048s",
                &channel,
                hex_text) != 2) {
            return
                "ERR USAGE=UART_WRITE_HEX_"
                "<channel>_<hex_data>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.uart_active[channel]) {
            return "ERR UART_NOT_OPEN\n";
        }

        std::vector<uint8_t> data;

        if (!decode_hex(hex_text, data)) {
            return
                "ERR INVALID_HEX_OR_SIZE_"
                "MAX_BYTES=1024\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.UART_Write(
                state.uart_handles[channel],
                data.data(),
                static_cast<uint16_t>(data.size()),
                &error)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d WRITTEN=%u\n",
            channel,
            static_cast<unsigned int>(data.size())
        );

        return std::string(response);
    }

    if (command.rfind("UART_READ_HEX", 0) == 0) {
        int channel = -1;
        unsigned int max_bytes = 0;

        if (std::sscanf(
                command.c_str(),
                "UART_READ_HEX %d %u",
                &channel,
                &max_bytes) != 2) {
            return
                "ERR USAGE=UART_READ_HEX_"
                "<channel>_<max_bytes>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (max_bytes == 0 ||
            max_bytes > MAX_UART_TRANSFER_BYTES) {
            return "ERR MAX_BYTES_RANGE=1..1024\n";
        }

        if (!state.uart_active[channel]) {
            return "ERR UART_NOT_OPEN\n";
        }

        std::vector<uint8_t> data(max_bytes, 0);
        uint16_t actual_size = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.UART_Read(
                state.uart_handles[channel],
                data.data(),
                static_cast<uint16_t>(max_bytes),
                actual_size,
                &error)) {
            return make_vmx_error(error);
        }

        if (actual_size > max_bytes) {
            return "ERR UART_READ_SIZE_INVALID\n";
        }

        const std::string hex =
            encode_hex(data.data(), actual_size);

        char prefix[128];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CHANNEL=%d ROLE=%s PARTNER=%d "
            "SIZE=%u HEX=",
            channel,
            state.uart_is_tx[channel] ? "TX" : "RX",
            state.uart_partner[channel],
            static_cast<unsigned int>(actual_size)
        );

        return std::string(prefix) + hex + "\n";
    }

    if (command.rfind("UART_RELEASE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "UART_RELEASE %d",
                &channel) != 1) {
            return "ERR USAGE=UART_RELEASE_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.uart_active[channel]) {
            return "ERR UART_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;
        unsigned int released = 0;

        if (!release_channel(
                vmx,
                state,
                static_cast<std::size_t>(channel),
                error,
                released)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d UART_RELEASED=%u\n",
            channel,
            released
        );

        return std::string(response);
    }

    if (command.rfind("ACCUMULATOR_OPEN", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ACCUMULATOR_OPEN %d",
                &channel) != 1) {
            return "ERR USAGE=ACCUMULATOR_OPEN_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (state.analog_active[channel]) {
            if (state.analog_counter_enabled[channel]) {
                char response[128];

                std::snprintf(
                    response,
                    sizeof(response),
                    "OK CHANNEL=%d MODE=ACCUMULATOR "
                    "ALREADY_OPEN=1 COUNTER=1\n",
                    channel
                );

                return std::string(response);
            }

            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG_WITHOUT_COUNTER\n";
        }

        if (state.analog_trigger_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG_TRIGGER\n";
        }

        if (state.dio_output_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_OUTPUT\n";
        }

        if (state.dio_input_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT\n";
        }

        if (state.pwm_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::AccumulatorInput)) {
            return "ERR CHANNEL_NOT_ACCUMULATOR_CAPABLE\n";
        }

        AccumulatorConfig config;
        config.SetEnableAccumulationCounter(true);
        config.SetAccumulationCounterCenter(0);
        config.SetAccumulationCounterDeadband(0);

        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::AccumulatorInput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_Counter_Reset(
                handle,
                &error)) {
            VMXErrorCode cleanup_error = 0;

            vmx.io.DeallocateResource(
                handle,
                &cleanup_error
            );

            return make_vmx_error(error);
        }

        state.analog_active[channel] = true;
        state.analog_handles[channel] = handle;
        state.analog_counter_enabled[channel] = true;

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=ACCUMULATOR "
            "OVERSAMPLE_BITS=%u AVERAGE_BITS=%u "
            "COUNTER=1 CENTER=%d DEADBAND=%d\n",
            channel,
            static_cast<unsigned int>(
                config.GetNumOversampleBits()
            ),
            static_cast<unsigned int>(
                config.GetNumAverageBits()
            ),
            static_cast<int>(
                config.GetAccumulationCounterCenter()
            ),
            static_cast<int>(
                config.GetAccumulationCounterDeadband()
            )
        );

        return std::string(response);
    }

    if (command.rfind("ACCUMULATOR_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ACCUMULATOR_READ %d",
                &channel) != 1) {
            return "ERR USAGE=ACCUMULATOR_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.analog_active[channel]) {
            return "ERR ACCUMULATOR_NOT_OPEN\n";
        }

        uint32_t oversample_value = 0;
        uint32_t average_value = 0;
        uint32_t instantaneous_value = 0;
        float average_voltage = 0.0f;
        float full_scale_voltage = 0.0f;
        VMXErrorCode error = 0;

        if (!vmx.io.Accumulator_GetOversampleValue(
                state.analog_handles[channel],
                oversample_value,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_GetAverageValue(
                state.analog_handles[channel],
                average_value,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_GetInstantaneousValue(
                state.analog_handles[channel],
                instantaneous_value,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_GetAverageVoltage(
                state.analog_handles[channel],
                average_voltage,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_GetFullScaleVoltage(
                full_scale_voltage,
                &error)) {
            return make_vmx_error(error);
        }

        char response[224];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d OVERSAMPLE=%lu AVERAGE=%lu "
            "INSTANT=%lu VOLTS=%.3f FULL_SCALE=%.3f "
            "COUNTER=%d\n",
            channel,
            static_cast<unsigned long>(oversample_value),
            static_cast<unsigned long>(average_value),
            static_cast<unsigned long>(instantaneous_value),
            static_cast<double>(average_voltage),
            static_cast<double>(full_scale_voltage),
            state.analog_counter_enabled[channel] ? 1 : 0
        );

        return std::string(response);
    }

    if (command.rfind(
            "ACCUMULATOR_COUNTER_READ",
            0) == 0) {

        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ACCUMULATOR_COUNTER_READ %d",
                &channel) != 1) {
            return "ERR USAGE=ACCUMULATOR_COUNTER_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.analog_active[channel]) {
            return "ERR ACCUMULATOR_NOT_OPEN\n";
        }

        if (!state.analog_counter_enabled[channel]) {
            return "ERR ACCUMULATOR_COUNTER_NOT_ENABLED\n";
        }

        int64_t value = 0;
        uint32_t count = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.Accumulator_Counter_GetValueAndCount(
                state.analog_handles[channel],
                value,
                count,
                &error)) {
            return make_vmx_error(error);
        }

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d VALUE=%lld COUNT=%lu\n",
            channel,
            static_cast<long long>(value),
            static_cast<unsigned long>(count)
        );

        return std::string(response);
    }

    if (command.rfind(
            "ACCUMULATOR_COUNTER_RESET",
            0) == 0) {

        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ACCUMULATOR_COUNTER_RESET %d",
                &channel) != 1) {
            return "ERR USAGE=ACCUMULATOR_COUNTER_RESET_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.analog_active[channel]) {
            return "ERR ACCUMULATOR_NOT_OPEN\n";
        }

        if (!state.analog_counter_enabled[channel]) {
            return "ERR ACCUMULATOR_COUNTER_NOT_ENABLED\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.Accumulator_Counter_Reset(
                state.analog_handles[channel],
                &error)) {
            return make_vmx_error(error);
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d COUNTER_RESET=1\n",
            channel
        );

        return std::string(response);
    }

    if (command.rfind(
            "ANALOG_TRIGGER_OPEN",
            0) == 0) {

        int channel = -1;
        int threshold_high = -1;
        int threshold_low = -1;
        char mode_text[32] = {0};

        if (std::sscanf(
                command.c_str(),
                "ANALOG_TRIGGER_OPEN %d %d %d %31s",
                &channel,
                &threshold_high,
                &threshold_low,
                mode_text) != 4) {
            return
                "ERR USAGE=ANALOG_TRIGGER_OPEN_"
                "<channel>_<high_raw>_<low_raw>_"
                "<STATE|RISING|FALLING>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (threshold_high < 0 ||
            threshold_high > 4095 ||
            threshold_low < 0 ||
            threshold_low > 4095) {
            return "ERR THRESHOLD_RANGE=0..4095\n";
        }

        AnalogTriggerConfig::AnalogTriggerMode mode =
            AnalogTriggerConfig::STATE;

        const std::string mode_string(mode_text);

        if (mode_string == "STATE") {
            mode = AnalogTriggerConfig::STATE;
        } else if (mode_string == "RISING") {
            mode = AnalogTriggerConfig::RISING_EDGE_PULSE;
        } else if (mode_string == "FALLING") {
            mode = AnalogTriggerConfig::FALLING_EDGE_PULSE;
        } else {
            return "ERR INVALID_TRIGGER_MODE\n";
        }

        if (state.analog_trigger_active[channel]) {
            char response[112];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=ANALOG_TRIGGER "
                "ALREADY_OPEN=1\n",
                channel
            );

            return std::string(response);
        }

        if (state.analog_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG\n";
        }

        if (state.dio_output_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_OUTPUT\n";
        }

        if (state.dio_input_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT\n";
        }

        if (state.pwm_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::AnalogTriggerInput)) {
            return "ERR CHANNEL_NOT_ANALOG_TRIGGER_CAPABLE\n";
        }

        AnalogTriggerConfig config(
            static_cast<uint16_t>(threshold_high),
            static_cast<uint16_t>(threshold_low),
            mode
        );

        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::AnalogTriggerInput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        state.analog_trigger_active[channel] = true;
        state.analog_trigger_handles[channel] = handle;

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=ANALOG_TRIGGER "
            "THRESHOLD_HIGH=%u THRESHOLD_LOW=%u "
            "TRIGGER_MODE=%s\n",
            channel,
            static_cast<unsigned int>(
                config.GetThresholdHigh()
            ),
            static_cast<unsigned int>(
                config.GetThresholdLow()
            ),
            mode_text
        );

        return std::string(response);
    }

    if (command.rfind(
            "ANALOG_TRIGGER_READ",
            0) == 0) {

        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ANALOG_TRIGGER_READ %d",
                &channel) != 1) {
            return "ERR USAGE=ANALOG_TRIGGER_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.analog_trigger_active[channel]) {
            return "ERR ANALOG_TRIGGER_NOT_OPEN\n";
        }

        VMXIO::AnalogTriggerState trigger_state =
            VMXIO::BelowThreshold;

        VMXErrorCode error = 0;

        if (!vmx.io.AnalogTrigger_GetState(
                state.analog_trigger_handles[channel],
                trigger_state,
                &error)) {
            return make_vmx_error(error);
        }

        const char* state_text = "BELOW_THRESHOLD";

        if (trigger_state == VMXIO::AboveThreshold) {
            state_text = "ABOVE_THRESHOLD";
        } else if (trigger_state == VMXIO::InWindow) {
            state_text = "IN_WINDOW";
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d STATE=%s\n",
            channel,
            state_text
        );

        return std::string(response);
    }

    if (command.rfind("ANALOG_OPEN", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ANALOG_OPEN %d",
                &channel) != 1) {
            return "ERR USAGE=ANALOG_OPEN_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (state.analog_trigger_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG_TRIGGER\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.analog_active[channel]) {
            char response[112];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=ANALOG ALREADY_OPEN=1\n",
                channel
            );

            return std::string(response);
        }

        if (state.dio_output_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_OUTPUT\n";
        }

        if (state.dio_input_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT\n";
        }

        if (state.pwm_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::AccumulatorInput)) {
            return "ERR CHANNEL_NOT_ANALOG_CAPABLE\n";
        }

        AccumulatorConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::AccumulatorInput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        state.analog_active[channel] = true;
        state.analog_handles[channel] = handle;
        state.analog_counter_enabled[channel] = false;

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=ANALOG AVERAGE_BITS=%u\n",
            channel,
            static_cast<unsigned int>(
                config.GetNumAverageBits()
            )
        );

        return std::string(response);
    }

    if (command.rfind("ANALOG_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "ANALOG_READ %d",
                &channel) != 1) {
            return "ERR USAGE=ANALOG_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.analog_active[channel]) {
            return "ERR ANALOG_NOT_OPEN\n";
        }

        uint32_t raw_value = 0;
        float voltage = 0.0f;
        VMXErrorCode error = 0;

        if (!vmx.io.Accumulator_GetAverageValue(
                state.analog_handles[channel],
                raw_value,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.Accumulator_GetAverageVoltage(
                state.analog_handles[channel],
                voltage,
                &error)) {
            return make_vmx_error(error);
        }

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d RAW=%lu VOLTS=%.3f\n",
            channel,
            static_cast<unsigned long>(raw_value),
            static_cast<double>(voltage)
        );

        return std::string(response);
    }

    if (command.rfind("PWM_OPEN", 0) == 0) {
        int channel = -1;
        unsigned int frequency_hz = 0;

        if (std::sscanf(
                command.c_str(),
                "PWM_OPEN %d %u",
                &channel,
                &frequency_hz) != 2) {
            return "ERR USAGE=PWM_OPEN_<channel>_<frequency_hz>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (frequency_hz == 0) {
            return "ERR INVALID_FREQUENCY\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.dio_output_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_OUTPUT\n";
        }

        if (state.dio_input_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT\n";
        }

        if (state.analog_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG\n";
        }

        if (state.pwm_active[channel]) {
            if (state.pwm_frequency_hz[channel] != frequency_hz) {
                return "ERR PWM_ALREADY_OPEN_DIFFERENT_FREQUENCY\n";
            }

            char response[128];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=PWM FREQUENCY=%u ALREADY_OPEN=1\n",
                channel,
                frequency_hz
            );

            return std::string(response);
        }

        VMXChannelType channel_type;
        VMXChannelCapability capabilities;

        if (!vmx.io.GetChannelCapabilities(
                static_cast<VMXChannelIndex>(channel),
                channel_type,
                capabilities)) {
            return "ERR CHANNEL_INFO_FAILED\n";
        }

        // FlexDIO PWM pairs share one resource and are not supported.
        if (channel_type == VMXChannelType::FlexDIO) {
            return "ERR FLEX_PWM_PAIR_NOT_SUPPORTED_YET\n";
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::PWMGeneratorOutput)) {
            return "ERR CHANNEL_NOT_PWM_CAPABLE\n";
        }

        PWMGeneratorConfig config(
            static_cast<uint32_t>(frequency_hz)
        );

        config.SetMaxDutyCycleValue(255);

        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::PWMGeneratorOutput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        if (!vmx.io.PWMGenerator_SetDutyCycle(
                handle,
                static_cast<VMXResourcePortIndex>(0),
                0,
                &error)) {
            VMXErrorCode cleanup_error = 0;
            vmx.io.DeallocateResource(handle, &cleanup_error);
            return make_vmx_error(error);
        }

        state.pwm_active[channel] = true;
        state.pwm_handles[channel] = handle;
        state.pwm_frequency_hz[channel] = frequency_hz;

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=PWM FREQUENCY=%u DUTY=0\n",
            channel,
            frequency_hz
        );

        return std::string(response);
    }

    if (command.rfind("PWM_WRITE", 0) == 0) {
        int channel = -1;
        int duty = -1;

        if (std::sscanf(
                command.c_str(),
                "PWM_WRITE %d %d",
                &channel,
                &duty) != 2) {
            return "ERR USAGE=PWM_WRITE_<channel>_<0-255>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (duty < 0 || duty > 255) {
            return "ERR INVALID_DUTY\n";
        }

        if (!state.pwm_active[channel]) {
            return "ERR PWM_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.PWMGenerator_SetDutyCycle(
                state.pwm_handles[channel],
                static_cast<VMXResourcePortIndex>(0),
                static_cast<uint16_t>(duty),
                &error)) {
            return make_vmx_error(error);
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d DUTY=%d\n",
            channel,
            duty
        );

        return std::string(response);
    }

    if (command.rfind("PWM_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "PWM_READ %d",
                &channel) != 1) {
            return "ERR USAGE=PWM_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (!state.pwm_active[channel]) {
            return "ERR PWM_NOT_OPEN\n";
        }

        uint16_t duty = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.PWMGenerator_GetDutyCycle(
                state.pwm_handles[channel],
                static_cast<VMXResourcePortIndex>(0),
                &duty,
                &error)) {
            return make_vmx_error(error);
        }

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d FREQUENCY=%lu DUTY=%u\n",
            channel,
            static_cast<unsigned long>(
                state.pwm_frequency_hz[channel]
            ),
            static_cast<unsigned int>(duty)
        );

        return std::string(response);
    }

    if (command.rfind("DIO_OPEN_OUT", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "DIO_OPEN_OUT %d",
                &channel) != 1) {
            return "ERR USAGE=DIO_OPEN_OUT_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.analog_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG\n";
        }

        if (state.pwm_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM\n";
        }

        if (state.dio_input_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT\n";
        }

        if (state.dio_output_active[channel]) {
            char response[96];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=OUTPUT ALREADY_OPEN=1\n",
                channel
            );

            return std::string(response);
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::DigitalOutput)) {
            return "ERR CHANNEL_NOT_OUTPUT_CAPABLE\n";
        }

        DIOConfig config(DIOConfig::OutputMode::PUSHPULL);
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::DigitalOutput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        if (!vmx.io.DIO_Set(handle, false, &error)) {
            VMXErrorCode cleanup_error = 0;
            vmx.io.DeallocateResource(handle, &cleanup_error);
            return make_vmx_error(error);
        }

        state.dio_output_handles[channel] = handle;
        state.dio_output_active[channel] = true;

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=OUTPUT VALUE=0\n",
            channel
        );

        return std::string(response);
    }

    if (command.rfind("DIO_OPEN_IN", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "DIO_OPEN_IN %d",
                &channel) != 1) {
            return "ERR USAGE=DIO_OPEN_IN_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (state.input_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_INPUT_CAPTURE\n";
        }

        if (state.pwm_capture_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM_CAPTURE\n";
        }

        if (state.encoder_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ENCODER\n";
        }

        if (state.analog_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_ANALOG\n";
        }

        if (state.pwm_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_PWM\n";
        }

        if (state.dio_output_active[channel]) {
            return "ERR CHANNEL_ALREADY_OPEN_AS_OUTPUT\n";
        }

        if (state.dio_input_active[channel]) {
            char response[96];

            std::snprintf(
                response,
                sizeof(response),
                "OK CHANNEL=%d MODE=INPUT ALREADY_OPEN=1\n",
                channel
            );

            return std::string(response);
        }

        if (!vmx.io.ChannelSupportsCapability(
                static_cast<VMXChannelIndex>(channel),
                VMXChannelCapability::DigitalInput)) {
            return "ERR CHANNEL_NOT_INPUT_CAPABLE\n";
        }

        DIOConfig config;
        VMXResourceHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.ActivateSinglechannelResource(
                VMXChannelInfo(
                    static_cast<VMXChannelIndex>(channel),
                    VMXChannelCapability::DigitalInput),
                &config,
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        state.dio_input_handles[channel] = handle;
        state.dio_input_active[channel] = true;

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d MODE=INPUT PULL=PULLUP\n",
            channel
        );

        return std::string(response);
    }

    if (command.rfind("DIO_WRITE", 0) == 0) {
        int channel = -1;
        int value = -1;

        if (std::sscanf(
                command.c_str(),
                "DIO_WRITE %d %d",
                &channel,
                &value) != 2) {
            return "ERR USAGE=DIO_WRITE_<channel>_<0|1>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        if (value != 0 && value != 1) {
            return "ERR INVALID_VALUE\n";
        }

        if (!state.dio_output_active[channel]) {
            return "ERR CHANNEL_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.io.DIO_Set(
                state.dio_output_handles[channel],
                value != 0,
                &error)) {
            return make_vmx_error(error);
        }

        char response[64];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d VALUE=%d\n",
            channel,
            value
        );

        return std::string(response);
    }

    if (command.rfind("DIO_READ", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "DIO_READ %d",
                &channel) != 1) {
            return "ERR USAGE=DIO_READ_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        VMXResourceHandle handle = 0;

        if (state.dio_output_active[channel]) {
            handle = state.dio_output_handles[channel];
        } else if (state.dio_input_active[channel]) {
            handle = state.dio_input_handles[channel];
        } else {
            return "ERR CHANNEL_NOT_OPEN\n";
        }

        bool value = false;
        VMXErrorCode error = 0;

        if (!vmx.io.DIO_Get(
                handle,
                value,
                &error)) {
            return make_vmx_error(error);
        }

        char response[64];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d VALUE=%d\n",
            channel,
            value ? 1 : 0
        );

        return std::string(response);
    }

    // VMX_CAN_BRIDGE_V1
    if (command == "CAN_INIT") {
        VMXErrorCode error = 0;
        unsigned int closed_count = 0;

        if (!close_all_can_streams(
                vmx,
                state,
                error,
                closed_count)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.can.FlushRxFIFO(&error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.can.FlushTxFIFO(&error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.can.SetMode(
                VMXCAN::VMXCAN_NORMAL,
                &error)) {
            return make_vmx_error(error);
        }

        vmx.time.DelayMilliseconds(20);

        char response[112];

        std::snprintf(
            response,
            sizeof(response),
            "OK CAN_INIT MODE=NORMAL CLOSED=%u\n",
            closed_count
        );

        return std::string(response);
    }

    if (command.rfind("CAN_MODE", 0) == 0) {
        char mode_text[16] = {0};

        if (std::sscanf(
                command.c_str(),
                "CAN_MODE %15s",
                mode_text) != 1) {
            return
                "ERR USAGE=CAN_MODE_"
                "<NORMAL|LISTEN|LOOPBACK>\n";
        }

        VMXCAN::VMXCANMode mode =
            VMXCAN::VMXCAN_NORMAL;

        const std::string requested(mode_text);

        if (requested == "NORMAL") {
            mode = VMXCAN::VMXCAN_NORMAL;
        } else if (requested == "LISTEN") {
            mode = VMXCAN::VMXCAN_LISTEN;
        } else if (requested == "LOOPBACK") {
            mode = VMXCAN::VMXCAN_LOOPBACK;
        } else {
            return "ERR INVALID_CAN_MODE\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.can.SetMode(mode, &error)) {
            return make_vmx_error(error);
        }

        vmx.time.DelayMilliseconds(20);

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CAN_MODE=%s\n",
            can_mode_name(mode)
        );

        return std::string(response);
    }


    if (command.rfind("TITAN_INIT", 0) == 0) {
        unsigned int can_id = 0;

        if (std::sscanf(
                command.c_str(),
                "TITAN_INIT %u",
                &can_id) != 1) {
            return "ERR USAGE=TITAN_INIT_<can_id_1_to_63>\n";
        }

        if (can_id < 1 || can_id > 63) {
            return "ERR TITAN_CAN_ID_RANGE=1..63\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.can.SetMode(VMXCAN::VMXCAN_NORMAL, &error)) {
            return make_vmx_error(error);
        }

        state.titan_configured = true;
        state.titan_can_id = static_cast<uint8_t>(can_id);
        state.titan_enabled = false;

        for (int motor = 0; motor < 4; ++motor) {
            state.titan_speed[motor] = 0.0;
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK TITAN_INIT CAN_ID=%u ENABLED=0\n",
            can_id
        );

        return std::string(response);
    }

    if (command == "TITAN_ENABLE") {
        if (!state.titan_configured) {
            return "ERR TITAN_NOT_INITIALIZED\n";
        }

        VMXErrorCode error = 0;

        if (!titan_enable(vmx, state, error)) {
            return make_vmx_error(error);
        }

        return "OK TITAN_ENABLE ENABLED=1\n";
    }

    if (command.rfind("TITAN_SET", 0) == 0) {
        unsigned int motor = 0;
        double speed = 0.0;

        if (std::sscanf(
                command.c_str(),
                "TITAN_SET %u %lf",
                &motor,
                &speed) != 2) {
            return "ERR USAGE=TITAN_SET_<motor_0_to_3>_<speed_-1_to_1>\n";
        }

        if (!state.titan_configured) {
            return "ERR TITAN_NOT_INITIALIZED\n";
        }

        if (motor > 3) {
            return "ERR TITAN_MOTOR_RANGE=0..3\n";
        }

        if (speed < -1.0 || speed > 1.0) {
            return "ERR TITAN_SPEED_RANGE=-1..1\n";
        }

        VMXErrorCode error = 0;

        if (!state.titan_enabled &&
            !titan_enable(vmx, state, error)) {
            return make_vmx_error(error);
        }

        const double magnitude = speed < 0.0 ? -speed : speed;
        int duty = static_cast<int>((magnitude * 100.0) + 0.5);

        if (duty > 100) {
            duty = 100;
        }

        uint8_t in_a = 1;
        uint8_t in_b = 1;

        if (speed > 0.0) {
            in_a = 1;
            in_b = 0;
        } else if (speed < 0.0) {
            in_a = 0;
            in_b = 1;
        }

        const uint8_t data[8] = {
            static_cast<uint8_t>(motor),
            static_cast<uint8_t>(duty),
            in_a,
            in_b,
            0,
            0,
            0,
            0
        };

        if (!send_titan_frame(
                vmx,
                state.titan_can_id,
                TITAN_SET_SPEED_INDEX,
                data,
                0,
                error)) {
            return make_vmx_error(error);
        }

        state.titan_speed[motor] = speed;

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK TITAN_SET MOTOR=%u SPEED=%.3f DUTY=%d\n",
            motor,
            speed,
            duty
        );

        return std::string(response);
    }

    if (command == "TITAN_STOP" ||
        command == "TITAN_DISABLE") {
        if (!state.titan_configured) {
            return "ERR TITAN_NOT_INITIALIZED\n";
        }

        VMXErrorCode error = 0;

        if (!titan_stop_and_disable(vmx, state, error)) {
            return make_vmx_error(error);
        }

        return "OK TITAN_STOP ENABLED=0\n";
    }

    if (command == "TITAN_STATUS") {
        if (!state.titan_configured) {
            return "OK TITAN_STATUS CONFIGURED=0\n";
        }

        char response[256];

        std::snprintf(
            response,
            sizeof(response),
            "OK TITAN_STATUS CONFIGURED=1 CAN_ID=%u ENABLED=%d "
            "M0=%.3f M1=%.3f M2=%.3f M3=%.3f\n",
            static_cast<unsigned int>(state.titan_can_id),
            state.titan_enabled ? 1 : 0,
            state.titan_speed[0],
            state.titan_speed[1],
            state.titan_speed[2],
            state.titan_speed[3]
        );

        return std::string(response);
    }

    if (command.rfind("CAN_SEND", 0) == 0) {
        unsigned int message_id = 0;
        char frame_text[8] = {0};
        char hex_text[17] = {0};

        if (std::sscanf(
                command.c_str(),
                "CAN_SEND %x %7s %16s",
                &message_id,
                frame_text,
                hex_text) != 3) {
            return
                "ERR USAGE=CAN_SEND_"
                "<id_hex>_<STD|EXT>_<hex_1_to_8_bytes>\n";
        }

        const std::string frame(frame_text);
        const bool standard = frame == "STD";
        const bool extended = frame == "EXT";

        if (!standard && !extended) {
            return "ERR CAN_FRAME_TYPE=STD|EXT\n";
        }

        if (standard && message_id > 0x7FFU) {
            return "ERR CAN_STANDARD_ID_RANGE=0..7FF\n";
        }

        if (extended && message_id > 0x1FFFFFFFU) {
            return
                "ERR CAN_EXTENDED_ID_RANGE=0..1FFFFFFF\n";
        }

        std::vector<uint8_t> data;

        if (!decode_hex(hex_text, data) ||
            data.empty() ||
            data.size() > 8) {
            return "ERR CAN_DATA_SIZE_RANGE=1..8_BYTES\n";
        }

        VMXCANMessage message {};
        message.messageID = message_id;
        message.dataSize =
            static_cast<uint8_t>(data.size());

        if (standard) {
            message.messageID |= VMXCAN_IS_FRAME_11BIT;
        }

        for (std::size_t index = 0;
             index < data.size();
             ++index) {
            message.data[index] = data[index];
        }

        VMXErrorCode error = 0;

        if (!vmx.can.SendMessage(
                message,
                0,
                &error)) {
            return make_vmx_error(error);
        }

        char prefix[144];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CAN_SEND ID=%08X FRAME=%s SIZE=%u HEX=",
            message_id,
            standard ? "STD" : "EXT",
            static_cast<unsigned int>(data.size())
        );

        return std::string(prefix) +
            encode_hex(data.data(), data.size()) +
            "\n";
    }

    if (command.rfind("CAN_OPEN", 0) == 0) {
        unsigned int filter = 0;
        unsigned int mask = 0;
        char frame_text[8] = {0};
        unsigned int capacity = 0;

        if (std::sscanf(
                command.c_str(),
                "CAN_OPEN %x %x %7s %u",
                &filter,
                &mask,
                frame_text,
                &capacity) != 4) {
            return
                "ERR USAGE=CAN_OPEN_"
                "<filter_hex>_<mask_hex>_"
                "<STD|EXT>_<capacity>\n";
        }

        const std::string frame(frame_text);
        const bool standard = frame == "STD";
        const bool extended = frame == "EXT";

        if (!standard && !extended) {
            return "ERR CAN_FRAME_TYPE=STD|EXT\n";
        }

        if (capacity < 1 || capacity > 1000) {
            return "ERR CAN_CAPACITY_RANGE=1..1000\n";
        }

        if (state.can_receive_streams.size() >=
            MAX_CAN_RECEIVE_STREAMS) {
            return "ERR CAN_STREAM_LIMIT=32\n";
        }

        if (standard) {
            if (filter > 0x7FFU || mask > 0x7FFU) {
                return
                    "ERR CAN_STANDARD_ID_RANGE=0..7FF\n";
            }

            filter |= VMXCAN_IS_FRAME_11BIT;
            mask |= VMXCAN_IS_FRAME_11BIT;
        } else {
            if (filter > 0x1FFFFFFFU ||
                mask > 0x1FFFFFFFU) {
                return
                    "ERR CAN_EXTENDED_ID_RANGE="
                    "0..1FFFFFFF\n";
            }
        }

        VMXCANReceiveStreamHandle handle = 0;
        VMXErrorCode error = 0;

        if (!vmx.can.OpenReceiveStream(
                handle,
                filter,
                mask,
                capacity,
                &error)) {
            return make_vmx_error(error);
        }

        state.can_receive_streams.push_back(handle);

        char response[176];

        std::snprintf(
            response,
            sizeof(response),
            "OK CAN_OPEN HANDLE=%u FRAME=%s "
            "FILTER=%08X MASK=%08X CAPACITY=%u\n",
            static_cast<unsigned int>(handle),
            standard ? "STD" : "EXT",
            standard
                ? filter & ~VMXCAN_IS_FRAME_11BIT
                : filter,
            standard
                ? mask & ~VMXCAN_IS_FRAME_11BIT
                : mask,
            capacity
        );

        return std::string(response);
    }

    if (command.rfind("CAN_READ", 0) == 0) {
        unsigned int handle_value = 0;

        if (std::sscanf(
                command.c_str(),
                "CAN_READ %u",
                &handle_value) != 1) {
            return "ERR USAGE=CAN_READ_<handle>\n";
        }

        const VMXCANReceiveStreamHandle handle =
            static_cast<VMXCANReceiveStreamHandle>(
                handle_value
            );

        if (!can_stream_is_open(state, handle)) {
            return "ERR CAN_STREAM_NOT_OPEN\n";
        }

        VMXCANTimestampedMessage message {};
        uint32_t messages_read = 0;
        VMXErrorCode error = 0;

        if (!vmx.can.ReadReceiveStream(
                handle,
                &message,
                1,
                messages_read,
                &error)) {
            return make_vmx_error(error);
        }

        if (messages_read == 0) {
            char response[80];

            std::snprintf(
                response,
                sizeof(response),
                "OK CAN_READ HANDLE=%u EMPTY=1\n",
                handle_value
            );

            return std::string(response);
        }

        if (message.dataSize > 8) {
            return "ERR CAN_RECEIVED_DATA_SIZE_INVALID\n";
        }

        const bool standard =
            (message.messageID &
             VMXCAN_IS_FRAME_11BIT) != 0;

        const bool remote =
            (message.messageID &
             VMXCAN_IS_FRAME_REMOTE) != 0;

        uint32_t message_id = message.messageID;
        message_id &= ~VMXCAN_IS_FRAME_11BIT;
        message_id &= ~VMXCAN_IS_FRAME_REMOTE;

        const std::string hex =
            encode_hex(
                message.data,
                message.dataSize
            );

        char prefix[240];

        std::snprintf(
            prefix,
            sizeof(prefix),
            "OK CAN_READ HANDLE=%u EMPTY=0 "
            "TIMESTAMP_MS=%lu ID=%08lX "
            "FRAME=%s REMOTE=%d SIZE=%u HEX=",
            handle_value,
            static_cast<unsigned long>(
                message.timeStampMS
            ),
            static_cast<unsigned long>(message_id),
            standard ? "STD" : "EXT",
            remote ? 1 : 0,
            static_cast<unsigned int>(
                message.dataSize
            )
        );

        return std::string(prefix) + hex + "\n";
    }

    if (command.rfind("CAN_CLOSE", 0) == 0) {
        unsigned int handle_value = 0;

        if (std::sscanf(
                command.c_str(),
                "CAN_CLOSE %u",
                &handle_value) != 1) {
            return "ERR USAGE=CAN_CLOSE_<handle>\n";
        }

        const VMXCANReceiveStreamHandle handle =
            static_cast<VMXCANReceiveStreamHandle>(
                handle_value
            );

        if (!can_stream_is_open(state, handle)) {
            return "ERR CAN_STREAM_NOT_OPEN\n";
        }

        VMXErrorCode error = 0;

        if (!vmx.can.CloseReceiveStream(
                handle,
                &error)) {
            return make_vmx_error(error);
        }

        remove_can_stream(state, handle);

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CAN_CLOSE HANDLE=%u\n",
            handle_value
        );

        return std::string(response);
    }

    if (command == "CAN_STATUS") {
        VMXCAN::VMXCANMode mode =
            VMXCAN::VMXCAN_OFF;

        VMXErrorCode error = 0;

        if (!vmx.can.GetMode(mode, &error)) {
            return make_vmx_error(error);
        }

        VMXCANBusStatus status {};
        error = 0;

        if (!vmx.can.GetCANBUSStatus(
                status,
                &error)) {
            return make_vmx_error(error);
        }

        char response[384];

        std::snprintf(
            response,
            sizeof(response),
            "OK CAN_STATUS MODE=%s STREAMS=%u "
            "UTILIZATION=%.3f WARNING=%d PASSIVE=%d "
            "BUS_OFF=%d BUS_ERROR=%d "
            "TX_ERROR=%lu RX_ERROR=%lu "
            "BUS_OFF_COUNT=%lu TX_FULL=%lu "
            "HW_RX_OVERFLOW=%d SW_RX_OVERFLOW=%d "
            "MESSAGE_ERROR=%d WAKE=%d\n",
            can_mode_name(mode),
            static_cast<unsigned int>(
                state.can_receive_streams.size()
            ),
            static_cast<double>(
                status.percentBusUtilization
            ),
            status.busWarning ? 1 : 0,
            status.busPassiveError ? 1 : 0,
            status.busOffError ? 1 : 0,
            status.busError ? 1 : 0,
            static_cast<unsigned long>(
                status.transmitErrorCount
            ),
            static_cast<unsigned long>(
                status.receiveErrorCount
            ),
            static_cast<unsigned long>(
                status.busOffCount
            ),
            static_cast<unsigned long>(
                status.txFullCount
            ),
            status.hwRxOverflow ? 1 : 0,
            status.swRxOverflow ? 1 : 0,
            status.messageError ? 1 : 0,
            status.wake ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "STATUS") {
        unsigned long dio_output_count = 0;
        unsigned long dio_input_count = 0;
        unsigned long pwm_count = 0;
        unsigned long analog_count = 0;
        unsigned long encoder_count = 0;
        unsigned long input_capture_count = 0;
        unsigned long pwm_capture_count = 0;
        unsigned long analog_trigger_count = 0;
        unsigned long uart_count = 0;
        unsigned long spi_count = 0;
        unsigned long i2c_count = 0;
        unsigned long led_array_count = 0;

        for (std::size_t channel = 0;
             channel < MAX_VMX_CHANNELS;
             ++channel) {

            if (state.dio_output_active[channel]) {
                ++dio_output_count;
            }

            if (state.dio_input_active[channel]) {
                ++dio_input_count;
            }

            if (state.pwm_active[channel]) {
                ++pwm_count;
            }

            if (state.analog_active[channel]) {
                ++analog_count;
            }

            if (state.encoder_active[channel] &&
                state.encoder_is_a[channel]) {
                ++encoder_count;
            }

            if (state.input_capture_active[channel] &&
                state.input_capture_is_primary[channel]) {
                ++input_capture_count;
            }

            if (state.pwm_capture_active[channel]) {
                ++pwm_capture_count;
            }

            if (state.analog_trigger_active[channel]) {
                ++analog_trigger_count;
            }

            if (state.uart_active[channel] &&
                state.uart_is_tx[channel]) {
                ++uart_count;
            }

            if (state.spi_active[channel] &&
                state.spi_role[channel] == 0) {
                ++spi_count;
            }

            if (state.i2c_active[channel] &&
                state.i2c_is_sda[channel]) {
                ++i2c_count;
            }

            if (state.led_array_active[channel]) {
                ++led_array_count;
            }
        }

        const unsigned long total_count =
            dio_output_count +
            dio_input_count +
            pwm_count +
            analog_count +
            encoder_count +
            input_capture_count +
            pwm_capture_count +
            analog_trigger_count +
            uart_count +
            spi_count +
            i2c_count +
            led_array_count;

        char response[320];

        std::snprintf(
            response,
            sizeof(response),
            "OK DIO_OUT=%lu DIO_IN=%lu PWM=%lu "
            "ANALOG=%lu ENCODER=%lu "
            "INPUT_CAPTURE=%lu PWM_CAPTURE=%lu "
            "ANALOG_TRIGGER=%lu UART=%lu "
            "SPI=%lu I2C=%lu "
            "LED_ARRAY=%lu TOTAL=%lu\n",
            dio_output_count,
            dio_input_count,
            pwm_count,
            analog_count,
            encoder_count,
            input_capture_count,
            pwm_capture_count,
            analog_trigger_count,
            uart_count,
            spi_count,
            i2c_count,
            led_array_count,
            total_count
        );

        return std::string(response);
    }

    if (command.rfind("CHANNEL_STATE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "CHANNEL_STATE %d",
                &channel) != 1) {
            return "ERR USAGE=CHANNEL_STATE_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        char response[320];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d DIO_OUT=%d DIO_IN=%d "
            "PWM=%d ANALOG=%d ENCODER=%d "
            "INPUT_CAPTURE=%d PWM_CAPTURE=%d "
            "ANALOG_TRIGGER=%d UART=%d SPI=%d "
            "I2C=%d LED_ARRAY=%d\n",
            channel,
            state.dio_output_active[channel] ? 1 : 0,
            state.dio_input_active[channel] ? 1 : 0,
            state.pwm_active[channel] ? 1 : 0,
            state.analog_active[channel] ? 1 : 0,
            state.encoder_active[channel] ? 1 : 0,
            state.input_capture_active[channel] ? 1 : 0,
            state.pwm_capture_active[channel] ? 1 : 0,
            state.analog_trigger_active[channel] ? 1 : 0,
            state.uart_active[channel] ? 1 : 0,
            state.spi_active[channel] ? 1 : 0,
            state.i2c_active[channel] ? 1 : 0,
            state.led_array_active[channel] ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "RELEASE_ALL") {
        unsigned long total_released = 0;

        for (std::size_t channel = 0;
             channel < MAX_VMX_CHANNELS;
             ++channel) {

            VMXErrorCode error = 0;
            unsigned int released_count = 0;

            if (!release_channel(
                    vmx,
                    state,
                    channel,
                    error,
                    released_count)) {
                return make_vmx_error(error);
            }

            total_released += released_count;
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK RELEASED=%lu\n",
            total_released
        );

        return std::string(response);
    }

    if (command.rfind("CHANNEL_RELEASE", 0) == 0) {
        int channel = -1;

        if (std::sscanf(
                command.c_str(),
                "CHANNEL_RELEASE %d",
                &channel) != 1) {
            return "ERR USAGE=CHANNEL_RELEASE_<channel>\n";
        }

        if (channel < 0 ||
            channel >= static_cast<int>(MAX_VMX_CHANNELS)) {
            return "ERR INVALID_CHANNEL\n";
        }

        VMXErrorCode error = 0;
        unsigned int released_count = 0;

        if (!release_channel(
                vmx,
                state,
                static_cast<std::size_t>(channel),
                error,
                released_count)) {
            return make_vmx_error(error);
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK CHANNEL=%d RELEASED=%u\n",
            channel,
            released_count
        );

        return std::string(response);
    }

    if (command == "WATCHDOG_STATUS") {
        bool enabled = false;
        bool expired = false;
        bool flexdio = false;
        bool hicurrdio = false;
        bool commdio = false;
        uint16_t timeout_ms = 0;
        VMXErrorCode error = 0;

        if (!vmx.io.GetWatchdogEnabled(
                enabled,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.GetWatchdogExpired(
                expired,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.GetWatchdogManagedOutputs(
                flexdio,
                hicurrdio,
                commdio,
                &error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.GetWatchdogTimeoutPeriodMS(
                timeout_ms,
                &error)) {
            return make_vmx_error(error);
        }

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK WATCHDOG ENABLED=%d EXPIRED=%d "
            "TIMEOUT_MS=%u FLEXDIO=%d "
            "HICURRDIO=%d COMMDIO=%d\n",
            enabled ? 1 : 0,
            expired ? 1 : 0,
            static_cast<unsigned int>(timeout_ms),
            flexdio ? 1 : 0,
            hicurrdio ? 1 : 0,
            commdio ? 1 : 0
        );

        return std::string(response);
    }

    if (command.rfind(
            "WATCHDOG_CONFIG",
            0) == 0) {

        unsigned int timeout_ms = 0;
        int enabled = -1;

        if (std::sscanf(
                command.c_str(),
                "WATCHDOG_CONFIG %u %d",
                &timeout_ms,
                &enabled) != 2) {
            return
                "ERR USAGE=WATCHDOG_CONFIG_"
                "<timeout_ms>_<0|1>\n";
        }

        if (timeout_ms < 100 ||
            timeout_ms > 60000) {
            return
                "ERR WATCHDOG_TIMEOUT_RANGE=100..60000\n";
        }

        if (enabled != 0 && enabled != 1) {
            return "ERR WATCHDOG_ENABLED_RANGE=0|1\n";
        }

        VMXErrorCode error = 0;

        if (!configure_watchdog(
                vmx,
                static_cast<uint16_t>(timeout_ms),
                enabled != 0,
                error)) {
            return make_vmx_error(error);
        }

        bool flexdio = false;
        bool hicurrdio = false;
        bool commdio = false;

        error = 0;

        if (!vmx.io.GetWatchdogManagedOutputs(
                flexdio,
                hicurrdio,
                commdio,
                &error)) {
            return make_vmx_error(error);
        }

        char response[192];

        std::snprintf(
            response,
            sizeof(response),
            "OK WATCHDOG_CONFIG ENABLED=%d "
            "TIMEOUT_MS=%u FLEXDIO=%d "
            "HICURRDIO=%d COMMDIO=%d\n",
            enabled,
            timeout_ms,
            flexdio ? 1 : 0,
            hicurrdio ? 1 : 0,
            commdio ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "WATCHDOG_FEED") {
        VMXErrorCode error = 0;

        if (!vmx.io.FeedWatchdog(&error)) {
            return make_vmx_error(error);
        }

        bool expired = false;
        error = 0;

        if (!vmx.io.GetWatchdogExpired(
                expired,
                &error)) {
            return make_vmx_error(error);
        }

        char response[80];

        std::snprintf(
            response,
            sizeof(response),
            "OK WATCHDOG_FEED EXPIRED=%d\n",
            expired ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "WATCHDOG_EXPIRE") {
        VMXErrorCode error = 0;

        if (!safe_stop_outputs(
                vmx,
                state,
                error)) {
            return make_vmx_error(error);
        }

        error = 0;

        if (!vmx.io.ExpireWatchdogNow(
                &error)) {
            return make_vmx_error(error);
        }

        bool expired = false;
        error = 0;

        if (!vmx.io.GetWatchdogExpired(
                expired,
                &error)) {
            return make_vmx_error(error);
        }

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK WATCHDOG_EXPIRE "
            "REQUESTED=1 EXPIRED=%d SAFE_STOP=1\n",
            expired ? 1 : 0
        );

        return std::string(response);
    }

    if (command == "SAFE_STOP") {
        VMXErrorCode error = 0;

        if (!safe_stop_outputs(vmx, state, error)) {
            return make_vmx_error(error);
        }

        return "OK SAFE_STOP\n";
    }

    if (command == "SERVER_STOP") {
        VMXErrorCode error = 0;

        if (!safe_stop_outputs(vmx, state, error)) {
            return make_vmx_error(error);
        }

        unsigned long total_released = 0;

        for (std::size_t channel = 0;
             channel < MAX_VMX_CHANNELS;
             ++channel) {

            unsigned int released_count = 0;
            error = 0;

            if (!release_channel(
                    vmx,
                    state,
                    channel,
                    error,
                    released_count)) {
                return make_vmx_error(error);
            }

            total_released += released_count;
        }

        stop_requested = 1;
        close_client = true;

        char response[96];

        std::snprintf(
            response,
            sizeof(response),
            "OK SERVER_STOPPING RELEASED=%lu\n",
            total_released
        );

        return std::string(response);
    }

    if (command == "QUIT") {
        close_client = true;
        return "OK BYE\n";
    }

    if (command.empty()) {
        return "ERR EMPTY_COMMAND\n";
    }

    return "ERR UNKNOWN_COMMAND\n";
}

int create_server_socket()
{
    unlink(SOCKET_PATH);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;

    std::snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "%s",
        SOCKET_PATH
    );

    if (bind(
            fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        perror("bind");
        close(fd);
        unlink(SOCKET_PATH);
        return -1;
    }

    if (chmod(SOCKET_PATH, 0660) < 0) {
        perror("chmod");
        close(fd);
        unlink(SOCKET_PATH);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("listen");
        close(fd);
        unlink(SOCKET_PATH);
        return -1;
    }

    return fd;
}

}  // namespace

int main()
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    std::puts("[vmx-controller] Initializing VMX HAL...");

    VMXPi vmx(false, 50);

    if (!vmx.IsOpen()) {
        std::fputs(
            "[vmx-controller] ERROR: VMX HAL initialization failed.\n",
            stderr
        );

        return 1;
    }

    std::printf(
        "[vmx-controller] HAL %s, firmware %s\n",
        vmx.version.GetHALVersion().c_str(),
        vmx.version.GetFirmwareVersion().c_str()
    );

    ControllerState controller_state;

    VMXErrorCode watchdog_error = 0;

    if (!configure_watchdog(
            vmx,
            1000,
            true,
            watchdog_error)) {
        std::fprintf(
            stderr,
            "[vmx-controller] ERROR: "
            "Watchdog configuration failed: %d\n",
            static_cast<int>(watchdog_error)
        );

        return 1;
    }

    bool watchdog_flexdio = false;
    bool watchdog_hicurrdio = false;
    bool watchdog_commdio = false;

    watchdog_error = 0;

    if (!vmx.io.GetWatchdogManagedOutputs(
            watchdog_flexdio,
            watchdog_hicurrdio,
            watchdog_commdio,
            &watchdog_error)) {
        std::fprintf(
            stderr,
            "[vmx-controller] ERROR: "
            "Watchdog read-back failed: %d\n",
            static_cast<int>(watchdog_error)
        );

        return 1;
    }

    std::printf(
        "[vmx-controller] Watchdog enabled: "
        "timeout=1000ms, flexdio=%d, "
        "hicurrdio=%d, commdio=%d.\n",
        watchdog_flexdio ? 1 : 0,
        watchdog_hicurrdio ? 1 : 0,
        watchdog_commdio ? 1 : 0
    );

    server_fd = create_server_socket();

    if (server_fd < 0) {
        return 1;
    }

    std::printf(
        "[vmx-controller] Listening on %s\n",
        SOCKET_PATH
    );

    while (!stop_requested) {
        const int client_fd = accept(server_fd, nullptr, nullptr);

        if (client_fd < 0) {
            if (stop_requested || errno == EBADF || errno == EINTR) {
                continue;
            }

            perror("accept");
            break;
        }

        timeval receive_timeout {};
        receive_timeout.tv_sec = 1;
        receive_timeout.tv_usec = 0;

        if (setsockopt(
                client_fd,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &receive_timeout,
                sizeof(receive_timeout)
            ) < 0) {
            perror("setsockopt(SO_RCVTIMEO)");
            close(client_fd);
            continue;
        }

        std::puts("[vmx-controller] Client connected.");

        send_all(
            client_fd,
            "OK VMX_CONTROLLER READY\n"
        );

        std::string command;

        while (!stop_requested && read_line(client_fd, command)) {
            bool close_client = false;

            const std::string response =
                process_command(
                    command,
                    vmx,
                    controller_state,
                    close_client
                );

            if (!send_all(client_fd, response) || close_client) {
                break;
            }
        }

        VMXErrorCode can_close_error = 0;
        unsigned int can_streams_closed = 0;

        if (!close_all_can_streams(
                vmx,
                controller_state,
                can_close_error,
                can_streams_closed)) {
            std::fprintf(
                stderr,
                "[vmx-controller] CAN stream cleanup failed: %d\n",
                static_cast<int>(can_close_error)
            );
        }

        VMXErrorCode stop_error = 0;

        if (!safe_stop_outputs(
                vmx,
                controller_state,
                stop_error)) {
            std::fprintf(
                stderr,
                "[vmx-controller] SAFE_STOP failed: %d\n",
                static_cast<int>(stop_error)
            );
        }

        close(client_fd);

        std::puts("[vmx-controller] Client disconnected.");
    }

    VMXErrorCode final_can_error = 0;
    unsigned int final_can_streams_closed = 0;

    if (!close_all_can_streams(
            vmx,
            controller_state,
            final_can_error,
            final_can_streams_closed)) {
        std::fprintf(
            stderr,
            "[vmx-controller] Final CAN cleanup failed: %d\n",
            static_cast<int>(final_can_error)
        );
    }

    VMXErrorCode final_stop_error = 0;

    if (!safe_stop_outputs(
            vmx,
            controller_state,
            final_stop_error)) {
        std::fprintf(
            stderr,
            "[vmx-controller] Final SAFE_STOP failed: %d\n",
            static_cast<int>(final_stop_error)
        );
    }

    for (std::size_t channel = 0;
         channel < MAX_VMX_CHANNELS;
         ++channel) {

        VMXErrorCode release_error = 0;
        unsigned int released_count = 0;

        if (!release_channel(
                vmx,
                controller_state,
                channel,
                release_error,
                released_count)) {
            std::fprintf(
                stderr,
                "[vmx-controller] Final release failed: "
                "channel=%lu error=%d\n",
                static_cast<unsigned long>(channel),
                static_cast<int>(release_error)
            );
        }
    }

    VMXErrorCode watchdog_disable_error = 0;

    if (!vmx.io.SetWatchdogEnabled(
            false,
            &watchdog_disable_error)) {
        std::fprintf(
            stderr,
            "[vmx-controller] Watchdog disable failed: %d\n",
            static_cast<int>(watchdog_disable_error)
        );
    }

    if (server_fd >= 0) {
        close(server_fd);
    }

    unlink(SOCKET_PATH);

    std::puts("[vmx-controller] Stopped.");

    return 0;
}
