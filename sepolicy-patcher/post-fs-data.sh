#!/system/bin/sh
MODDIR="${0%/*}"
CAMDIR=/data/camera
LOGTAG='sepolicy-patcher'

logi() {
  log -t "$LOGTAG" -p i "$*" 2>/dev/null || echo "I:$LOGTAG: $*"
}

mkdir -p "$CAMDIR"
chmod 0755 "$CAMDIR" 2>/dev/null || true
chown root:root "$CAMDIR" 2>/dev/null || true

# Put directory on a stable data label.
chcon u:object_r:system_data_file:s0 "$CAMDIR" 2>/dev/null || true

# Any .so dropped here gets system_lib_file so cameraserver can dlopen/map it.
for so in "$CAMDIR"/*.so; do
  [ -e "$so" ] || continue
  chmod 0644 "$so" 2>/dev/null || true
  chown root:root "$so" 2>/dev/null || true
  chcon u:object_r:system_lib_file:s0 "$so" 2>/dev/null || true
  logi "relabeled $so -> system_lib_file"
done

logi 'post-fs-data done'
