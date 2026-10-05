#!/bin/sh
VM=$(dirname "$(readlink -f "$0")")
$VM/mon.py screendump $VM/screen.ppm >/dev/null
python3 -c "from PIL import Image; Image.open('$VM/screen.ppm').save('$VM/screen.png')"
echo $VM/screen.png
