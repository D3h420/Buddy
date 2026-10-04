#!/bin/sh
set -eu

# Run from the project root. Optional argument: installed Adafruit GFX path.
task_gfx=${1:-"$HOME/Documents/Arduino/libraries/Adafruit_GFX_Library"}
task_build=$(mktemp -d /tmp/buddy-preview.XXXXXX)
task_output=BuddyHardwareTest/previews
mkdir -p "$task_output"
clang++ -std=c++17 -DARDUINO=100 \
  -I BuddyHardwareTest/tools/host -I "$task_gfx" \
  BuddyHardwareTest/tools/host/preview.cpp "$task_gfx/Adafruit_GFX.cpp" \
  -o "$task_build/preview"
"$task_build/preview" "$task_build/screens"
for task_image in "$task_build"/screens/*.ppm; do
  task_name=$(basename "$task_image" .ppm)
  sips -s format png "$task_image" \
    --out "$task_output/$task_name.png" >/dev/null
done
printf 'Screens saved in %s\n' "$task_output"
