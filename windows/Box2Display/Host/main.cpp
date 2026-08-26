#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <swdevice.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
constexpr wchar_t kStopEventName[] = L"Local\\Box2DisplayHostStop";
constexpr wchar_t kMutexName[] = L"Local\\Box2DisplayHostSingleton";
constexpr unsigned short kDiscoveryPort = 5001;
constexpr DWORD kSourceWidth = 480;
constexpr DWORD kSourceHeight = 360;
constexpr unsigned int kLcdWidth = 320;
constexpr unsigned int kLcdHeight = 240;
constexpr float kJpegQuality = 0.80f;
bool g_Verbose = false;

bool EnsureLandscapeMode()
{
    DISPLAY_DEVICE display = {};
    display.cb = sizeof(display);
    for (DWORD index = 0; EnumDisplayDevices(nullptr, index, &display, 0);
         ++index)
    {
        if (wcsstr(display.DeviceString, L"BOX-2 Wi-Fi Display Adapter") ==
            nullptr)
        {
            display = {};
            display.cb = sizeof(display);
            continue;
        }

        DEVMODE mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsEx(display.DeviceName, ENUM_CURRENT_SETTINGS,
                                   &mode, 0))
        {
            return false;
        }
        if (mode.dmDisplayOrientation == DMDO_DEFAULT &&
            mode.dmPelsWidth == kSourceWidth &&
            mode.dmPelsHeight == kSourceHeight)
        {
            return true;
        }

        mode.dmFields |= DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
        mode.dmDisplayOrientation = DMDO_DEFAULT;
        mode.dmPelsWidth = kSourceWidth;
        mode.dmPelsHeight = kSourceHeight;
        LONG result = ChangeDisplaySettingsEx(display.DeviceName, &mode,
                                              nullptr, CDS_UPDATEREGISTRY,
                                              nullptr);
        if (g_Verbose)
        {
            fwprintf(stderr, L"Landscape mode result for %ls: %ld\n",
                     display.DeviceName, result);
        }
        return result == DISP_CHANGE_SUCCESSFUL;
    }
    return false;
}

#pragma pack(push, 1)
struct DiscoveryRequest
{
    char Magic[4];
    uint16_t Version;
    uint16_t Reserved;
};

struct DiscoveryReply
{
    char Magic[4];
    uint16_t Version;
    uint16_t Width;
    uint16_t Height;
    uint16_t StreamPort;
    uint32_t Reserved;
};

struct StreamHello
{
    char Magic[4];
    uint16_t Version;
    uint16_t Width;
    uint16_t Height;
    uint16_t PixelFormat;
};

struct StreamAck
{
    char Magic[4];
    uint16_t Version;
    uint16_t Status;
};

struct JpegFrameHeader
{
    char Magic[4];
    uint32_t PayloadBytes;
    uint32_t Sequence;
    uint16_t Width;
    uint16_t Height;
};
#pragma pack(pop)

