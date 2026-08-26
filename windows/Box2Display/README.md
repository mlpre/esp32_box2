# BOX-2 Wi-Fi Display

This project turns the BOX-2 LCD into a Windows 11 extended display. The
indirect display driver exposes one landscape `480 x 360 @ 30 Hz` monitor. The
elevated host process captures that monitor, scales it to the LCD's native
`320 x 240` resolution using high-quality downscaling, encodes every frame as
JPEG quality 80, and streams the MJPEG frames over Wi-Fi. Only landscape mode
is exposed. The host also draws an enlarged native cursor into the transmitted
image before encoding.

USB serial is used only for firmware flashing and diagnostics. Display pixels
travel over UDP port 5000. The host first broadcasts a discovery request on UDP
port 5001 and falls back to directed broadcasts on the PC's local `/24`
networks. A versioned `B2DS`/`B2DA` datagram handshake prevents sending video
to an unrelated UDP service.

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

Protocol version 4 splits each baseline JPEG image into UDP datagrams on port
5000. Each datagram stays below the normal Ethernet MTU: a 24-byte header and
at most 1400 JPEG bytes. Its header carries the frame size and sequence plus the
fragment index, count, offset, and payload length. UDP port 5001 remains
responsible for discovery.

The host captures and encodes at up to 30 FPS. The ESP32 decodes each JPEG
directly to RGB565 little-endian in an aligned PSRAM buffer, keeps only the
latest completed frame waiting for the LCD, and drops stale completed frames
instead of accumulating display latency. The Windows host reuses its GDI
capture surface and JPEG memory stream, and runs UDP transmission on a separate
thread backed by a single latest-frame slot. The ESP32 decodes only fully
reassembled JPEG frames. If any fragment is missing, that frame is discarded as
soon as a newer sequence arrives, so packet loss produces a skipped frame rather
than partial-image artifacts or retransmission latency. A one-second session
keepalive lets streaming recover automatically after an ESP32 reboot or a brief
Wi-Fi interruption, without waiting for a UDP disconnect event that does not
exist.

## Power button

Hold the middle **M** key for 1.5 seconds. On battery power this turns off the
LCD and releases the board's `SYS_POW` latch for a real hardware shutdown. Hold
the same key to power the board on again. USB supplies the board independently,
so while USB is attached the same long press enters display standby instead;
press **M** once to wake the display and resume decoding the newest frame.
