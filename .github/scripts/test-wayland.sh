#!/bin/sh
# Runs the examples' tests on Wayland, in a headless sway, as test.sh runs them on SDL's offscreen driver.
# There, popups and pop-out windows are windows of their own, which the offscreen driver does not have.
# Sway floats every window, so each keeps the size it asks for, and the tests' positions hold.
set -u

executable="$1"
runtime=$(mktemp -d)
chmod 700 "$runtime"
printf 'output HEADLESS-1 resolution 1920x1080\nfor_window [app_id=".*"] floating enable\n' > "$runtime/sway.conf"

mkdir -p build
XDG_RUNTIME_DIR="$runtime" WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$runtime/sway.conf" > build/sway.log 2>&1 &
sway=$!
trap 'kill "$sway" 2> /dev/null; wait "$sway" 2> /dev/null; rm -rf "$runtime"' EXIT

# sway makes its socket once it is ready, up to ten seconds later
display=
for _ in $(seq 100); do
    display=$(find "$runtime" -maxdepth 1 -type s -name 'wayland-*' -printf '%f\n' | head -n 1)
    [ -n "$display" ] && break
    kill -0 "$sway" 2> /dev/null || break
    sleep 0.1
done

if [ -z "$display" ]; then
    cat build/sway.log
    echo "Sway did not start; build/sway.log has its output."
    exit 1
fi

XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$display" SDL_VIDEO_DRIVER=wayland "$(dirname "$0")/test.sh" "$executable"
