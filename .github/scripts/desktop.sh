#!/bin/sh
# Adds a preset's tool to the system's launcher with the Debug executable's --install, checks what it wrote, then removes it with --uninstall and checks
# that it is gone (DESIGN 13.9): on Linux the desktop entry, and on Windows the Start menu's shortcut, the icon file and "Open with" in the registry.
set -eu

executable="$1"
folder="$PWD/build/desktop"
rm -rf "$folder"
mkdir -p "$folder"
cp resources/sample.png "$folder/icon.png"
cat > "$folder/preset.lua" << 'PRESET'
return {
  format = 3,
  name = "desktop-check",
  version = "0.1.0",
  app = { id = "openecs.check.desktop", name = "Desktop Check", icon = "icon.png", categories = { "Utility" }, mimeTypes = { "image/png" } },
  depends = {},
  workspaces = { { name = "Main", windows = { { panels = {} } } } },
}
PRESET

fail() {
    echo "$1"
    cat build/desktop.log
    exit 1
}

"$executable" --preset "$folder/preset.lua" --install > build/desktop.log 2>&1 || fail "--install failed."

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    # Git Bash would turn reg's /v into a path
    export MSYS_NO_PATHCONV=1
    shortcut="$(cygpath "$APPDATA")/Microsoft/Windows/Start Menu/Programs/Desktop Check.lnk"
    icon="$(cygpath "$LOCALAPPDATA")/OpenECS/icons/openecs.check.desktop.ico"
    [ -f "$shortcut" ] || fail "--install wrote no shortcut."
    [ -f "$icon" ] || fail "--install wrote no icon file."
    reg query 'HKCU\Software\Classes\OpenECS.openecs.check.desktop\shell\open\command' > /dev/null || fail "--install registered no class."
    reg query 'HKCU\Software\Classes\.png\OpenWithProgids' /v OpenECS.openecs.check.desktop > /dev/null || fail "--install added nothing to \"Open with\" of .png."
    ;;
*)
    entry="${XDG_DATA_HOME:-$HOME/.local/share}/applications/openecs.check.desktop.desktop"
    [ -f "$entry" ] || fail "--install wrote no desktop entry."
    grep -q "^MimeType=image/png;$" "$entry" || fail "The desktop entry has no MimeType."
    ;;
esac

"$executable" --preset "$folder/preset.lua" --uninstall >> build/desktop.log 2>&1 || fail "--uninstall failed."

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    [ ! -e "$shortcut" ] || fail "--uninstall left the shortcut."
    [ ! -e "$icon" ] || fail "--uninstall left the icon file."
    ! reg query 'HKCU\Software\Classes\OpenECS.openecs.check.desktop' > /dev/null 2>&1 || fail "--uninstall left the class."
    ! reg query 'HKCU\Software\Classes\.png\OpenWithProgids' /v OpenECS.openecs.check.desktop > /dev/null 2>&1 || fail "--uninstall left \"Open with\" of .png."
    ;;
*)
    [ ! -e "$entry" ] || fail "--uninstall left the desktop entry."
    ;;
esac
