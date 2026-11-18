#!/bin/bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IPK_DIR="$ROOT_DIR/webos-ipk"
WORK_DIR="$IPK_DIR/.stage"
OUT_IPK="$IPK_DIR/webos_dtv_tunerbridge.ipk"

rm -rf "$WORK_DIR"
APP_STAGE="$WORK_DIR/data/usr/palm/applications/io.github.webostuner.app"
PKG_STAGE="$WORK_DIR/data/usr/palm/packages/io.github.webostuner.app"
mkdir -p "$APP_STAGE/native" "$PKG_STAGE" "$WORK_DIR/ctrl"

cp -r "$IPK_DIR/app/"* "$APP_STAGE/"
cp "$ROOT_DIR/tv-bridge/libwebos_tuner.so" \
   "$ROOT_DIR/tv-bridge/webos_inject" \
   "$ROOT_DIR/tv-bridge/webos_tuner_d" \
   "$APP_STAGE/native/"

sed -i 's/\r$//' "$APP_STAGE/native/tunerctl.sh"
chmod 755 "$APP_STAGE/native/tunerctl.sh" "$APP_STAGE/native/webos_inject" "$APP_STAGE/native/webos_tuner_d"
chmod 644 "$APP_STAGE/native/libwebos_tuner.so" "$APP_STAGE/appinfo.json" "$APP_STAGE/index.html"

cat <<'EOF' > "$PKG_STAGE/packageinfo.json"
{
  "id": "io.github.webostuner.app",
  "version": "1.1.0",
  "app": "io.github.webostuner.app"
}
EOF
chmod 644 "$PKG_STAGE/packageinfo.json"

SIZE=$(du -sb "$WORK_DIR/data" | awk '{print $1}')
cat <<EOF > "$WORK_DIR/ctrl/control"
Package: io.github.webostuner.app
Version: 1.1.0
Section: misc
Priority: optional
Architecture: all
Installed-Size: $SIZE
Maintainer: fnoji <https://github.com/fnoji>
Description: webOS TV Dual-Tuner Bridge (BonDriver_WebOS / Mirakurun API)
webOS-Package-Format-Version: 2
webOS-Packager-Version: 1.1.0
EOF

printf "2.0\n" > "$WORK_DIR/debian-binary"
(cd "$WORK_DIR/ctrl" && tar --format=gnu --owner=0 --group=0 -czf "$WORK_DIR/control.tar.gz" ./control)
(cd "$WORK_DIR/data" && tar --format=gnu --owner=0 --group=0 -czf "$WORK_DIR/data.tar.gz" ./usr)

# 末尾スラッシュ '/' のない純粋 Debian ar アーカイブを生成 (webOS opkg/appInstallService 互換)
WORK_DIR="$WORK_DIR" OUT_IPK="$OUT_IPK" python3 - <<'PY'
import os, time
work_dir = os.environ["WORK_DIR"]
out_ipk = os.environ["OUT_IPK"]
now = int(time.time())
parts = [
    ("debian-binary", os.path.join(work_dir, "debian-binary")),
    ("control.tar.gz", os.path.join(work_dir, "control.tar.gz")),
    ("data.tar.gz", os.path.join(work_dir, "data.tar.gz")),
]
with open(out_ipk, "wb") as out:
    out.write(b"!<arch>\n")
    for name, path in parts:
        data = open(path, "rb").read()
        hdr = (f"{name:<16}{now:<12}0     0     100644  {len(data):<10}" + chr(96) + "\n").encode("ascii")
        assert len(hdr) == 60
        out.write(hdr)
        out.write(data)
        if len(data) % 2 == 1:
            out.write(b"\n")
PY

rm -rf "$WORK_DIR"
ls -lh "$OUT_IPK"
head -c 140 "$OUT_IPK" | cat -v
echo ""
