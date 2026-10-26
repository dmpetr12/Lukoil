#!/usr/bin/env bash
set -Eeuo pipefail

# Run on the Ubuntu panel as its desktop user, from a copied source checkout.
SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

seed_runtime() {
    local source_dir="$1" runtime_dir="$2" file
    mkdir -p -- "$runtime_dir/logs" "$runtime_dir/backups" "$runtime_dir/releases"
    for file in config.json lines.json schedule.json; do
        # Settings on an installed cabinet belong to that cabinet.
        if [[ ! -e "$runtime_dir/$file" ]]; then
            cp -- "$source_dir/$file" "$runtime_dir/$file"
        fi
    done
}

rollback_install() {
    local runtime_dir="$1" old_release="$2" backup_dir="$3" code="$4"
    trap - ERR
    echo "Installation failed; settings backup: $backup_dir" >&2
    if [[ -n "$old_release" ]]; then
        ln -sfn -- "$old_release" "$runtime_dir/current"
        if [[ -f "$backup_dir/lukoil-backend.service" ]]; then
            sudo cp -- "$backup_dir/lukoil-backend.service" /etc/systemd/system/lukoil-backend.service || true
            sudo systemctl daemon-reload || true
        fi
        sudo systemctl restart lukoil-backend.service || true
    fi
    exit "$code"
}

