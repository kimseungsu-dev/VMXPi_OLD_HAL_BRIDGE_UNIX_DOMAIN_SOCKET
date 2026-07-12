#!/usr/bin/env bash
set -Eeuo pipefail

SOCKET="/run/vmx-controller.sock"
BINARY="./vmx_controller32"
CLIENT64="./vmx_client64"
LOG="./smoke_server.log"
SERVER_PID=""

fail()
{
    echo
    echo "[FAIL] $*" >&2

    if [[ -f "$LOG" ]]; then
        echo
        echo "===== server log =====" >&2
        tail -n 120 "$LOG" >&2
    fi

    exit 1
}

cleanup()
{
    set +e

    if [[ -S "$SOCKET" ]]; then
        printf 'SERVER_STOP\n' |
            socat - UNIX-CONNECT:"$SOCKET" \
            >/dev/null 2>&1
    fi

    if [[ -n "${SERVER_PID:-}" ]]; then
        wait "$SERVER_PID" 2>/dev/null
    fi
}

trap cleanup EXIT INT TERM

request()
{
    local payload="$1"

    printf '%b' "$payload" |
        timeout 5s socat - UNIX-CONNECT:"$SOCKET"
}

expect_fixed()
{
    local output="$1"
    local expected="$2"

    if ! grep -Fq -- "$expected" <<<"$output"; then
        echo
        echo "===== response =====" >&2
        printf '%s\n' "$output" >&2
        fail "Expected response text not found: $expected"
    fi
}

expect_regex()
{
    local output="$1"
    local pattern="$2"

    if ! grep -Eq -- "$pattern" <<<"$output"; then
        echo
        echo "===== response =====" >&2
        printf '%s\n' "$output" >&2
        fail "Response did not match regex: $pattern"
    fi
}

echo "[1/19] Preflight checks"

[[ -x "$BINARY" ]] ||
    fail "Controller executable not found: $BINARY"

[[ -x "$CLIENT64" ]] ||
    fail "ARM64 client executable not found: $CLIENT64"

controller_file="$(file "$BINARY")"
client_file="$(file "$CLIENT64")"

grep -Eq 'ELF 32-bit.*ARM' <<<"$controller_file" ||
    fail "Controller is not a 32-bit ARM executable: $controller_file"

grep -Eq 'ELF 64-bit.*ARM aarch64' <<<"$client_file" ||
    fail "Client is not a 64-bit ARM executable: $client_file"

command -v socat >/dev/null ||
    fail "socat is not installed."

command -v timeout >/dev/null ||
    fail "timeout command is not installed."

rm -f "$SOCKET" "$LOG"

echo "[2/19] Starting server"

"$BINARY" >"$LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 200); do
    if [[ -S "$SOCKET" ]]; then
        break
    fi

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "Server exited before creating the socket."
    fi

    sleep 0.1
done

[[ -S "$SOCKET" ]] ||
    fail "Socket was not created within 20 seconds."

echo "[3/19] Basic protocol test"

output="$(request \
    $'GET_PROTOCOL\nPING\nGET_INFO\nREAD_ADC_SCALE\nGET_RTC_TIME\nSET_RTC_TIME 2026 2 30 12 00\nSTATUS\nQUIT\n')"

expect_fixed "$output" "OK VMX_CONTROLLER READY"
expect_fixed "$output" "OK PROTOCOL=1"
expect_fixed "$output" "OK PONG"
expect_fixed "$output" "OK HAL=1.1.257 FW=3.0"
expect_fixed "$output" "OK VOLTS=5.000"

expect_regex "$output"     'OK RTC YEAR=20[2-9][0-9] MONTH=([1-9]|1[0-2]) DAY=([1-9]|[12][0-9]|3[01]) WEEKDAY=[1-7] HOUR=([0-9]|1[0-9]|2[0-3]) MINUTE=([0-9]|[1-5][0-9]) SECOND=([0-9]|[1-5][0-9]) MILLISECOND=([0-9]|[1-9][0-9]{0,2})'

expect_fixed "$output"     "ERR INVALID_RTC_DATE_TIME"

expect_fixed "$output" \
    "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0"
expect_fixed "$output" "OK BYE"

echo "[4/19] DIO output/input resource test"

output="$(request \
    $'DIO_OPEN_OUT 0\nDIO_READ 0\nCHANNEL_STATE 0\nCHANNEL_RELEASE 0\nCHANNEL_STATE 0\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=0 MODE=OUTPUT VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=0 DIO_OUT=1 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=0 RELEASED=1"
expect_fixed "$output" \
    "OK CHANNEL=0 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"

output="$(request \
    $'DIO_OPEN_IN 1\nDIO_READ 1\nCHANNEL_RELEASE 1\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=1 MODE=INPUT PULL=PULLUP"
