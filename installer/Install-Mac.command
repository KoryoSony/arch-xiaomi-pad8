#!/usr/bin/env bash
# Arch Linux for Xiaomi Pad 8 - installer for macOS and Linux.
# Put this file in the same folder as the downloaded .img files and run it.
set -euo pipefail
cd "$(dirname "$0")"

say()  { printf '\n\033[1;36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31mProblem: %s\033[0m\n' "$*"; read -r -p "Press Enter to close." _; exit 1; }

say "Arch Linux for Xiaomi Pad 8 - installer"

# 1. fastboot: use the one on this computer, or download Google's official one.
if command -v fastboot >/dev/null 2>&1; then
    FB=fastboot
else
    case "$(uname -s)" in
        Darwin) os=darwin ;;
        Linux)  os=linux ;;
        *) fail "this script works on macOS and Linux (on Windows use Install-Windows.bat)" ;;
    esac
    if [ ! -x platform-tools/fastboot ]; then
        say "Downloading Google's fastboot tool..."
        curl -fL -o platform-tools.zip "https://dl.google.com/android/repository/platform-tools-latest-$os.zip" \
            || fail "could not download fastboot. Is the internet connected?"
        unzip -q -o platform-tools.zip && rm platform-tools.zip
    fi
    FB=./platform-tools/fastboot
fi

# 2. The files.
for f in boot.img init_boot.img; do [ -f "$f" ] || fail "$f is missing. Put all the downloaded files in this folder."; done
parts=$(ls userdata.part*.img 2>/dev/null | sort -V)
[ -n "$parts" ] || fail "userdata.part1.img (and the other parts) are missing. Put all the downloaded files in this folder."
if [ -f SHA256SUMS ]; then
    say "Checking the files are not broken..."
    if command -v sha256sum >/dev/null 2>&1; then sha256sum -c --quiet SHA256SUMS || fail "a file is broken, download it again"
    else shasum -a 256 -c --quiet SHA256SUMS || fail "a file is broken, download it again"; fi
fi

# 3. The tablet.
say "Now put the tablet in FASTBOOT mode:"
echo "  1. Turn the tablet OFF."
echo "  2. Hold POWER + VOLUME DOWN together until you see the bunny (FASTBOOT)."
echo "  3. Connect it to this computer with a USB-C cable."
echo
echo "Waiting for the tablet..."
until "$FB" devices 2>/dev/null | grep -q fastboot; do sleep 1; done
product=$("$FB" getvar product 2>&1 | sed -n 's/^product: *//p' | head -1)
unlocked=$("$FB" getvar unlocked 2>&1 | sed -n 's/^unlocked: *//p' | head -1)
[ "$product" = yupei ] || fail "this is not a Xiaomi Pad 8 (it says '$product'). Nothing was changed."
[ "$unlocked" = yes ] || fail "the bootloader is locked. Unlock it first with Mi Unlock. Nothing was changed."

say "Found your Xiaomi Pad 8."
echo "This will ERASE EVERYTHING on the tablet (Android, apps, photos) and install Arch Linux."
read -r -p "Type YES and press Enter to continue: " ok
[ "$ok" = YES ] || fail "you did not type YES. Nothing was changed."

say "Installing... (do not unplug the tablet, this takes a few minutes)"
"$FB" flash boot boot.img
"$FB" flash init_boot init_boot.img
n=0; total=$(echo "$parts" | wc -l | tr -d ' ')
for p in $parts; do
    n=$((n + 1)); say "Installing the system, part $n of $total..."
    "$FB" flash userdata "$p"
done
"$FB" reboot

say "Done! The tablet is starting Arch Linux."
echo "The first start takes about a minute. Then it asks for your language,"
echo "Wi-Fi and a name and password for you. Have fun!"
read -r -p "Press Enter to close." _
