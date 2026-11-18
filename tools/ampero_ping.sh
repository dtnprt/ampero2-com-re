#!/bin/sh
# Does the Ampero still answer over USB MIDI? Sends CONNECT (0x05000017) and the firmware version request (0x03000005), like the editors, and prints the answer.
# Close the browser tab / Hotone editor first. Exit 0 = answers, 1 = no answer, 2 = sending blocked, 3 = no port, 4 = port busy.
port=$(amidi -l | awk '/Ampero/ {print $2; exit}')
[ -n "$port" ] || { echo "no Ampero MIDI port (amidi -l)"; exit 3; }
req='f0 21 25 4d 50 00 00 0d 11 08 00 00 00 01 07 00 00 00 00 00 05 00 00 00 00 00 00 00 00 f7 f0 21 25 4d 50 00 00 08 11 08 00 00 00 00 05 00 00 00 00 00 03 00 00 00 00 00 00 00 00 f7'
out=$(timeout 6 amidi -p "$port" -S "$req" -d -t 3 2>&1); rc=$?
echo "$out" | grep -q "busy" && { echo "$port: busy – another program (browser tab, Hotone editor) has the port open; close it first"; exit 4; }
[ $rc = 124 ] && { echo "$port: sending blocked – the pedal does not take MIDI data any more"; exit 2; }
echo "$out" | grep -qi "f0 21 25 4d" && { echo "$port: answers"; echo "$out" | head -3; exit 0; }
echo "$port: sent, but no answer"; echo "$out" | head -3; exit 1