expect_regex "$output" \
    'OK CHANNEL=1 VALUE=[01]'
expect_fixed "$output" \
    "OK CHANNEL=1 RELEASED=1"

echo "[5/19] Encoder resource test"

output="$(request \
    $'ENCODER_OPEN 0 1\nENCODER_READ 0\nENCODER_READ 1\nENCODER_DIRECTION 0\nENCODER_PERIOD 0\nENCODER_RESET 0\nCHANNEL_STATE 0\nCHANNEL_STATE 1\nCHANNEL_RELEASE 1\nENCODER_READ 0\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL_A=0 CHANNEL_B=1 MODE=ENCODER EDGES=X4 COUNT=0"
expect_regex "$output" \
    'OK CHANNEL=0 ROLE=A PARTNER=1 COUNT=-?[0-9]+'
expect_regex "$output" \
    'OK CHANNEL=1 ROLE=B PARTNER=0 COUNT=-?[0-9]+'
expect_regex "$output" \
    'OK CHANNEL=0 ROLE=A PARTNER=1 DIRECTION=(FORWARD|REVERSE)'
expect_regex "$output" \
    'OK CHANNEL=0 ROLE=A PARTNER=1 PERIOD_US=[0-9]+'
expect_fixed "$output" \
    "OK CHANNEL=0 PARTNER=1 COUNT=0"
expect_fixed "$output" \
    "OK CHANNEL=0 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=1 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=1 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=1 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=1 RELEASED=1"
expect_fixed "$output" \
    "ERR ENCODER_NOT_OPEN"

echo "[6/19] Input/PWM capture resource test"

output="$(request \
    $'INPUT_CAPTURE_OPEN 4 5\nINPUT_CAPTURE_READ 4\nINPUT_CAPTURE_STATUS 4\nINPUT_CAPTURE_RESET 5\nCHANNEL_STATE 4\nCHANNEL_STATE 5\nCHANNEL_RELEASE 5\nINPUT_CAPTURE_READ 4\nPWM_CAPTURE_OPEN 6\nPWM_CAPTURE_READ 6\nPWM_CAPTURE_RESET 6\nCHANNEL_STATE 6\nCHANNEL_RELEASE 6\nPWM_CAPTURE_READ 6\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL_A=4 CHANNEL_B=5 MODE=INPUT_CAPTURE TICK_US=1 COUNT=0"
expect_regex "$output" \
    'OK CHANNEL=4 ROLE=CH1 PARTNER=5 CH1=[0-9]+ CH2=[0-9]+ COUNT=-?[0-9]+'
expect_regex "$output" \
    'OK CHANNEL=4 PARTNER=5 DIRECTION=(FORWARD|REVERSE) ACTIVE=[01]'
expect_fixed "$output" \
    "OK CHANNEL=5 PARTNER=4 COUNT=0"
expect_fixed "$output" \
    "OK CHANNEL=4 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=1 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=5 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=1 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=5 RELEASED=1"
expect_fixed "$output" \
    "ERR INPUT_CAPTURE_NOT_OPEN"
expect_regex "$output" \
    'OK CHANNEL=6 MODE=PWM_CAPTURE TICK_US=1 TIMEOUT_20MS=[0-9]+'
expect_regex "$output" \
    'OK CHANNEL=6 FREQUENCY_US=[0-9]+ DUTY_US=[0-9]+'
expect_fixed "$output" \
    "OK CHANNEL=6 RESET=1"
expect_fixed "$output" \
    "OK CHANNEL=6 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=1 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=6 RELEASED=1"
expect_fixed "$output" \
    "ERR PWM_CAPTURE_NOT_OPEN"

echo "[7/19] PWM resource test"

output="$(request \
    $'PWM_OPEN 26 200\nPWM_READ 26\nCHANNEL_RELEASE 26\nPWM_OPEN 26 500\nPWM_READ 26\nCHANNEL_RELEASE 26\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=26 MODE=PWM FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=26 MODE=PWM FREQUENCY=500 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=500 DUTY=0"

release_count="$(
    grep -Fc \
        "OK CHANNEL=26 RELEASED=1" \
        <<<"$output"
)"

[[ "$release_count" -eq 2 ]] ||
    fail "Expected two PWM resource release responses."

echo "[8/19] Analog resource test"

output="$(request \
    $'ANALOG_OPEN 22\nANALOG_READ 22\nCHANNEL_STATE 22\nCHANNEL_RELEASE 22\nANALOG_READ 22\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=22 MODE=ANALOG AVERAGE_BITS=3"
expect_regex "$output" \
    'OK CHANNEL=22 RAW=[0-9]+ VOLTS=-?[0-9]+\.[0-9]{3}'
