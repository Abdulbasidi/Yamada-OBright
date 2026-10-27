#!/bin/bash
# Works on desktop (NDK) and directly in Termux (pkg install clang)
API_LEVEL=30
TARGET="aarch64-linux-android${API_LEVEL}"
BIN_NAME="yamada.obright-V1@3.0-service"
RC_NAME="init.yamada.obright.rc"
OUT_DIR="out"
BIN_DIR="$OUT_DIR/vendor/bin/hw"
ETC_DIR="$OUT_DIR/vendor/etc/init"

if [ -n "$TERMUX_VERSION" ] || [[ "$PREFIX" == *com.termux* ]]; then
    CC="clang"; CFLAGS=""
else
    NDK_ROOT=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/28.2.13676358}
    CC="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
    CFLAGS="--target=$TARGET"
    [ -f "$CC" ] || { echo "Clang not found: $CC"; exit 1; }
fi

rm -rf "$OUT_DIR"; mkdir -p "$BIN_DIR" "$ETC_DIR"

# 16 KB page-size aligned (required for Android 15+/17 devices with 16K pages), PIE, hardened, stripped
$CC $CFLAGS -O2 -Wall -fPIE -pie -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
    -Wl,-z,relro -Wl,-z,now -Wl,--build-id=sha1 -Wl,--gc-sections \
    -o "$BIN_DIR/$BIN_NAME" Main.c -llog -lm || { echo "Build failed"; exit 1; }
STRIP="$(dirname "$CC")/llvm-strip"; [ -x "$STRIP" ] || STRIP=llvm-strip
"$STRIP" --strip-unneeded "$BIN_DIR/$BIN_NAME" 2>/dev/null || true

cat > "$ETC_DIR/$RC_NAME" <<RC
on boot
    chown system system /sys/class/leds/lcd-backlight/brightness
    chmod 0660 /sys/class/leds/lcd-backlight/brightness

service yamada_obright /vendor/bin/hw/$BIN_NAME
    class hal
    user system
    group system graphics
    capabilities SYS_NICE
    writepid /dev/cpuset/system-background/tasks
RC

echo "OK -> $OUT_DIR/vendor"
echo "Install: copy bin to /vendor/bin/hw (chmod 0755), rc to /vendor/etc/init (0644)"
echo "Label bin in vendor file_contexts + add sepolicy (or test with setenforce 0 first)"
