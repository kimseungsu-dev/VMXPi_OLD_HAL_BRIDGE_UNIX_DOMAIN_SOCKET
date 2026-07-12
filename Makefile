.RECIPEPREFIX := >

CXX := arm-linux-gnueabihf-g++
TARGET := vmx_controller32
SOURCE := main.cpp

CPPFLAGS := -I/usr/local/include/vmxpi
CXXFLAGS := -std=gnu++11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS := -L/usr/local/lib/vmxpi
LDLIBS := -lvmxpi_hal_cpp -lrt -lpthread -latomic

.PHONY: all clean run check

all: $(TARGET)

$(TARGET): $(SOURCE)
>$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCE) -o $(TARGET) $(LDFLAGS) $(LDLIBS)

check: $(TARGET)
>file $(TARGET)

run: $(TARGET)
>./$(TARGET)

clean:
>rm -f $(TARGET)

# ARM64_NATIVE_CLIENT
HOST_CXX ?= g++
CLIENT64_TARGET := vmx_client64
CLIENT64_SOURCE := vmx_client64.cpp

.PHONY: client64 check-client64 clean-client64

client64: $(CLIENT64_TARGET)

$(CLIENT64_TARGET): $(CLIENT64_SOURCE)
>$(HOST_CXX) -std=gnu++11 -O2 -Wall -Wextra -Wpedantic $< -o $@

check-client64: $(CLIENT64_TARGET)
>file $(CLIENT64_TARGET)

clean-client64:
>rm -f $(CLIENT64_TARGET)

# FULL_BRIDGE_SMOKE_TEST
.PHONY: smoke

smoke: $(TARGET) $(CLIENT64_TARGET)
>./smoke_test.sh

# RELEASE_CHECK
.PHONY: release-check

release-check: smoke
>file $(TARGET) $(CLIENT64_TARGET)
>@echo
>@echo "release-check: PASS"


SERVICE_NAME := vmx-controller.service
INSTALL_DIR ?= /opt/vmxpi-bridge

.PHONY: install-service uninstall-service service-start service-stop service-restart service-status service-test

install-service: $(TARGET) $(CLIENT64_TARGET)
>./install.sh --skip-deps --skip-build --install-dir $(INSTALL_DIR)

uninstall-service:
>./uninstall.sh --install-dir $(INSTALL_DIR)

service-start:
>systemctl start $(SERVICE_NAME)

service-stop:
>systemctl stop $(SERVICE_NAME)

service-restart:
>systemctl restart $(SERVICE_NAME)

service-status:
>systemctl --no-pager --full status $(SERVICE_NAME)

service-test: install-service
>VMX_INSTALL_DIR=$(INSTALL_DIR) ./systemd_test.sh

.PHONY: healthcheck final-audit

healthcheck: $(CLIENT64_TARGET)
>./healthcheck.sh

final-audit: $(TARGET) $(CLIENT64_TARGET)
>./final_audit.sh

.PHONY: repo-check

repo-check:
>./repo_check.sh