expect_fixed "$output" \
    "OK CHANNEL=22 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=1 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"
expect_fixed "$output" \
    "OK CHANNEL=22 RELEASED=1"
expect_fixed "$output" \
    "ERR ANALOG_NOT_OPEN"

echo "[9/19] Accumulator and analog trigger test"

output="$(request \
    $'ACCUMULATOR_OPEN 22\nACCUMULATOR_READ 22\nANALOG_READ 22\nACCUMULATOR_COUNTER_READ 22\nACCUMULATOR_COUNTER_RESET 22\nCHANNEL_STATE 22\nCHANNEL_RELEASE 22\nACCUMULATOR_READ 22\nANALOG_TRIGGER_OPEN 22 2482 992 STATE\nANALOG_TRIGGER_READ 22\nCHANNEL_STATE 22\nCHANNEL_RELEASE 22\nANALOG_TRIGGER_READ 22\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=22 MODE=ACCUMULATOR OVERSAMPLE_BITS=0 AVERAGE_BITS=3 COUNTER=1 CENTER=0 DEADBAND=0"

expect_regex "$output" \
    'OK CHANNEL=22 OVERSAMPLE=[0-9]+ AVERAGE=[0-9]+ INSTANT=[0-9]+ VOLTS=-?[0-9]+\.[0-9]{3} FULL_SCALE=[0-9]+\.[0-9]{3} COUNTER=1'

expect_regex "$output" \
    'OK CHANNEL=22 RAW=[0-9]+ VOLTS=-?[0-9]+\.[0-9]{3}'

expect_regex "$output" \
    'OK CHANNEL=22 VALUE=-?[0-9]+ COUNT=[0-9]+'

expect_fixed "$output" \
    "OK CHANNEL=22 COUNTER_RESET=1"

expect_fixed "$output" \
    "OK CHANNEL=22 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=1 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=22 RELEASED=1"

expect_fixed "$output" \
    "ERR ACCUMULATOR_NOT_OPEN"

expect_fixed "$output" \
    "OK CHANNEL=22 MODE=ANALOG_TRIGGER THRESHOLD_HIGH=2482 THRESHOLD_LOW=992 TRIGGER_MODE=STATE"

expect_regex "$output" \
    'OK CHANNEL=22 STATE=(BELOW_THRESHOLD|ABOVE_THRESHOLD|IN_WINDOW)'

expect_fixed "$output" \
    "OK CHANNEL=22 DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=1 UART=0 SPI=0 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=22 RELEASED=1"

expect_fixed "$output" \
    "ERR ANALOG_TRIGGER_NOT_OPEN"

echo "[10/19] UART resource test"

