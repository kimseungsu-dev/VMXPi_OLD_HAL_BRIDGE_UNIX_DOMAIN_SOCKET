[English](README.md) | [한국어](README_KO.md)

# VMX-pi Unix Domain Socket Bridge

Independent bridge for using the legacy VMX-pi HAL 1.1.257 ARMHF library from native ARM64 applications.

## Architecture

- vmx_controller32: ARMHF 32-bit VMX HAL controller
- vmx_client64: native ARM64 client
- Socket: /run/vmx-controller.sock
- Protocol: newline-terminated text commands
- One active client session at a time

## Build

    make clean
    make
    make client64

## Testing

Do not run make release-check while the systemd service is active because both processes require exclusive access to pigpio and the VMX SPI interface.

Use the complete audit instead:

    make final-audit

The final audit automatically stops the service, runs the standalone bridge test, tests systemd recovery, and leaves the service running.

## Service installation

    make install-service

## Service control

    make service-start
    make service-stop
    make service-restart
    make service-status
    make service-test

## Healthcheck

    make healthcheck

## Safety behavior

The controller places outputs into the following safe state:

- DIO output: LOW
- PWM output: duty cycle 0
- One-Wire LED array: RGB 0,0,0 followed by render

Safe-stop runs on:

- SAFE_STOP
- SERVER_STOP
- Client disconnect
- Receive timeout
- Socket send failure
- Process shutdown

## Watchdog

Default configuration:

- Enabled: yes
- Timeout: 1000 ms
- FlexDIO: managed
- High-current DIO: managed
- CommDIO: firmware dependent

VMX firmware 3.0.436 reports COMMDIO=0.

The firmware may also report EXPIRED=0 immediately after WATCHDOG_EXPIRE. The protocol therefore reports the expiration request and firmware read-back separately.

## Supported interfaces

- DIO input and output
- PWM
- Encoder
- Input capture
- PWM capture
- Analog input
- Accumulator
- Analog trigger
- UART
- SPI
- I2C
- One-Wire LED array
- navX / IMU
- HAL watchdog
- Resource tracking and release

## Known limitations

- Interrupt support is disabled because the HAL reports PI_BAD_ISR_INIT.
- RGBW LED formats are unavailable through the public HAL buffer API.
- FlexDIO paired PWM resources are not supported.
- UART, SPI and I2C require device-specific physical testing.
- WS281x-style LEDs can retain their last color after sudden power loss or SIGKILL.


## Installation

When the VMX-pi HAL is not already installed, the installer downloads
VMX-pi HAL 1.1.257 ARMHF from the configured archive URL.

On a fresh ARM64 system:

    sudo ./install.sh

To use a local package instead:

    sudo ./install.sh --hal-deb /path/to/vmxpi-hal_1.1.257_armhf.deb

To override the download location:

    sudo ./install.sh --hal-url https://example.com/vmxpi-hal_1.1.257_armhf.deb

Default runtime settings:

- Install directory: `/opt/vmxpi-bridge`
- Service: `vmx-controller.service`
- CPU affinity: CPU 3 on Raspberry Pi 4
- Timezone: `Asia/Seoul`
- Boot startup: enabled

Installed commands:

    set_rtc_time 2026 7 12 11 20
    vmx-healthcheck

Uninstall:

    sudo ./uninstall.sh


## Repository validation

Before publishing:

    make repo-check
    make final-audit

Generated binaries, VMX-pi HAL packages, logs and development context files
are excluded from Git.
