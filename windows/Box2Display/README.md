# BOX-2 Wi-Fi Display

This project turns the BOX-2 LCD into a Windows 11 extended display. The
indirect display driver exposes one landscape `480 x 360 @ 30 Hz` monitor. The
elevated host process captures that monitor, scales it to the LCD's native
`320 x 240` resolution using high-quality downscaling, converts BGRA8 to
RGB565 little-endian, and streams it over Wi-Fi. Only landscape mode is
exposed. The host also draws an enlarged native cursor into the transmitted
image.

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

Protocol version 2 starts with a full `320 * 240 * RGB565LE` key frame. Later
updates contain only the changed bounding rectangle. An unchanged desktop sends
no pixel payload, while ordinary cursor movement usually refreshes only a small
region. Large animation or scrolling automatically falls back to a full-screen
rectangle. Each rectangle uses lossless RGB565 run-length encoding when that is
smaller than its raw pixels, and automatically falls back to raw RGB565 for
photos or video.

The host captures at up to 30 FPS. The ESP32 uses three PSRAM update buffers and
applies every rectangle in order, preventing partial updates from being lost.
This reduces typical bandwidth and LCD writes by an order of magnitude compared
with continuously sending 153600-byte full frames.