find_uart_pair()
{
    local tx_channel
    local rx_channel
    local tx_output
    local rx_output
    local tx_caps_hex
    local rx_caps_hex
    local tx_caps
    local rx_caps
    local candidate_output

    for tx_channel in $(seq 0 33); do
        tx_output="$(request "$(
            printf 'CHANNEL_INFO %d\nQUIT\n' "$tx_channel"
        )")"

        tx_caps_hex="$(
            sed -n \
                's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
                <<<"$tx_output" |
            head -n 1
        )"

        [[ -n "$tx_caps_hex" ]] || continue

        tx_caps=$((16#$tx_caps_hex))

        (( (tx_caps & 0x00000800) != 0 )) ||
            continue

        for rx_channel in $(seq 0 33); do
            [[ "$tx_channel" -ne "$rx_channel" ]] ||
                continue

            rx_output="$(request "$(
                printf 'CHANNEL_INFO %d\nQUIT\n' "$rx_channel"
            )")"

            rx_caps_hex="$(
                sed -n \
                    's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
                    <<<"$rx_output" |
                head -n 1
            )"

            [[ -n "$rx_caps_hex" ]] || continue

            rx_caps=$((16#$rx_caps_hex))

            (( (rx_caps & 0x00001000) != 0 )) ||
                continue

            candidate_output="$(request "$(
                printf \
                    'UART_OPEN %d %d 57600\nUART_AVAILABLE %d\nUART_RELEASE %d\nQUIT\n' \
                    "$tx_channel" \
                    "$rx_channel" \
                    "$tx_channel" \
                    "$rx_channel"
            )")"

            if grep -Fq \
                "OK CHANNEL_TX=$tx_channel CHANNEL_RX=$rx_channel MODE=UART BAUD=57600" \
                <<<"$candidate_output"; then

                printf '%d %d\n' \
                    "$tx_channel" \
                    "$rx_channel"

                return 0
            fi
        done
    done

    return 1
}

uart_pair="$(find_uart_pair || true)"

[[ -n "$uart_pair" ]] ||
    fail "No compatible UART TX/RX channel pair found."

read -r uart_tx uart_rx <<<"$uart_pair"

output="$(request "$(
    printf \
        'UART_OPEN %d %d 57600\nUART_AVAILABLE %d\nUART_READ_HEX %d 16\nCHANNEL_STATE %d\nCHANNEL_STATE %d\nUART_RELEASE %d\nUART_AVAILABLE %d\nQUIT\n' \
        "$uart_tx" \
        "$uart_rx" \
        "$uart_tx" \
        "$uart_rx" \
        "$uart_tx" \
        "$uart_rx" \
        "$uart_rx" \
        "$uart_tx"
)")"

expect_fixed "$output" \
    "OK CHANNEL_TX=$uart_tx CHANNEL_RX=$uart_rx MODE=UART BAUD=57600"

expect_regex "$output" \
    "OK CHANNEL=$uart_tx ROLE=TX PARTNER=$uart_rx AVAILABLE=[0-9]+"

expect_regex "$output" \
    "OK CHANNEL=$uart_rx ROLE=RX PARTNER=$uart_tx SIZE=[0-9]+ HEX=[0-9A-F]*"

expect_fixed "$output" \
    "OK CHANNEL=$uart_tx DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=1 SPI=0 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$uart_rx DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=1 SPI=0 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$uart_rx UART_RELEASED=1"

expect_fixed "$output" \
    "ERR UART_NOT_OPEN"

echo "[11/19] SPI resource test"

find_spi_quad()
{
    local channel
    local output
    local caps_hex
    local caps
    local clk
    local miso
    local mosi
    local cs
    local candidate_output

    local -a clk_candidates=()
    local -a miso_candidates=()
    local -a mosi_candidates=()
    local -a cs_candidates=()

    for channel in $(seq 0 33); do
        output="$(request "$(
            printf 'CHANNEL_INFO %d\nQUIT\n' "$channel"
        )")"

        caps_hex="$(
            sed -n \
                's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
                <<<"$output" |
            head -n 1
        )"

        [[ -n "$caps_hex" ]] || continue

        caps=$((16#$caps_hex))

        if (( (caps & 0x00002000) != 0 )); then
            clk_candidates+=("$channel")
        fi

        if (( (caps & 0x00004000) != 0 )); then
            miso_candidates+=("$channel")
        fi

        if (( (caps & 0x00008000) != 0 )); then
            mosi_candidates+=("$channel")
        fi

        if (( (caps & 0x00010000) != 0 )); then
            cs_candidates+=("$channel")
        fi
    done

    for clk in "${clk_candidates[@]}"; do
        for miso in "${miso_candidates[@]}"; do
            [[ "$miso" -ne "$clk" ]] || continue

            for mosi in "${mosi_candidates[@]}"; do
                [[ "$mosi" -ne "$clk" ]] || continue
                [[ "$mosi" -ne "$miso" ]] || continue

                for cs in "${cs_candidates[@]}"; do
                    [[ "$cs" -ne "$clk" ]] || continue
                    [[ "$cs" -ne "$miso" ]] || continue
                    [[ "$cs" -ne "$mosi" ]] || continue

                    candidate_output="$(request "$(
                        printf \
                            'SPI_OPEN %d %d %d %d 1000000 3\nSPI_RELEASE %d\nQUIT\n' \
                            "$clk" \
                            "$miso" \
                            "$mosi" \
                            "$cs" \
                            "$cs"
                    )")"

                    if grep -Fq \
                        "OK CLK=$clk MISO=$miso MOSI=$mosi CS=$cs MODE=SPI BITRATE=1000000 SPI_MODE=3 CS_ACTIVE_LOW=1 MSB_FIRST=1" \
                        <<<"$candidate_output"; then

                        printf '%d %d %d %d\n' \
                            "$clk" \
                            "$miso" \
                            "$mosi" \
                            "$cs"

                        return 0
                    fi
                done
            done
        done
    done

    return 1
}

spi_quad="$(find_spi_quad || true)"

[[ -n "$spi_quad" ]] ||
    fail "No compatible SPI CLK/MISO/MOSI/CS channel set found."

read -r spi_clk spi_miso spi_mosi spi_cs <<<"$spi_quad"

output="$(request "$(
    printf \
        'SPI_OPEN %d %d %d %d 1000000 3\nCHANNEL_STATE %d\nCHANNEL_STATE %d\nCHANNEL_STATE %d\nCHANNEL_STATE %d\nSTATUS\nSPI_RELEASE %d\nSPI_READ_HEX %d 1\nQUIT\n' \
        "$spi_clk" \
        "$spi_miso" \
        "$spi_mosi" \
        "$spi_cs" \
        "$spi_clk" \
        "$spi_miso" \
        "$spi_mosi" \
        "$spi_cs" \
        "$spi_mosi" \
        "$spi_clk"
)")"

expect_fixed "$output" \
    "OK CLK=$spi_clk MISO=$spi_miso MOSI=$spi_mosi CS=$spi_cs MODE=SPI BITRATE=1000000 SPI_MODE=3 CS_ACTIVE_LOW=1 MSB_FIRST=1"

expect_fixed "$output" \
    "OK CHANNEL=$spi_clk DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=1 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$spi_miso DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=1 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$spi_mosi DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=1 I2C=0 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$spi_cs DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=1 I2C=0 LED_ARRAY=0"

expect_regex "$output" \
    'OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=1 I2C=0 LED_ARRAY=0 TOTAL=1'

expect_fixed "$output" \
    "OK CHANNEL=$spi_mosi SPI_RELEASED=1"

expect_fixed "$output" \
    "ERR SPI_NOT_OPEN"

echo "[12/19] I2C resource test"

find_i2c_pair()
{
    local channel
    local output
    local caps_hex
    local caps
    local sda
    local scl
    local candidate_output

    local -a sda_candidates=()
    local -a scl_candidates=()

    for channel in $(seq 0 33); do
        output="$(request "$(
            printf 'CHANNEL_INFO %d\nQUIT\n' "$channel"
        )")"

        caps_hex="$(
            sed -n \
                's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
                <<<"$output" |
            head -n 1
        )"

        [[ -n "$caps_hex" ]] || continue

        caps=$((16#$caps_hex))

        if (( (caps & 0x00020000) != 0 )); then
            sda_candidates+=("$channel")
        fi

        if (( (caps & 0x00040000) != 0 )); then
            scl_candidates+=("$channel")
        fi
    done

    for sda in "${sda_candidates[@]}"; do
        for scl in "${scl_candidates[@]}"; do
            [[ "$sda" -ne "$scl" ]] || continue

            candidate_output="$(request "$(
                printf \
                    'I2C_OPEN %d %d\nI2C_RELEASE %d\nQUIT\n' \
                    "$sda" \
                    "$scl" \
                    "$scl"
            )")"

            if grep -Fq \
                "OK CHANNEL_SDA=$sda CHANNEL_SCL=$scl MODE=I2C" \
                <<<"$candidate_output"; then

                printf '%d %d\n' "$sda" "$scl"
                return 0
            fi
        done
    done

    return 1
}

i2c_pair="$(find_i2c_pair || true)"

[[ -n "$i2c_pair" ]] ||
    fail "No compatible I2C SDA/SCL channel pair found."

read -r i2c_sda i2c_scl <<<"$i2c_pair"

output="$(request "$(
    printf \
        'I2C_OPEN %d %d\nI2C_OPEN %d %d\nCHANNEL_STATE %d\nCHANNEL_STATE %d\nSTATUS\nI2C_READ_HEX %d 128 0 1\nI2C_WRITE_HEX %d 0 0 GG\nI2C_RELEASE %d\nI2C_READ_HEX %d 0 0 1\nQUIT\n' \
        "$i2c_sda" \
        "$i2c_scl" \
        "$i2c_sda" \
        "$i2c_scl" \
        "$i2c_sda" \
        "$i2c_scl" \
        "$i2c_sda" \
        "$i2c_sda" \
        "$i2c_scl" \
        "$i2c_sda"
)")"

expect_fixed "$output" \
    "OK CHANNEL_SDA=$i2c_sda CHANNEL_SCL=$i2c_scl MODE=I2C"

expect_fixed "$output" \
    "OK CHANNEL_SDA=$i2c_sda CHANNEL_SCL=$i2c_scl MODE=I2C ALREADY_OPEN=1"

expect_fixed "$output" \
    "OK CHANNEL=$i2c_sda DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=1 LED_ARRAY=0"

expect_fixed "$output" \
    "OK CHANNEL=$i2c_scl DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=1 LED_ARRAY=0"

expect_regex "$output" \
    'OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=1 LED_ARRAY=0 TOTAL=1'

expect_fixed "$output" \
    "ERR DEVICE_ADDRESS_RANGE=0..127"

expect_fixed "$output" \
    "ERR INVALID_HEX_OR_SIZE_MAX_BYTES=1024"

expect_fixed "$output" \
    "OK CHANNEL=$i2c_scl I2C_RELEASED=1"

expect_fixed "$output" \
    "ERR I2C_NOT_OPEN"

echo "[13/19] One-Wire LED array test"

find_led_array_channel()
{
    local channel
    local output
    local caps_hex
    local caps
    local candidate_output

    for channel in $(seq 0 33); do
        output="$(request "$(
            printf 'CHANNEL_INFO %d\nQUIT\n' "$channel"
        )")"

        caps_hex="$(
            sed -n \
                's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
                <<<"$output" |
            head -n 1
        )"

        [[ -n "$caps_hex" ]] || continue

        caps=$((16#$caps_hex))

        (( (caps & 0x00080000) != 0 )) ||
            continue

        candidate_output="$(request "$(
            printf \
                'LED_ARRAY_OPEN %d 1 GRB\nLED_ARRAY_RELEASE %d\nQUIT\n' \
                "$channel" \
                "$channel"
        )")"

        if grep -Fq \
            "OK CHANNEL=$channel MODE=LED_ARRAY PIXELS=1 FORMAT=GRB FREQUENCY_HZ=800000" \
            <<<"$candidate_output"; then

            printf '%d\n' "$channel"
            return 0
        fi
    done

    return 1
}

led_channel="$(find_led_array_channel || true)"

[[ -n "$led_channel" ]] ||
    fail "No compatible One-Wire LED array channel found."

output="$(request "$(
    printf \
        'LED_ARRAY_OPEN %d 4 GRB\nLED_ARRAY_OPEN %d 4 GRB\nLED_ARRAY_SET_PIXEL %d 0 1 2 3\nLED_ARRAY_GET_PIXEL %d 0\nLED_ARRAY_FILL %d 4 5 6\nLED_ARRAY_GET_PIXEL %d 3\nLED_ARRAY_SHOW %d\nLED_ARRAY_CLEAR %d\nLED_ARRAY_GET_PIXEL %d 0\nCHANNEL_STATE %d\nSTATUS\nLED_ARRAY_RELEASE %d\nLED_ARRAY_GET_PIXEL %d 0\nQUIT\n' \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel"
)")"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel MODE=LED_ARRAY PIXELS=4 FORMAT=GRB FREQUENCY_HZ=800000"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel MODE=LED_ARRAY PIXELS=4 FORMAT=GRB FREQUENCY_HZ=800000 ALREADY_OPEN=1"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=1 G=2 B=3 UPDATED=1"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=1 G=2 B=3"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel PIXELS=4 R=4 G=5 B=6 FILLED=1"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=3 R=4 G=5 B=6"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel RENDERED=1"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel PIXELS=4 CLEARED=1"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=0 G=0 B=0"

expect_fixed "$output" \
    "OK CHANNEL=$led_channel DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=1"

expect_regex "$output" \
    'OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=1 TOTAL=1'

expect_fixed "$output" \
    "OK CHANNEL=$led_channel LED_ARRAY_RELEASED=1"

expect_fixed "$output" \
    "ERR LED_ARRAY_NOT_OPEN"

echo "[14/19] navX/IMU test"

output="$(request "$(
    printf '%s\n' \
        'IMU_STATUS' \
        'IMU_ORIENTATION' \
        'IMU_QUATERNION' \
        'IMU_LINEAR_ACCEL' \
        'IMU_RAW' \
        'IMU_VELOCITY' \
        'IMU_DISPLACEMENT' \
        'IMU_ENVIRONMENT' \
        'IMU_ZERO_YAW' \
        'IMU_RESET_DISPLACEMENT' \
        'IMU_DISPLACEMENT' \
        'QUIT'
)")"

imu_number='[-+]?[0-9]+\.[0-9]+'

expect_regex "$output" \
    'OK IMU_STATUS CONNECTED=1 CALIBRATING=[01] MOVING=[01] ROTATING=[01] MAG_CALIBRATED=[01] MAG_DISTURBANCE=[01] ALTITUDE_VALID=[01] ACTUAL_RATE_HZ=[0-9]+ REQUESTED_RATE_HZ=[0-9]+ SENSOR_TIMESTAMP=-?[0-9]+ UPDATE_COUNT=[0-9]+ BYTE_COUNT=[0-9]+ FW=[^ ]+ YAW_AXIS=(X|Y|Z|UNKNOWN) YAW_AXIS_UP=[01]'

expect_regex "$output" \
    "OK IMU_ORIENTATION YAW=$imu_number PITCH=$imu_number ROLL=$imu_number COMPASS=$imu_number FUSED=$imu_number ANGLE=$imu_number RATE=$imu_number"

expect_regex "$output" \
    "OK IMU_QUATERNION W=$imu_number X=$imu_number Y=$imu_number Z=$imu_number"

expect_regex "$output" \
    "OK IMU_LINEAR_ACCEL X_G=$imu_number Y_G=$imu_number Z_G=$imu_number MOVING=[01] ROTATING=[01]"

expect_regex "$output" \
    "OK IMU_RAW GYRO_X=$imu_number GYRO_Y=$imu_number GYRO_Z=$imu_number ACCEL_X=$imu_number ACCEL_Y=$imu_number ACCEL_Z=$imu_number MAG_X=$imu_number MAG_Y=$imu_number MAG_Z=$imu_number TEMP_C=$imu_number"

expect_regex "$output" \
    "OK IMU_VELOCITY X_MPS=$imu_number Y_MPS=$imu_number Z_MPS=$imu_number"

expect_regex "$output" \
    "OK IMU_DISPLACEMENT X_M=$imu_number Y_M=$imu_number Z_M=$imu_number"

expect_regex "$output" \
    "OK IMU_ENVIRONMENT ALTITUDE_M=$imu_number ALTITUDE_VALID=[01] BARO_PRESSURE=$imu_number PRESSURE=$imu_number TEMP_C=$imu_number MAG_CALIBRATED=[01] MAG_DISTURBANCE=[01]"

expect_fixed "$output" \
    "OK IMU_ZERO_YAW=1"

expect_fixed "$output" \
    "OK IMU_RESET_DISPLACEMENT=1"

echo "[15/19] Safe-stop and watchdog test"

output="$(request \
    $'WATCHDOG_STATUS\nWATCHDOG_CONFIG 1000 1\nWATCHDOG_FEED\nWATCHDOG_STATUS\nQUIT\n')"

expect_regex "$output" \
    'OK WATCHDOG ENABLED=1 EXPIRED=[01] TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]'

expect_regex "$output" \
    'OK WATCHDOG_CONFIG ENABLED=1 TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]'

expect_fixed "$output" \
    "OK WATCHDOG_FEED EXPIRED=0"

expect_regex "$output" \
    "OK WATCHDOG ENABLED=1 EXPIRED=0 TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]"

output="$(request "$(
    printf \
        'DIO_OPEN_OUT 0\nDIO_WRITE 0 1\nPWM_OPEN 26 200\nPWM_WRITE 26 200\nLED_ARRAY_OPEN %d 4 GRB\nLED_ARRAY_FILL %d 8 9 10\nLED_ARRAY_SHOW %d\nSAFE_STOP\nDIO_READ 0\nPWM_READ 26\nLED_ARRAY_GET_PIXEL %d 0\nRELEASE_ALL\nQUIT\n' \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel"
)")"

expect_fixed "$output" "OK SAFE_STOP"
expect_fixed "$output" "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=0 G=0 B=0"

output="$(request "$(
    printf \
        'DIO_OPEN_OUT 0\nDIO_WRITE 0 1\nPWM_OPEN 26 200\nPWM_WRITE 26 200\nLED_ARRAY_OPEN %d 4 GRB\nLED_ARRAY_FILL %d 11 12 13\nLED_ARRAY_SHOW %d\n' \
        "$led_channel" \
        "$led_channel" \
        "$led_channel"
)")"

sleep 0.2

output="$(request "$(
    printf \
        'DIO_READ 0\nPWM_READ 26\nLED_ARRAY_GET_PIXEL %d 0\nRELEASE_ALL\nQUIT\n' \
        "$led_channel"
)")"

expect_fixed "$output" "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=0 G=0 B=0"

output="$(
    {
        printf \
            'DIO_OPEN_OUT 0\nDIO_WRITE 0 1\nPWM_OPEN 26 200\nPWM_WRITE 26 200\n'
        sleep 2
    } |
        timeout 5s socat - UNIX-CONNECT:"$SOCKET"
)"

sleep 0.2

output="$(request \
    $'DIO_READ 0\nPWM_READ 26\nRELEASE_ALL\nQUIT\n')"

expect_fixed "$output" "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=200 DUTY=0"

output="$(request "$(
    printf \
        'WATCHDOG_FEED\nDIO_OPEN_OUT 0\nDIO_WRITE 0 1\nPWM_OPEN 26 200\nPWM_WRITE 26 200\nLED_ARRAY_OPEN %d 4 GRB\nLED_ARRAY_FILL %d 14 15 16\nLED_ARRAY_SHOW %d\nWATCHDOG_EXPIRE\nWATCHDOG_STATUS\nWATCHDOG_FEED\nWATCHDOG_STATUS\nDIO_READ 0\nPWM_READ 26\nLED_ARRAY_GET_PIXEL %d 0\nRELEASE_ALL\nQUIT\n' \
        "$led_channel" \
        "$led_channel" \
        "$led_channel" \
        "$led_channel"
)")"

expect_regex "$output" \
    'OK WATCHDOG_EXPIRE REQUESTED=1 EXPIRED=[01] SAFE_STOP=1'

expect_regex "$output" \
    "OK WATCHDOG ENABLED=1 EXPIRED=[01] TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]"

expect_fixed "$output" \
    "OK WATCHDOG_FEED EXPIRED=0"

expect_regex "$output" \
    "OK WATCHDOG ENABLED=1 EXPIRED=0 TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]"

expect_fixed "$output" "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=26 FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=$led_channel INDEX=0 R=0 G=0 B=0"

echo "[16/19] Release-all resource test"

output="$(request \
    $'DIO_OPEN_OUT 0\nDIO_OPEN_IN 1\nENCODER_OPEN 2 3\nPWM_OPEN 26 200\nANALOG_OPEN 22\nSTATUS\nRELEASE_ALL\nSTATUS\nQUIT\n')"

expect_fixed "$output" \
    "OK CHANNEL=0 MODE=OUTPUT VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=1 MODE=INPUT PULL=PULLUP"
expect_fixed "$output" \
    "OK CHANNEL_A=2 CHANNEL_B=3 MODE=ENCODER EDGES=X4 COUNT=0"
expect_fixed "$output" \
    "OK CHANNEL=26 MODE=PWM FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK CHANNEL=22 MODE=ANALOG AVERAGE_BITS=3"
expect_fixed "$output" \
    "OK DIO_OUT=1 DIO_IN=1 PWM=1 ANALOG=1 ENCODER=1 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=5"
expect_fixed "$output" \
    "OK RELEASED=5"
expect_fixed "$output" \
    "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0"

echo "[17/19] ARM64 native client test"

output="$(
    timeout 5s "$CLIENT64" \
        "GET_PROTOCOL" \
        "PING" \
        "GET_INFO" \
        "STATUS" \
        "QUIT"
)"

expect_fixed "$output" "OK VMX_CONTROLLER READY"
expect_fixed "$output" "OK PROTOCOL=1"
expect_fixed "$output" "OK PONG"
expect_fixed "$output" "OK HAL=1.1.257 FW=3.0"
expect_fixed "$output" \
    "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0"
expect_fixed "$output" "OK BYE"

output="$(
    timeout 5s "$CLIENT64" \
        "DIO_OPEN_OUT 0" \
        "DIO_READ 0" \
        "CHANNEL_RELEASE 0" \
        "QUIT"
)"

expect_fixed "$output" \
    "OK CHANNEL=0 MODE=OUTPUT VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=0 VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=0 RELEASED=1"
expect_fixed "$output" "OK BYE"

echo "[18/19] Final resource state and clean shutdown test"

output="$(request $'STATUS\nQUIT\n')"

expect_fixed "$output" \
    "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0"

output="$(request \
    $'DIO_OPEN_OUT 0\nPWM_OPEN 26 200\nSERVER_STOP\n')"

expect_fixed "$output" \
    "OK CHANNEL=0 MODE=OUTPUT VALUE=0"
expect_fixed "$output" \
    "OK CHANNEL=26 MODE=PWM FREQUENCY=200 DUTY=0"
expect_fixed "$output" \
    "OK SERVER_STOPPING RELEASED=2"

if ! wait "$SERVER_PID"; then
    fail "Server exited with a nonzero status."
fi

SERVER_PID=""

[[ ! -e "$SOCKET" ]] ||
    fail "Socket file remains after server shutdown."

echo "[19/19] HAL shutdown log verification"

if grep -Eqi \
    'already allocated|No compatible Resources' \
    "$LOG"; then
    fail "HAL resource allocation error found in the log."
fi

for label in \
    "Write CRC Mismatches" \
    "Write Retries" \
    "Write Failures" \
    "Read CRC Mismatches" \
    "Read Retries" \
    "Read Failures"
do
    if ! grep -Eq \
        "${label}:[[:space:]]+0" \
        "$LOG"; then
        fail "HAL statistic is nonzero or missing: $label"
    fi
done

grep -Fq \
    "pigpio library closed." \
    "$LOG" ||
    fail "pigpio clean shutdown log not found."

trap - EXIT INT TERM

echo
echo "========================================"
echo "PASS: VMX controller smoke test"
echo "========================================"
echo "DIO OUT     : PASS"
echo "DIO IN      : PASS"
echo "PWM         : PASS"
echo "ENCODER     : PASS"
echo "CAPTURE     : PASS"
echo "ANALOG      : PASS"
echo "ACCUMULATOR : PASS"
echo "ANALOG TRIG : PASS"
echo "UART        : PASS"
echo "SPI         : PASS"
echo "I2C         : PASS"
echo "LED ARRAY   : PASS"
echo "IMU         : PASS"
echo "SAFE STOP   : PASS"
echo "WATCHDOG    : PASS"
echo "IDLE TIMEOUT: PASS"
echo "RESOURCE    : PASS"
echo "RELEASE ALL : PASS"
echo "ARM64 CLIENT: PASS"
echo "PROTOCOL V1 : PASS"
echo "SOCKET      : PASS"
echo "HAL CRC     : PASS"
echo "CLEAN STOP  : PASS"
