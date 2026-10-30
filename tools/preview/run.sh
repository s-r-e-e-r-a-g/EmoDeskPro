#!/bin/bash
# Build the host preview renderer and write PNGs of every screen to out/.
set -e
cd "$(dirname "$0")"
# Point these at your U8g2 C sources (src/clib) and ArduinoJson src folder.
U8=${U8G2_CLIB:-$HOME/Arduino/libraries/U8g2/src/clib}
AJ=${ARDUINOJSON:-$HOME/Arduino/libraries/ArduinoJson/src}
mkdir -p out build
for f in "$U8"/*.c; do
  o=build/$(basename "$f" .c).o
  [ -f "$o" ] || gcc -O1 -c -I"$U8" "$f" -o "$o"
done
gcc -O1 -c -I"$U8" -I../../EmoDesk ../../EmoDesk/qrcode.c -o build/qrcode.o
g++ -std=gnu++17 -O1 -w -Ishim -I"$U8" -I"$AJ" -I../../EmoDesk \
  preview.cpp ../../EmoDesk/settings.cpp ../../EmoDesk/gfx.cpp ../../EmoDesk/emo.cpp ../../EmoDesk/alerts.cpp \
  ../../EmoDesk/cards.cpp ../../EmoDesk/cards_time.cpp ../../EmoDesk/cards_data.cpp ../../EmoDesk/cards_sports.cpp \
  build/*.o -lm -o build/preview
rm -f out/*.pbm out/*.png
./build/preview
python3 pbm2png.py out