class DisplayStreamer
{
public:
    DisplayStreamer()
    {
        WSADATA data = {};
        m_WinsockReady = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ComInitialized = result == S_OK || result == S_FALSE;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_WicFactory));
        if (FAILED(result))
        {
            m_WicFactory = nullptr;
        }
    }

    ~DisplayStreamer()
    {
        CloseSocket();
        if (m_WicFactory)
        {
            m_WicFactory->Release();
        }
        if (m_ComInitialized)
        {
            CoUninitialize();
        }
        if (m_WinsockReady)
        {
            WSACleanup();
        }
    }

    bool CaptureAndSend()
    {
        DEVMODE mode = {};
        mode.dmSize = sizeof(mode);
        if (!FindDisplay(mode))
        {
            if (g_Verbose) fwprintf(stderr, L"No 480x360 monitor found.\n");
            return false;
        }

        std::vector<uint8_t> bgr(kLcdWidth * kLcdHeight * 3);
        HDC desktop = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(desktop);
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(kLcdWidth);
        info.bmiHeader.biHeight = -static_cast<LONG>(kLcdHeight);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS,
                                          &pixels, nullptr, 0);
        if (!desktop || !memory || !bitmap || !pixels)
        {
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (desktop) ReleaseDC(nullptr, desktop);
            return false;
        }

        HGDIOBJ previous = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        BOOL captured = StretchBlt(memory, 0, 0, kLcdWidth, kLcdHeight,
                                   desktop, mode.dmPosition.x, mode.dmPosition.y,
                                   mode.dmPelsWidth, mode.dmPelsHeight,
                                   SRCCOPY | CAPTUREBLT);
        if (captured)
        {
            DrawCursor(memory, mode);
            memcpy(bgr.data(), pixels, bgr.size());
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, desktop);
        if (!captured)
        {
            return false;
        }

        std::vector<uint8_t> jpeg;
        if (!EncodeJpeg(bgr, jpeg))
        {
            return false;
        }

        if (m_Socket == INVALID_SOCKET && !DiscoverAndConnect())
        {
            return false;
        }

        JpegFrameHeader header = {
            {'B', '2', 'J', '3'},
            htonl(static_cast<uint32_t>(jpeg.size())),
            htonl(m_Sequence++),
            htons(static_cast<uint16_t>(kLcdWidth)),
            htons(static_cast<uint16_t>(kLcdHeight)),
        };
        if (!SendAll(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ||
            !SendAll(jpeg.data(), jpeg.size()))
        {
            CloseSocket();
            return false;
        }
        return true;
    }

