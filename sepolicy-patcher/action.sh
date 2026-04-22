#!/system/bin/sh
CAMDIR=/data/camera
LOGTAG='sepolicy-patcher'
for so in "$CAMDIR"/*.so; do
  [ -e "$so" ] || continue
  chcon u:object_r:system_lib_file:s0 "$so" 2>/dev/null || true
  chmod 0644 "$so" 2>/dev/null || true
done
chcon u:object_r:system_data_file:s0 "$CAMDIR" 2>/dev/null || true
log -t "$LOGTAG" -p i 'manual relabel done' 2>/dev/null || true
