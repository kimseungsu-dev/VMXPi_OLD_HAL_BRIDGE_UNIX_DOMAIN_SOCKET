from pathlib import Path
from datetime import datetime
import re

path = Path("main.cpp")
text = path.read_text(encoding="utf-8")

if "TITAN_PERIODIC_V1" in text:
    raise SystemExit("Titan periodic patch is already applied.")

backup = Path(
    f"main.cpp.backup-titan-periodic-{datetime.now():%Y%m%d-%H%M%S}"
)
backup.write_text(text, encoding="utf-8")

for header in (
    "#include <atomic>\n",
    "#include <chrono>\n",
    "#include <mutex>\n",
    "#include <thread>\n",
):
    if header not in text:
        include_pos = text.find("#include")
        line_end = text.find("\n", include_pos) + 1
        text = text[:line_end] + header + text[line_end:]

state_anchor = "    // TITAN_BRIDGE_V1\n"

if text.count(state_anchor) != 1:
    raise SystemExit(
        f"Titan state anchor count must be 1. Found: "
        f"{text.count(state_anchor)}"
    )

text = text.replace(
    state_anchor,
    """    // TITAN_BRIDGE_V1
    // TITAN_PERIODIC_V1
    std::mutex titan_mutex;
""",
    1,
)

helper_pattern = re.compile(
    r"bool titan_enable\(\n"
    r".*?"
    r"\nbool safe_stop_outputs\(",
    re.DOTALL,
)

helper_replacement = r'''void titan_set_safe_state(ControllerState& state)
{
    std::lock_guard<std::mutex> lock(state.titan_mutex);

    state.titan_enabled = false;

    for (int motor = 0; motor < 4; ++motor) {
        state.titan_speed[motor] = 0.0;
    }
}

void titan_periodic_worker(
    VMXPi& vmx,
    ControllerState& state,
    std::atomic<bool>& stop_worker
)
{
    unsigned int cycle = 0;

    while (!stop_worker.load()) {
        bool configured = false;
        bool enabled = false;
        uint8_t can_id = 42;
        double speeds[4] = {0.0, 0.0, 0.0, 0.0};

        {
            std::lock_guard<std::mutex> lock(state.titan_mutex);

            configured = state.titan_configured;
            enabled = state.titan_enabled;
            can_id = state.titan_can_id;

            for (int motor = 0; motor < 4; ++motor) {
                speeds[motor] = state.titan_speed[motor];
            }
        }

        if (configured) {
            if (enabled && cycle % 5U == 0U) {
                const uint8_t enable_data[8] = {
                    0, 0, 0, 0, 0, 0, 0, 0
                };

                VMXErrorCode error = 0;

                send_titan_frame(
                    vmx,
                    can_id,
                    TITAN_ENABLED_INDEX,
                    enable_data,
                    0,
                    error
                );
            }

            for (uint8_t motor = 0; motor < 4; ++motor) {
                double speed = enabled ? speeds[motor] : 0.0;

                if (speed > 1.0) {
                    speed = 1.0;
                }

                if (speed < -1.0) {
                    speed = -1.0;
                }

                const double magnitude =
                    speed < 0.0 ? -speed : speed;

                int duty = static_cast<int>(
                    magnitude * 100.0 + 0.5
                );

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

                const uint8_t motor_data[8] = {
                    motor,
                    static_cast<uint8_t>(duty),
                    in_a,
                    in_b,
                    0,
                    0,
                    0,
                    0
                };

                VMXErrorCode error = 0;

                send_titan_frame(
                    vmx,
                    can_id,
                    TITAN_SET_SPEED_INDEX,
                    motor_data,
                    0,
                    error
                );
            }
        }

        ++cycle;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(20)
        );
    }
}

bool safe_stop_outputs('''

text, count = helper_pattern.subn(
    lambda match: helper_replacement,
    text,
    count=1,
)

if count != 1:
    raise SystemExit(f"Titan helper replacement failed: {count}")

safe_old = '''    if (state.titan_configured) {
        VMXErrorCode error = 0;

        if (!titan_stop_and_disable(vmx, state, error)) {
            success = false;
            last_error = error;
        }
    }
'''

safe_new = '''    if (state.titan_configured) {
        titan_set_safe_state(state);
    }
'''

