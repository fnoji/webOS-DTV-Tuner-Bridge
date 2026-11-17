#!/bin/sh
# webOS TV Tuner Bridge 起動・停止・自動起動管理スクリプト
# (システムパーティションは変更せず、/media/developer/apps/... 配下のみで完結)
APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
NATIVE_DIR="${APP_DIR}/native"
PID_FILE="/tmp/webos_tuner_d.pid"
LOG_FILE="/tmp/webos_tuner_d.log"
LOCK_FILE="/tmp/webos_tuner_d.lock"
INIT_HOOK="/var/lib/webosbrew/init.d/90-webostuner"
POWER_CFG="${NATIVE_DIR}/power_mode.conf"

get_power_mode() {
    if [ -f "$POWER_CFG" ] && grep -q '^ondemand' "$POWER_CFG" 2>/dev/null; then
        echo "ondemand"
    elif [ -f "$POWER_CFG" ] && grep -q '^shutdown' "$POWER_CFG" 2>/dev/null; then
        echo "shutdown"
    else
        echo "always"
    fi
}

set_power_mode() {
    MODE="$1"
    echo "$MODE" > "$POWER_CFG"
    if pidof webos_tuner_d >/dev/null 2>&1; then
        wget -qO- "http://127.0.0.1:40772/api/power?mode=${MODE}" >/dev/null 2>&1 || true
    fi
    echo "POWER_MODE=$MODE"
}

ensure_ls2_role() {
    LS2_BASE="/var/luna-service2-dev"
    [ -d "$LS2_BASE/roles.d" ] || LS2_BASE="/var/luna-service2"
    ROLE_FILE="$LS2_BASE/roles.d/io.github.webostuner.daemon.role.json"
    PERM_FILE="$LS2_BASE/client-permissions.d/io.github.webostuner.daemon.perm.json"
    MANI_FILE="$LS2_BASE/manifests.d/io.github.webostuner.daemon.json"
    NEED_SCAN=0
    if [ ! -f "$ROLE_FILE" ] || ! grep -q "${NATIVE_DIR}/webos_tuner_d" "$ROLE_FILE" 2>/dev/null; then
        mkdir -p "$LS2_BASE/roles.d" "$LS2_BASE/client-permissions.d" "$LS2_BASE/manifests.d"
        cat <<EOF > "$ROLE_FILE"
{
    "exeName": "${NATIVE_DIR}/webos_tuner_d",
    "type": "privileged",
    "allowedNames": ["io.github.webostuner.daemon*"],
    "permissions": [
        {
            "service": "io.github.webostuner.daemon*",
            "outbound": ["*"]
        }
    ]
}
EOF
        NEED_SCAN=1
    fi
    if [ ! -f "$PERM_FILE" ]; then
        cat <<EOF > "$PERM_FILE"
{
    "io.github.webostuner.daemon*": ["all"]
}
EOF
        NEED_SCAN=1
    fi
    if [ ! -f "$MANI_FILE" ]; then
        cat <<EOF > "$MANI_FILE"
{
    "id": "io.github.webostuner.daemon",
    "version": "0.1.0",
    "roleFiles": ["$ROLE_FILE"],
    "clientPermissionFiles": ["$PERM_FILE"]
}
EOF
        NEED_SCAN=1
    fi
    if [ "$NEED_SCAN" = "1" ]; then
        ls-control scan-services >/dev/null 2>&1 || true
    fi
}

cleanup_ls2_role() {
    rm -f /var/luna-service2-dev/roles.d/io.github.webostuner.daemon.role.json \
          /var/luna-service2-dev/client-permissions.d/io.github.webostuner.daemon.perm.json \
          /var/luna-service2-dev/manifests.d/io.github.webostuner.daemon.json \
          /var/luna-service2/roles.d/io.github.webostuner.daemon.role.json \
          /var/luna-service2/client-permissions.d/io.github.webostuner.daemon.perm.json \
          /var/luna-service2/manifests.d/io.github.webostuner.daemon.json 2>/dev/null || true
    ls-control scan-services >/dev/null 2>&1 || true
}

