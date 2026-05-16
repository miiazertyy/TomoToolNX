#!/bin/bash
# Helper used during the island-generator implementation to confirm builds.
# Safe to delete after the feature lands.
set -e
export DEVKITPRO="C:/devkitPro"
export DEVKITA64="C:/devkitPro/devkitA64"
export DEVKITARM=
export DEVKITPPC=
# Windows-native tools (gcc/ld/as) read TMP/TEMP from the Windows env. Default
# user temp dir is writable; C:\WINDOWS is not, which is what they fall back to
# when these are unset.
export TMP="C:/Users/miiaz/AppData/Local/Temp"
export TEMP="C:/Users/miiaz/AppData/Local/Temp"
export TMPDIR="$TMP"
export PATH="/c/devkitPro/devkitA64/bin:/c/devkitPro/tools/bin:$PATH"
echo "[.build.sh] DEVKITPRO=$DEVKITPRO"
echo "[.build.sh] PATH head: ${PATH:0:200}..."
exec make DEVKITPRO="$DEVKITPRO" DEVKITA64="$DEVKITA64" "$@"
