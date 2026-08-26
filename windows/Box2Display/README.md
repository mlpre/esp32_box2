# BOX-2 Wi-Fi Display

This project turns the BOX-2 LCD into a Windows 11 extended display. The
indirect display driver exposes one landscape `640 x 480 @ 30 Hz` monitor. The
elevated host process captures that monitor, scales it to the LCD's native
`320 x 240` resolution, converts BGRA8 to RGB565 little-endian, and streams it
over Wi-Fi. Only landscape mode is exposed. The host also draws an enlarged
cursor and a high-contrast hot-spot ring into the transmitted image.

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

Each physical frame is exactly 153600 bytes (`320 * 240 * RGB565LE`). The ESP32
receiver uses three PSRAM frame buffers and a latest-frame queue, so congestion
drops stale images instead of accumulating latency. The target transmission
rate is 15 FPS; the Windows virtual mode remains 30 Hz for normal desktop
composition.

The current uncompressed stream requires about 18.4 Mbit/s at 15 FPS, excluding
TCP/IP overhead. Future versions can add dirty rectangles or lightweight image
compression without changing the Windows virtual-monitor interface.