main() {
    if [[ "${1:-}" == --help ]]; then
        echo 'Usage: bash update_panel.sh [--check]'
        echo 'Builds this local source and installs it for the current Ubuntu desktop user.'
        return
    fi
    for file in CMakeLists.txt config.json lines.json schedule.json deploy/wait_backend.py; do
        [[ -f "$SOURCE_DIR/$file" ]] || { echo "Missing source file: $file" >&2; return 1; }
    done
    if [[ "${1:-}" == --check ]]; then
        echo "Source layout OK: $SOURCE_DIR"
        return
    fi
    [[ $# -eq 0 ]] || { echo 'Unknown argument; use --help' >&2; return 1; }
    [[ $(id -u) -ne 0 ]] || { echo 'Run as the desktop user, without sudo.' >&2; return 1; }
    [[ -f /etc/os-release ]] || { echo 'Ubuntu 24.04 is required.' >&2; return 1; }
    # shellcheck disable=SC1091
    source /etc/os-release
    [[ "$ID" == ubuntu && "$VERSION_ID" == 24.04 ]] || {
        echo 'This installer targets Ubuntu 24.04.' >&2; return 1;
    }

    local user_name user_home user_uid runtime build release previous='' opcua=OFF new_config=0
    user_name=$(id -un)
    user_uid=$(id -u)
    user_home=$(getent passwd "$user_name" | cut -d: -f6)
    runtime="$user_home/Lukoil-runtime"
    build="$SOURCE_DIR/build/ubuntu-release"
    # These paths are embedded in systemd, shell and desktop-entry files below.
    [[ "$user_home" =~ ^/[a-zA-Z0-9_./-]+$ ]] || {
        echo 'The desktop user home must use ASCII letters, digits, /, ., _ and -.' >&2; return 1;
    }
    for old_service in dialog-g2-engine panelFull panel-full panel-guard panel; do
        if systemctl is-active --quiet "$old_service.service"; then
            echo "Stop the previous cabinet service first: sudo systemctl disable --now $old_service.service" >&2
            return 1
        fi
    done
    local managed_pid pid
    managed_pid=$(systemctl show -p MainPID --value lukoil-backend.service 2>/dev/null || true)
    while read -r pid; do
        [[ "$pid" == "$managed_pid" ]] || {
            echo "Another panel-backend is running (PID $pid). Stop it before deployment." >&2; return 1;
        }
    done < <(pgrep -x panel-backend || true)

    sudo -v
    sudo apt-get update
    sudo apt-get install -y --no-upgrade \
        build-essential cmake ninja-build pkg-config python3 psmisc \
        qt6-base-dev qt6-declarative-dev qt6-httpserver-dev qt6-websockets-dev \
        qt6-serialbus-dev qt6-serialport-dev qt6-qpa-plugins \
        qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-controls \
        qml6-module-qtquick-layouts qml6-module-qtquick-templates \
        qml6-module-qtqml-workerscript fonts-ubuntu

    local cmake_options=()
    if [[ -n "${QT_OPCUA_SOURCE_DIR:-}" ]]; then
        [[ -f "$QT_OPCUA_SOURCE_DIR/src/3rdparty/open62541/open62541.c" &&
           -f "$QT_OPCUA_SOURCE_DIR/src/3rdparty/open62541/open62541.h" ]] || {
            echo 'QT_OPCUA_SOURCE_DIR must contain src/3rdparty/open62541/open62541.{c,h}.' >&2; return 1;
        }
        opcua=ON
        cmake_options+=("-DPANEL_QTOPCUA_SOURCE_DIR=$QT_OPCUA_SOURCE_DIR")
    fi
    echo "Building Lukoil (OPC UA: $opcua); the installed service is still running."
    cmake -S "$SOURCE_DIR" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$build/bin" \
        "-DPANEL_ENABLE_OPCUA_SERVER=$opcua" "${cmake_options[@]}"
    cmake --build "$build" --parallel "${BUILD_JOBS:-2}"
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$build" --output-on-failure
    bash "$SOURCE_DIR/tests/deployment_script_tests.sh"
    python3 -m unittest discover -s "$SOURCE_DIR/tests" -p deployment_wait_tests.py

    [[ -e "$runtime/config.json" ]] || new_config=1
    seed_runtime "$SOURCE_DIR" "$runtime"
    python3 - "$runtime" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for name in ("config.json", "lines.json", "schedule.json"):
    json.loads((root / name).read_text(encoding="utf-8-sig"))
PY
    release=$(mktemp -d "$runtime/releases/release-XXXXXXXX")
    install -m 755 "$build/bin/panel-backend" "$build/bin/panel-hmi" "$release/"
    install -m 644 "$SOURCE_DIR/deploy/wait_backend.py" "$release/"
    cp -a -- "$SOURCE_DIR/web/dist" "$release/web-dist"
    [[ ! -L "$runtime/current" ]] || previous=$(readlink -f "$runtime/current")
    [[ ! -e "$runtime/current" || -L "$runtime/current" ]] || {
        echo "$runtime/current must be a symlink, not a directory." >&2; return 1;
    }

    local backup rollback_command
    backup=$(mktemp -d "$runtime/backups/settings-XXXXXXXX")
    if [[ -f /etc/systemd/system/lukoil-backend.service ]]; then
        sudo cp -- /etc/systemd/system/lukoil-backend.service "$backup/"
    fi
    printf -v rollback_command 'rollback_install %q %q %q "$?"' "$runtime" "$previous" "$backup"
    trap "$rollback_command" ERR
    if systemctl cat lukoil-backend.service >/dev/null 2>&1; then
        sudo systemctl stop lukoil-backend.service
    fi
    for file in config.json lines.json schedule.json maintenance.json; do
        [[ ! -f "$runtime/$file" ]] || cp -a -- "$runtime/$file" "$backup/"
    done
    if [[ -n "$previous" ]]; then
        ln -sfn -- "$previous" "$runtime/previous"
    fi
    if [[ "$new_config" == 1 ]]; then
        # First installation only: panel mode; OPC UA is an explicit build option.
        python3 - "$runtime/config.json" "$opcua" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text(encoding="utf-8-sig"))
data.setdefault("logging", {})["level"] = "INFO"
if sys.argv[2] == "OFF":
    data.setdefault("opcUa", {})["enabled"] = False
p.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY
    fi
    ln -sfn -- "$release" "$runtime/current"
    mkdir -p "$runtime/web"
    ln -sfn -- "$runtime/current/web-dist" "$runtime/web/dist"
    sudo usermod -aG dialout "$user_name"
    sudo tee /etc/systemd/system/lukoil-backend.service >/dev/null <<SERVICE
[Unit]
Description=Lukoil emergency cabinet backend
After=network.target

[Service]
Type=simple
User=$user_name
SupplementaryGroups=dialout
WorkingDirectory=$runtime
ExecStart=$runtime/current/panel-backend
ExecStartPost=/usr/bin/python3 $runtime/current/wait_backend.py
Restart=on-failure
RestartSec=2
TimeoutStartSec=30
AmbientCapabilities=CAP_SYS_TIME CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_SYS_TIME CAP_NET_BIND_SERVICE
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
SERVICE
    cat > "$runtime/start-hmi.sh" <<LAUNCHER
#!/usr/bin/env bash
set -euo pipefail
cd '$runtime'
until /usr/bin/python3 '$runtime/current/wait_backend.py'; do
    sleep 2
done
export QT_QPA_PLATFORM="\${QT_QPA_PLATFORM:-xcb}"
exec '$runtime/current/panel-hmi'
LAUNCHER
    chmod 755 "$runtime/start-hmi.sh"
    mkdir -p "$user_home/.config/autostart"
    cat > "$user_home/.config/autostart/lukoil-hmi.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Lukoil HMI
Exec=$runtime/start-hmi.sh
Path=$runtime
Terminal=false
X-GNOME-Autostart-enabled=true
DESKTOP
    sudo systemctl daemon-reload
    sudo systemctl enable lukoil-backend.service
    sudo systemctl restart lukoil-backend.service
    trap - ERR
    # Replace only HMI processes belonging to this installation.
    while read -r pid; do
        case "$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)" in
            "$runtime"/releases/*/panel-hmi) kill "$pid" ;;
        esac
    done < <(pgrep -u "$user_uid" -x panel-hmi || true)
    if [[ -n "${DISPLAY:-}" ]]; then
        nohup "$runtime/start-hmi.sh" >> "$runtime/logs/hmi.log" 2>&1 &
    fi
    echo "Installed: $runtime"
    echo "Settings backup: $backup"
    echo 'Backend: systemctl status lukoil-backend.service'
    echo 'HMI: starts on desktop login; log out/in or reboot after first installation.'
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