if text.count(safe_old) != 1:
    raise SystemExit(
        f"Safe-stop Titan block count must be 1. Found: "
        f"{text.count(safe_old)}"
    )

text = text.replace(safe_old, safe_new, 1)

command_pattern = re.compile(
    r'    if \(command\.rfind\("TITAN_INIT", 0\) == 0\) \{'
    r'.*?'
    r'(?=    if \(command\.rfind\("CAN_SEND", 0\) == 0\) \{)',
    re.DOTALL,
)

command_replacement = r'''    if (command.rfind("TITAN_INIT", 0) == 0) {
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

        {
            std::lock_guard<std::mutex> lock(state.titan_mutex);

            state.titan_configured = true;
            state.titan_can_id =
                static_cast<uint8_t>(can_id);
            state.titan_enabled = false;

            for (int motor = 0; motor < 4; ++motor) {
                state.titan_speed[motor] = 0.0;
            }
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
        std::lock_guard<std::mutex> lock(state.titan_mutex);

        if (!state.titan_configured) {
            return "ERR TITAN_NOT_INITIALIZED\n";
        }

        state.titan_enabled = true;

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
            return
                "ERR USAGE=TITAN_SET_"
                "<motor_0_to_3>_<speed_-1_to_1>\n";
        }

        if (motor > 3) {
            return "ERR TITAN_MOTOR_RANGE=0..3\n";
        }

        if (speed < -1.0 || speed > 1.0) {
            return "ERR TITAN_SPEED_RANGE=-1..1\n";
        }

        {
            std::lock_guard<std::mutex> lock(state.titan_mutex);

            if (!state.titan_configured) {
                return "ERR TITAN_NOT_INITIALIZED\n";
            }

            if (!state.titan_enabled) {
                return "ERR TITAN_NOT_ENABLED\n";
            }

            state.titan_speed[motor] = speed;
        }

        char response[128];

        std::snprintf(
            response,
            sizeof(response),
            "OK TITAN_SET MOTOR=%u SPEED=%.3f\n",
            motor,
            speed
        );

        return std::string(response);
    }

    if (command == "TITAN_STOP" ||
        command == "TITAN_DISABLE") {

        {
            std::lock_guard<std::mutex> lock(state.titan_mutex);

            if (!state.titan_configured) {
                return "ERR TITAN_NOT_INITIALIZED\n";
            }

            state.titan_enabled = false;

            for (int motor = 0; motor < 4; ++motor) {
                state.titan_speed[motor] = 0.0;
            }
        }

        return "OK TITAN_STOP ENABLED=0 OUTPUTS=ZERO\n";
    }

    if (command == "TITAN_STATUS") {
        std::lock_guard<std::mutex> lock(state.titan_mutex);

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

'''

text, count = command_pattern.subn(
    lambda match: command_replacement,
    text,
    count=1,
)

if count != 1:
    raise SystemExit(f"Titan command replacement failed: {count}")

worker_start_anchor = '''    std::printf(
        "[vmx-controller] Listening on %s\\n",
        SOCKET_PATH
    );

    while (!stop_requested) {
'''

worker_start_new = '''    std::printf(
        "[vmx-controller] Listening on %s\\n",
        SOCKET_PATH
    );

    std::atomic<bool> titan_worker_stop(false);

    std::thread titan_worker(
        titan_periodic_worker,
        std::ref(vmx),
        std::ref(controller_state),
        std::ref(titan_worker_stop)
    );

    while (!stop_requested) {
'''

if text.count(worker_start_anchor) != 1:
    raise SystemExit(
        f"Worker start anchor count must be 1. Found: "
        f"{text.count(worker_start_anchor)}"
    )

text = text.replace(
    worker_start_anchor,
    worker_start_new,
    1,
)

worker_stop_anchor = '''    VMXErrorCode final_can_error = 0;
'''

worker_stop_new = '''    titan_worker_stop.store(true);

    if (titan_worker.joinable()) {
        titan_worker.join();
    }

    VMXErrorCode final_can_error = 0;
'''

if text.count(worker_stop_anchor) != 1:
    raise SystemExit(
        f"Worker stop anchor count must be 1. Found: "
        f"{text.count(worker_stop_anchor)}"
    )

text = text.replace(
    worker_stop_anchor,
    worker_stop_new,
    1,
)

path.write_text(text, encoding="utf-8")

print("Patched:", path)
print("Backup :", backup)
