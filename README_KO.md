[English](README.md) | **한국어**

# VMX-pi Unix Domain Socket Bridge

구형 ARMHF 32비트 VMX-pi HAL을 ARM64 Linux 프로그램에서 사용할 수 있도록 만든 Unix Domain Socket 기반 하드웨어 제어 브리지입니다.

## 프로젝트 구조

- `vmx_controller32`: ARMHF 32비트 VMX-pi HAL 제어 서버
- `vmx_client64`: ARM64 네이티브 Unix Domain Socket 클라이언트
- 소켓 경로: `/run/vmx-controller.sock`
- 통신 방식: 줄바꿈으로 구분되는 텍스트 명령
- 동시 활성 클라이언트: 1개

ARM64 프로그램이 32비트 HAL 라이브러리를 직접 로드하지 않고, 소켓을 통해 VMX 하드웨어를 제어할 수 있습니다.

## 주요 기능

- DIO 입력 및 출력
- PWM 출력
- Encoder
- Input Capture
- PWM Capture
- Analog Input
- Accumulator
- Analog Trigger
- UART
- SPI
- I2C
- One-Wire LED Array
- navX / IMU
- VMX RTC 읽기 및 설정
- Linux 시스템 시각 동기화
- VMX Watchdog
- 자원 추적 및 안전한 해제

## 안전 기능

다음 상황에서 출력 안전정지를 수행합니다.

- `SAFE_STOP`
- `SERVER_STOP`
- 클라이언트 연결 종료
- 클라이언트 수신 타임아웃
- 소켓 전송 실패
- 서버 정상 종료

안전정지 상태:

- DIO 출력: LOW
- PWM 출력: Duty 0
- LED Array: RGB 0, 0, 0 적용 후 렌더링
- 활성 자원 해제

## 요구 환경

- Raspberry Pi 4
- ARM64 Debian 또는 DietPi
- VMX-pi
- VMX-pi HAL 1.1.257 ARMHF
- systemd
- 인터넷 연결 또는 로컬 HAL Debian 패키지

## 자동 설치

저장소 디렉터리에서 실행합니다.

    sudo ./install.sh

HAL이 설치돼 있지 않으면 기본 보관 주소에서 VMX-pi HAL 1.1.257 ARMHF 패키지를 내려받고 SHA256 값을 검증합니다.

로컬 HAL 패키지를 사용하려면 다음과 같이 실행합니다.

    sudo ./install.sh \
        --hal-deb /path/to/vmxpi-hal_1.1.257_armhf.deb

기본 설치 설정:

- 설치 경로: `/opt/vmxpi-bridge`
- 서비스: `vmx-controller.service`
- CPU affinity: Raspberry Pi 4의 CPU 3
- 시간대: `Asia/Seoul`
- 부팅 자동 실행: 활성화

## 서비스 관리

상태 확인:

    systemctl status vmx-controller.service

시작:

    systemctl start vmx-controller.service

중지:

    systemctl stop vmx-controller.service

재시작:

    systemctl restart vmx-controller.service

로그 확인:

    journalctl -u vmx-controller.service -n 100 --no-pager

## 정상 동작 확인

전역 명령:

    vmx-healthcheck

프로젝트 디렉터리에서:

    make healthcheck

전체 기능 및 복구 검사:

    make final-audit

GitHub 배포 파일 검사:

    make repo-check

## RTC 설정

다음 형식으로 VMX RTC를 설정합니다.

    set_rtc_time 연도 월 일 시 분

예시:

    set_rtc_time 2026 7 12 14 30

브리지 시작 시 VMX RTC 값을 이용해 Linux 시스템 시각을 동기화합니다.

## CPU 사용 특성

VMX-pi HAL 1.1.257 내부의 실시간 AUX SPI 폴링 스레드는 CPU 코어 하나를 지속적으로 사용할 수 있습니다.

기본 설치에서는 VMX 프로세스를 CPU 3에 고정하여 Linux와 일반 프로그램이 사용하는 다른 코어에 미치는 영향을 줄입니다.

이 CPU 사용은 Unix Domain Socket 브리지 자체가 아니라 구형 VMX-pi HAL 내부 통신 구조에서 발생합니다.

## 알려진 제한 사항

- HAL에서 `PI_BAD_ISR_INIT` 오류가 발생하므로 인터럽트 기능은 비활성화돼 있습니다.
- UART, SPI, I2C는 연결하는 장치별 물리 검증이 필요합니다.
- RGBW LED 형식은 공개 HAL 버퍼 API에서 지원되지 않습니다.
- WS281x 계열 LED는 강제 종료나 전원 차단 때 마지막 색상을 유지할 수 있습니다.
- HAL watchdog의 `COMMDIO` 관리는 펌웨어에 따라 비활성화될 수 있습니다.
- watchdog 만료 상태가 feed 이후에도 펌웨어 내부에 유지될 수 있습니다.

## 제거

    sudo ./uninstall.sh

브리지 서비스와 `/opt/vmxpi-bridge` 설치 파일을 제거합니다.

VMX-pi HAL 패키지는 자동으로 제거하지 않습니다.

## 라이선스

이 저장소의 자체 개발 코드는 MIT 라이선스로 배포됩니다.

VMX-pi HAL과 관련 헤더 및 바이너리는 이 저장소에 포함하지 않으며, 원저작자와 배포자가 정한 별도의 저작권 및 라이선스 조건을 따릅니다. 자세한 내용은 `THIRD_PARTY_NOTICES.md`를 확인하십시오.
