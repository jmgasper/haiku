# USB cameras

USB Video Class cameras are a video input of the media kit: CodyCam shows
them, `BMediaRoster::GetVideoInput()` returns them. Tested with a Logitech
HD Pro Webcam C920 (046d:082d) on the X399, 2026-10-05.

## What it took

| Where | What was wrong |
| --- | --- |
| `usb_webcam.media_addon` | The UVC part was not built (and did not work). Rewritten: `addons/uvc/UVCCamDevice.cpp`. |
| `xhci` | High-bandwidth isochronous endpoints (more than one transaction per microframe, which is what cameras use): the extra-transaction bits were left in the packet size, a packet could not exceed one transaction, and the TDs did not say how many packets they hold. |
| `usb_raw`, USB Kit | One isochronous transfer at a time loses what the camera sends in between. New: `BUSBEndpoint::StartIsochronousStream()`, `ReadIsochronousStream()`, `StopIsochronousStream()`; the driver keeps transfers queued. |
| `media_addon_server` | Nodes of physical inputs were only created when an add-on was loaded: a camera found a moment later, or plugged in later, never got one. |
| `media_server` | With an audio default saved and no video default, no video input was ever chosen (the saved default of the wrong type was compared against). |

## What an application gets

* One node per camera, named as the camera names itself, with one
  `B_MEDIA_RAW_VIDEO` output, `B_RGB32`.
* The size is the consumer's choice: the size in the format passed to
  `Connect()` is what arrives. The camera is switched to that size, or to the
  next larger one it has, which is cropped to the proportions and scaled. No
  size asked for: 320x240.
* The camera's formats are used as they fit: uncompressed (YUY2, UYVY, NV12,
  I420) where it reaches 30 pictures per second, else Motion-JPEG (needs
  libjpeg at build time: the `jpeg` build feature).
* Pictures are sent as the camera delivers them. Incomplete ones are dropped.
* The controls (brightness, white balance, exposure, focus, zoom, ...) are
  the node's parameters: Media preferences shows them.
* A consumer that dies without disconnecting frees the camera within a
  second.

## Checking

`tests/camgrab.cpp` connects to the default video input, counts what arrives
and can save a picture; `tests/camnodes.cpp` lists the inputs and the
camera's controls, and sets one. `tools/check-workstation.sh` runs camgrab.

    camgrab 1920 1080 60
    connected: 1920x1080, 30 per second, "HD Pro Webcam C920"
    1792 frames in 60 s; first after 0.28 s, then 30.00 per second, longest gap 37 ms

Measured on the C920: 160x120, 320x240, 640x480 (YUY2), 500x300 (scaled),
1280x720 and 1920x1080 (Motion-JPEG) all at 30.0 per second; 2304x1536 at 2.

## Not tested

* Unplugging and plugging the camera while the system runs: nobody was at the
  machine. The code for it is there (the node is retired and a new one made).
* Cameras with a bulk endpoint, cameras on a full speed bus, cameras that
  only have NV12/I420/UYVY: no such hardware here.
* EHCI/OHCI/UHCI controllers: only the xhci driver was fixed.

The message `TRB ... was not found in the endpoint!` in the syslog when a
camera is stopped is the cancelled transfers' last event; harmless.
