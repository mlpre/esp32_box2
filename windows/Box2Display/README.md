# BOX-2 Wi-Fi Display

This project turns the BOX-2 LCD into a Windows 11 extended display. The
indirect display driver exposes one landscape `480 x 360 @ 30 Hz` monitor. The
elevated host process captures that monitor, scales it to the LCD's native
`320 x 240` resolution using high-quality downscaling, encodes every frame as
JPEG quality 80, and streams the MJPEG frames over Wi-Fi. Only landscape mode
is exposed. The host also draws an enlarged native cursor into the transmitted
image before encoding.

USB serial is used only for firmware flashing and diagnostics. Display pixels
travel over TCP port 5000. The host first broadcasts a discovery request on UDP
port 5001 and falls back to probing the PC's local `/24` networks. A versioned
`B2DS`/`B2DA` handshake prevents connecting to an unrelated TCP service.

## Build and install

Requirements:

- Windows 11 x64
- Visual Studio 2022 Build Tools with the C++ workload
- Windows Driver Kit 10.0.26100 or newer

Run from an ordinary PowerShell window:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build.ps1
.\install.ps1
```

`install.ps1` requests administrator permission, trusts the local development
certificate, installs the driver package, adds the firewall rule, and creates
the elevated `Box2DisplayHost` logon task. Select **Extend these displays** in
Windows Settings if Windows does not choose it automatically.

The local certificate is suitable for development on this PC. Public
distribution requires Microsoft driver signing.

To remove the display, driver, firewall rule, and logon task:

```powershell
.\uninstall.ps1
```

## Transport

Protocol version 3 carries one complete baseline JPEG image per TCP frame. UDP
port 5001 remains responsible for discovery, while the ordered MJPEG byte stream
uses TCP port 5000. Every JPEG header includes its encoded size, sequence number,
and fixed `320 x 240` dimensions.

The host captures and encodes at up to 30 FPS. The ESP32 decodes each JPEG
directly to RGB565 little-endian in an aligned PSRAM buffer, keeps only the
latest completed frame waiting for the LCD, and drops stale completed frames
instead of accumulating display latency. The Windows host reuses its GDI
capture surface and JPEG memory stream, and runs TCP transmission on a separate
thread backed by a single latest-frame slot. Network stalls can therefore
replace an old unsent frame without blocking capture or building a backlog.
