#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$root/dist/F133-Radio-x86_64.AppImage"}
case "$(uname -m)" in
    x86_64) ;;
    *) echo 'AppImage packaging currently supports x86_64 only.' >&2; exit 1 ;;
esac
command -v linuxdeploy >/dev/null || { echo 'linuxdeploy is required' >&2; exit 1; }
command -v appimagetool >/dev/null || { echo 'appimagetool is required' >&2; exit 1; }
mkdir -p "$(dirname -- "$output")"
output=$(realpath -m "$output")
stage=$(mktemp -d "${TMPDIR:-/tmp}/f133-appimage.XXXXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
appdir="$stage/AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/qemu"
cp "$root/qemu/build/qemu-system-riscv64" "$appdir/usr/bin/"
${CC:-cc} "$root/packaging/firmware-picker.c" -o "$appdir/usr/bin/f133-firmware-picker" \
    $(pkg-config --cflags --libs gtk+-3.0)
cp -R "$root/qemu/pc-bios/keymaps" "$appdir/usr/share/qemu/"
cp "$root/packaging/AppRun" "$appdir/AppRun"
chmod +x "$appdir/AppRun"
cat > "$appdir/usr/bin/f133-radio" <<'EOF'
#!/bin/sh
exec "${APPDIR:?}/AppRun" "$@"
EOF
chmod +x "$appdir/usr/bin/f133-radio"
# Both tools can themselves be AppImages; this also works in containers without FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
linuxdeploy --appdir "$appdir" \
    --executable "$appdir/usr/bin/qemu-system-riscv64" \
    --executable "$appdir/usr/bin/f133-firmware-picker" \
    --desktop-file "$root/packaging/f133-radio.desktop" \
    --icon-file "$root/packaging/f133-radio.svg"
ARCH=x86_64 appimagetool "$appdir" "$stage/F133-Radio-x86_64.AppImage"
mv "$stage/F133-Radio-x86_64.AppImage" "$output"
printf 'AppImage: %s\n' "$output"