private:
    bool EncodeJpeg(const std::vector<uint8_t>& Pixels,
                    std::vector<uint8_t>& Encoded) const
    {
        if (!m_WicFactory)
        {
            return false;
        }

        IStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        HRESULT result = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(result))
        {
            result = m_WicFactory->CreateEncoder(GUID_ContainerFormatJpeg,
                                                 nullptr, &encoder);
        }
        if (SUCCEEDED(result))
        {
            result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        }
        if (SUCCEEDED(result))
        {
            result = encoder->CreateNewFrame(&frame, &properties);
        }
        if (SUCCEEDED(result) && properties)
        {
            PROPBAG2 option = {};
            option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = kJpegQuality;
            result = properties->Write(1, &option, &value);
            VariantClear(&value);
        }
        if (SUCCEEDED(result)) result = frame->Initialize(properties);
        if (SUCCEEDED(result)) result = frame->SetSize(kLcdWidth, kLcdHeight);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
        if (SUCCEEDED(result) && format != GUID_WICPixelFormat24bppBGR)
        {
            result = E_FAIL;
        }
        if (SUCCEEDED(result))
        {
            result = frame->WritePixels(
                kLcdHeight, kLcdWidth * 3,
                static_cast<UINT>(Pixels.size()),
                const_cast<BYTE*>(Pixels.data()));
        }
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();

        HGLOBAL memory = nullptr;
        STATSTG statistics = {};
        if (SUCCEEDED(result)) result = GetHGlobalFromStream(stream, &memory);
        if (SUCCEEDED(result)) result = stream->Stat(&statistics, STATFLAG_NONAME);
        if (SUCCEEDED(result) && statistics.cbSize.HighPart == 0)
        {
            const size_t size = statistics.cbSize.LowPart;
            const void* data = GlobalLock(memory);
            if (data && size > 0 && size <= UINT32_MAX)
            {
                const auto first = static_cast<const uint8_t*>(data);
                Encoded.assign(first, first + size);
            }
            else
            {
                result = E_FAIL;
            }
            if (data) GlobalUnlock(memory);
        }
        else if (SUCCEEDED(result))
        {
            result = E_FAIL;
        }

        if (properties) properties->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        return SUCCEEDED(result) && !Encoded.empty();
    }

    static void DrawCursor(HDC Target, const DEVMODE& Mode)
    {
        CURSORINFO cursor = {};
        cursor.cbSize = sizeof(cursor);
        if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
        {
            return;
        }

        LONG relativeX = cursor.ptScreenPos.x - Mode.dmPosition.x;
        LONG relativeY = cursor.ptScreenPos.y - Mode.dmPosition.y;
        if (relativeX < 0 || relativeY < 0 ||
            relativeX >= static_cast<LONG>(Mode.dmPelsWidth) ||
            relativeY >= static_cast<LONG>(Mode.dmPelsHeight))
        {
            return;
        }

        int pointerX = MulDiv(relativeX, kLcdWidth, Mode.dmPelsWidth);
        int pointerY = MulDiv(relativeY, kLcdHeight, Mode.dmPelsHeight);

        // Draw only the enlarged native cursor; the additional hot-spot ring
        // obscures small controls on the 320x240 LCD.
        constexpr int cursorSize = 28;
        int drawX = pointerX;
        int drawY = pointerY;
        ICONINFO icon = {};
        if (GetIconInfo(cursor.hCursor, &icon))
        {
            int systemWidth = GetSystemMetrics(SM_CXCURSOR);
            int systemHeight = GetSystemMetrics(SM_CYCURSOR);
            drawX -= MulDiv(static_cast<int>(icon.xHotspot), cursorSize,
                            systemWidth ? systemWidth : cursorSize);
            drawY -= MulDiv(static_cast<int>(icon.yHotspot), cursorSize,
                            systemHeight ? systemHeight : cursorSize);
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
        }
        DrawIconEx(Target, drawX, drawY, cursor.hCursor, cursorSize, cursorSize,
                   0, nullptr, DI_NORMAL);
    }

    bool FindDisplay(DEVMODE& Mode)
    {
        struct MonitorSearch
        {
            DEVMODE* Mode;
            bool Found;
        } search = {&Mode, false};

        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL
            {
                auto searchContext = reinterpret_cast<MonitorSearch*>(context);
                MONITORINFOEX info = {};
                info.cbSize = sizeof(info);
                if (!GetMonitorInfo(monitor, &info))
                {
                    return TRUE;
                }
                LONG width = info.rcMonitor.right - info.rcMonitor.left;
                LONG height = info.rcMonitor.bottom - info.rcMonitor.top;
                if (g_Verbose)
                {
                    fwprintf(stderr, L"Monitor %ls: %ldx%ld at %ld,%ld\n",
                             info.szDevice, width, height,
                             info.rcMonitor.left, info.rcMonitor.top);
                }
                if (width != static_cast<LONG>(kSourceWidth) ||
                    height != static_cast<LONG>(kSourceHeight))
                {
                    return TRUE;
                }
                searchContext->Mode->dmPosition.x = info.rcMonitor.left;
                searchContext->Mode->dmPosition.y = info.rcMonitor.top;
                searchContext->Mode->dmPelsWidth = static_cast<DWORD>(width);
                searchContext->Mode->dmPelsHeight = static_cast<DWORD>(height);
                searchContext->Found = true;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&search));
        return search.Found;
    }

    bool DiscoverAndConnect()
    {
        if (!m_WinsockReady)
        {
            return false;
        }
        SOCKET discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery == INVALID_SOCKET)
        {
            return false;
        }
        BOOL broadcastEnabled = TRUE;
        DWORD timeout = 800;
        setsockopt(discovery, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&broadcastEnabled),
                   sizeof(broadcastEnabled));
        setsockopt(discovery, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        DiscoveryRequest request = {{'B', '2', 'D', 'Q'}, htons(1), 0};
        sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kDiscoveryPort);
        destination.sin_addr.s_addr = INADDR_BROADCAST;
        int result = sendto(discovery, reinterpret_cast<const char*>(&request),
                            sizeof(request), 0,
                            reinterpret_cast<sockaddr*>(&destination),
                            sizeof(destination));
        if (result != sizeof(request))
        {
            if (g_Verbose) fwprintf(stderr, L"Discovery send failed: %d\n", WSAGetLastError());
            closesocket(discovery);
            return false;
        }

        DiscoveryReply reply = {};
        sockaddr_in board = {};
        int boardLength = sizeof(board);
        result = recvfrom(discovery, reinterpret_cast<char*>(&reply),
                          sizeof(reply), 0,
                          reinterpret_cast<sockaddr*>(&board), &boardLength);
        int receiveError = result == SOCKET_ERROR ? WSAGetLastError() : 0;
        closesocket(discovery);
        if (result != sizeof(reply) || memcmp(reply.Magic, "B2DR", 4) != 0 ||
            ntohs(reply.Version) != 1 || ntohs(reply.Width) != kLcdWidth ||
            ntohs(reply.Height) != kLcdHeight)
        {
            if (g_Verbose) fwprintf(stderr, L"Discovery reply failed: result=%d error=%d; scanning LAN.\n", result, receiveError);
            return ConnectOnLocalSubnets();
        }

        board.sin_port = reply.StreamPort;
        return ConnectToAddress(board, false);
    }

    bool ConnectToAddress(sockaddr_in Board, bool NonBlocking,
                          uint32_t LocalAddress = 0)
    {
        SOCKET stream = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (stream == INVALID_SOCKET)
        {
            return false;
        }
        if (LocalAddress != 0)
        {
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(LocalAddress);
            if (bind(stream, reinterpret_cast<sockaddr*>(&local),
                     sizeof(local)) == SOCKET_ERROR)
            {
                closesocket(stream);
                return false;
            }
        }
        if (NonBlocking)
        {
            u_long enabled = 1;
            ioctlsocket(stream, FIONBIO, &enabled);
        }

        int connectResult = connect(stream, reinterpret_cast<sockaddr*>(&Board),
                                    sizeof(Board));
        if (connectResult == SOCKET_ERROR && NonBlocking &&
            WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(stream, &writable);
            FD_SET(stream, &failed);
            timeval timeout = {0, 8000};
            int selected = select(0, nullptr, &writable, &failed, &timeout);
            int socketError = 1;
            int socketErrorLength = sizeof(socketError);
            if (selected > 0 && FD_ISSET(stream, &writable))
            {
                getsockopt(stream, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socketError),
                           &socketErrorLength);
            }
            connectResult = socketError == 0 ? 0 : SOCKET_ERROR;
        }
        if (connectResult == SOCKET_ERROR)
        {
            closesocket(stream);
            return false;
        }
        if (NonBlocking)
        {
            u_long disabled = 0;
            ioctlsocket(stream, FIONBIO, &disabled);
        }
        BOOL noDelay = TRUE;
        DWORD sendTimeout = 1500;
        DWORD receiveTimeout = NonBlocking ? 120 : 600;
        setsockopt(stream, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        setsockopt(stream, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));

        StreamHello hello = {{'B', '2', 'D', 'S'}, htons(3),
                             htons(static_cast<uint16_t>(kLcdWidth)),
                             htons(static_cast<uint16_t>(kLcdHeight)), htons(2)};
        size_t helloSent = 0;
        while (helloSent < sizeof(hello))
        {
            int result = send(stream,
                              reinterpret_cast<const char*>(&hello) + helloSent,
                              static_cast<int>(sizeof(hello) - helloSent), 0);
            if (result <= 0)
            {
                closesocket(stream);
                return false;
            }
            helloSent += static_cast<size_t>(result);
        }
        StreamAck ack = {};
        size_t ackReceived = 0;
        while (ackReceived < sizeof(ack))
        {
            int result = recv(stream, reinterpret_cast<char*>(&ack) + ackReceived,
                              static_cast<int>(sizeof(ack) - ackReceived), 0);
            if (result <= 0)
            {
                closesocket(stream);
                return false;
            }
            ackReceived += static_cast<size_t>(result);
        }
        if (memcmp(ack.Magic, "B2DA", 4) != 0 || ntohs(ack.Version) != 3 ||
            ntohs(ack.Status) != 0)
        {
            closesocket(stream);
            return false;
        }
        m_Socket = stream;
        if (g_Verbose) fwprintf(stderr, L"Connected to BOX-2 MJPEG stream.\n");
        return true;
    }

    bool ConnectOnLocalSubnets()
    {
        ULONG bufferSize = 16 * 1024;
        std::vector<uint8_t> adapterBuffer(bufferSize);
        auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(
            adapterBuffer.data());
        ULONG result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                         GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufferSize);
        if (result == ERROR_BUFFER_OVERFLOW)
        {
            adapterBuffer.resize(bufferSize);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(
                adapterBuffer.data());
            result = GetAdaptersAddresses(
                AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, adapters, &bufferSize);
        }
        if (result != NO_ERROR)
        {
            return false;
        }

        // Wi-Fi is the normal BOX-2 path, so probe it before wired adapters.
        // Tunnel and software-display adapters are deliberately excluded: on
        // some PCs they proxy every address and turn a quick /24 scan into a
        // multi-minute sequence of false TCP connections.
        std::vector<uint32_t> localAddresses;
        const ULONG interfaceTypes[] = {IF_TYPE_IEEE80211,
                                        IF_TYPE_ETHERNET_CSMACD};
        for (ULONG type : interfaceTypes)
        {
            for (auto adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp ||
                    adapter->IfType != type)
                {
                    continue;
                }
                for (auto address = adapter->FirstUnicastAddress; address;
                     address = address->Next)
                {
                    if (!address->Address.lpSockaddr ||
                        address->Address.lpSockaddr->sa_family != AF_INET)
                    {
                        continue;
                    }
                    auto ipv4 = reinterpret_cast<sockaddr_in*>(
                        address->Address.lpSockaddr);
                    uint32_t localAddress = ntohl(ipv4->sin_addr.s_addr);
                    uint8_t first = static_cast<uint8_t>(localAddress >> 24);
                    uint8_t second = static_cast<uint8_t>(localAddress >> 16);
                    if (first == 127 || (first == 169 && second == 254) ||
                        std::find(localAddresses.begin(), localAddresses.end(),
                                  localAddress) != localAddresses.end())
                    {
                        continue;
                    }
                    localAddresses.push_back(localAddress);
                }
            }
        }

        bool connected = false;
        for (uint32_t localAddress : localAddresses)
        {
            uint32_t network = localAddress & 0xFFFFFF00u;
            uint32_t localHost = localAddress & 0xFFu;
            for (uint32_t offset = 1; offset < 255 && !connected; ++offset)
            {
                uint32_t host = ((localHost - 1 + offset) % 254) + 1;
                uint32_t candidate = network | host;
                sockaddr_in board = {};
                board.sin_family = AF_INET;
                board.sin_port = htons(5000);
                board.sin_addr.s_addr = htonl(candidate);
                connected = ConnectToAddress(board, true, localAddress);
            }
            if (connected)
            {
                break;
            }
        }
        return connected;
    }

    bool SendAll(const uint8_t* Data, size_t Size)
    {
        size_t sent = 0;
        while (sent < Size)
        {
            int result = send(m_Socket,
                              reinterpret_cast<const char*>(Data + sent),
                              static_cast<int>(Size - sent), 0);
            if (result <= 0)
            {
                return false;
            }
            sent += static_cast<size_t>(result);
        }
        return true;
    }

    void CloseSocket()
    {
        if (m_Socket != INVALID_SOCKET)
        {
            shutdown(m_Socket, SD_BOTH);
            closesocket(m_Socket);
            m_Socket = INVALID_SOCKET;
        }
    }

    bool m_WinsockReady = false;
    bool m_ComInitialized = false;
    IWICImagingFactory* m_WicFactory = nullptr;
    SOCKET m_Socket = INVALID_SOCKET;
    uint32_t m_Sequence = 0;
};