start_daemon() {
    [ -f "${APP_DIR}/appinfo.json" ] || exit 0
    if pidof webos_tuner_d >/dev/null 2>&1; then
        echo "ALREADY_RUNNING pid=$(pidof webos_tuner_d)"
        return 0
    fi
    chmod 755 "${NATIVE_DIR}/webos_inject" "${NATIVE_DIR}/webos_tuner_d" "${NATIVE_DIR}/tunerctl.sh" 2>/dev/null || true
    ensure_ls2_role

    (
        exec 9>"$LOCK_FILE"
        flock -n 9 || exit 0
        # 起動直後等で tvservice がまだ初期化中なら最大 30 秒待つ
        i=0
        while [ "$i" -lt 30 ]; do
            if pidof tvservice >/dev/null 2>&1; then
                break
            fi
            i=$((i + 1))
            sleep 1
        done
        sleep 1
        nohup "${NATIVE_DIR}/webos_tuner_d" \
            --so "${NATIVE_DIR}/libwebos_tuner.so" \
            --inj "${NATIVE_DIR}/webos_inject" \
            > "$LOG_FILE" 2>&1 < /dev/null &
        echo $! > "$PID_FILE"
    ) </dev/null >/dev/null 2>&1 &
    sleep 1
    if pidof webos_tuner_d >/dev/null 2>&1; then
        echo "STARTED pid=$(pidof webos_tuner_d)"
    else
        echo "STARTING (check $LOG_FILE)"
    fi
}

stop_daemon() {
    killall webos_tuner_d 2>/dev/null || true
    rm -f "$PID_FILE"
    echo "STOPPED"
}

status_daemon() {
    RUNNING=0
    PID=""
    if pidof webos_tuner_d >/dev/null 2>&1; then
        RUNNING=1
        PID="$(pidof webos_tuner_d)"
    fi
    AUTOSTART=0
    if [ -x "$INIT_HOOK" ] || [ -L "$INIT_HOOK" ]; then
        AUTOSTART=1
    fi
    LOCAL_IP="$(ifconfig 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p' | grep -v '^127\.' | head -n 1)"
    [ -n "$LOCAL_IP" ] || LOCAL_IP="$(ip -4 addr show 2>/dev/null | sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | grep -v '^127\.' | head -n 1)"
    PMODE="$(get_power_mode)"
    echo "{\"running\":$RUNNING,\"pid\":\"$PID\",\"autostart\":$AUTOSTART,\"powerMode\":\"$PMODE\",\"ip\":\"$LOCAL_IP\",\"appDir\":\"$APP_DIR\"}"
}

case "${1:-}" in
    start) start_daemon ;;
    stop) stop_daemon ;;
    restart) stop_daemon; sleep 1; start_daemon ;;
    power-always) set_power_mode always ;;
    power-ondemand) set_power_mode ondemand ;;
    power-shutdown) set_power_mode shutdown ;;
    enable-autostart)
        mkdir -p /var/lib/webosbrew/init.d
        cat <<EOF > "$INIT_HOOK"
#!/bin/sh
(
    # ブート直後は /media/developer が未マウントの場合があるため最大 45 秒待機
    i=0
    while [ "\$i" -lt 45 ]; do
        if [ -f "${APP_DIR}/appinfo.json" ]; then
            break
        fi
        sleep 1
        i=\$((i + 1))
    done
    [ -f "${APP_DIR}/appinfo.json" ] || exit 0

    sleep 2
    LD_PRELOAD="" nohup "${APP_DIR}/native/tunerctl.sh" start >/tmp/autostart_wt.log 2>&1 &
) </dev/null >/dev/null 2>&1 &
EOF
        chmod 755 "$INIT_HOOK"
        echo "AUTOSTART_ENABLED"
        ;;
    disable-autostart)
        rm -f "$INIT_HOOK"
        echo "AUTOSTART_DISABLED"
        ;;
    uninstall-prep)
        stop_daemon
        rm -f "$INIT_HOOK"
        cleanup_ls2_role
        echo "CLEANED_UP"
        ;;
    status) status_daemon ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|enable-autostart|disable-autostart|uninstall-prep}"
        exit 1
        ;;
esac