struct CreationContext
{
    HANDLE Event;
    HRESULT Result;
};

void WINAPI CreationCallback(HSWDEVICE Device, HRESULT Result, PVOID Context,
                             PCWSTR InstanceId)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(InstanceId);
    auto creation = static_cast<CreationContext*>(Context);
    creation->Result = Result;
    SetEvent(creation->Event);
}

HANDLE g_StopEvent = nullptr;

BOOL WINAPI ConsoleHandler(DWORD ControlType)
{
    if (ControlType == CTRL_C_EVENT || ControlType == CTRL_BREAK_EVENT ||
        ControlType == CTRL_CLOSE_EVENT || ControlType == CTRL_SHUTDOWN_EVENT)
    {
        if (g_StopEvent)
        {
            SetEvent(g_StopEvent);
        }
        return TRUE;
    }
    return FALSE;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && _wcsicmp(argv[1], L"--stop") == 0)
    {
        HANDLE stop = OpenEvent(EVENT_MODIFY_STATE, FALSE, kStopEventName);
        if (!stop)
        {
            return 0;
        }
        SetEvent(stop);
        CloseHandle(stop);
        return 0;
    }
    g_Verbose = argc > 1 && _wcsicmp(argv[1], L"--verbose") == 0;

    // Monitor coordinates must stay in physical pixels even when the primary
    // display uses Windows scaling. Otherwise a 480x360 target can be exposed
    // to this process as a smaller, DPI-virtualized rectangle.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE singleton = CreateMutex(nullptr, TRUE, kMutexName);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (singleton)
        {
            CloseHandle(singleton);
        }
        return 0;
    }

    g_StopEvent = CreateEvent(nullptr, TRUE, FALSE, kStopEventName);
    HANDLE creationEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_StopEvent || !creationEvent)
    {
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    static const wchar_t hardwareIds[] = L"Box2Display\0";
    SW_DEVICE_CREATE_INFO createInfo = {};
    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = hardwareIds;
    createInfo.pszInstanceId = L"Box2Display";
    createInfo.pszzHardwareIds = hardwareIds;
    createInfo.pszDeviceDescription = L"BOX-2 Wi-Fi Display Adapter";
    createInfo.CapabilityFlags = SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    CreationContext context = {creationEvent, E_PENDING};
    HSWDEVICE softwareDevice = nullptr;
    HRESULT result = SwDeviceCreate(L"Box2Display", L"HTREE\\ROOT\\0",
                                    &createInfo, 0, nullptr, CreationCallback,
                                    &context, &softwareDevice);
    if (FAILED(result))
    {
        fwprintf(stderr, L"SwDeviceCreate failed: 0x%08lx\n", result);
        return 2;
    }

    if (WaitForSingleObject(creationEvent, 15000) != WAIT_OBJECT_0 ||
        FAILED(context.Result))
    {
        fwprintf(stderr, L"BOX-2 display creation failed: 0x%08lx\n",
                 context.Result);
        SwDeviceClose(softwareDevice);
        return 3;
    }

    wprintf(L"BOX-2 virtual display is active. Press Ctrl+C to stop.\n");
    for (int attempt = 0; attempt < 20 && !EnsureLandscapeMode(); ++attempt)
    {
        Sleep(250);
    }
    DisplayStreamer streamer;
    while (true)
    {
        ULONGLONG start = GetTickCount64();
        bool sent = streamer.CaptureAndSend();
        ULONGLONG elapsed = GetTickCount64() - start;
        DWORD delay = sent ? static_cast<DWORD>(elapsed < 33 ? 33 - elapsed : 1)
                           : 500;
        if (WaitForSingleObject(g_StopEvent, delay) == WAIT_OBJECT_0)
        {
            break;
        }
    }

    SwDeviceClose(softwareDevice);
    CloseHandle(creationEvent);
    CloseHandle(g_StopEvent);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
