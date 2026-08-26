#define NOMINMAX
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <swdevice.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace
{
constexpr wchar_t kStopEventName[] = L"Local\\Box2DisplayHostStop";
constexpr wchar_t kMutexName[] = L"Local\\Box2DisplayHostSingleton";
constexpr unsigned short kDiscoveryPort = 5001;
constexpr DWORD kSourceWidth = 480;
constexpr DWORD kSourceHeight = 360;
constexpr unsigned int kLcdWidth = 320;
constexpr unsigned int kLcdHeight = 240;
constexpr size_t kCaptureBytes = kLcdWidth * kLcdHeight * 3;
constexpr size_t kMaxJpegBytes = kLcdWidth * kLcdHeight * sizeof(uint16_t);
constexpr size_t kFragmentPayloadBytes = 1400;
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

struct UdpFragmentHeader
{
    char Magic[4];
    uint32_t FrameBytes;
    uint32_t Sequence;
    uint16_t FragmentIndex;
    uint16_t FragmentCount;
    uint32_t FragmentOffset;
    uint16_t PayloadBytes;
    uint16_t Reserved;
};
#pragma pack(pop)

class DisplayStreamer
{
public:
    DisplayStreamer()
    {
        WSADATA data = {};
        m_WinsockReady = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        m_EncodeBuffer.reserve(kMaxJpegBytes);
        m_PendingJpeg.reserve(kMaxJpegBytes);
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ComInitialized = result == S_OK || result == S_FALSE;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_WicFactory));
        if (FAILED(result))
        {
            m_WicFactory = nullptr;
        }
        if (m_WicFactory)
        {
            result = CreateStreamOnHGlobal(nullptr, TRUE, &m_JpegStream);
            if (SUCCEEDED(result))
            {
                ULARGE_INTEGER capacity = {};
                capacity.QuadPart = kMaxJpegBytes;
                result = m_JpegStream->SetSize(capacity);
            }
            if (FAILED(result) && m_JpegStream)
            {
                m_JpegStream->Release();
                m_JpegStream = nullptr;
            }
        }
    }

    ~DisplayStreamer()
    {
        Stop();
        ReleaseCaptureResources();
        if (m_JpegStream)
        {
            m_JpegStream->Release();
        }
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

    bool Start()
    {
        if (!m_WinsockReady || !m_WicFactory || !m_JpegStream ||
            !EnsureCaptureResources())
        {
            return false;
        }
        m_Stopping = false;
        m_SenderThread = std::thread(&DisplayStreamer::SenderLoop, this);
        return true;
    }

    void Stop()
    {
        m_Stopping = true;
        m_FrameCondition.notify_all();
        if (m_SenderThread.joinable())
        {
            m_SenderThread.join();
        }
        CloseSocket();
    }

    bool CaptureAndQueue()
    {
        const ULONGLONG now = GetTickCount64();
        if (!m_HaveMode || now >= m_NextModeRefresh)
        {
            DEVMODE mode = {};
            mode.dmSize = sizeof(mode);
            if (!FindDisplay(mode))
            {
                m_HaveMode = false;
                if (g_Verbose) fwprintf(stderr, L"No 480x360 monitor found.\n");
                return false;
            }
            m_Mode = mode;
            m_HaveMode = true;
            m_NextModeRefresh = now + 2000;
        }
        if (!EnsureCaptureResources())
        {
            return false;
        }

        BOOL captured = StretchBlt(
            m_CaptureMemory, 0, 0, kLcdWidth, kLcdHeight,
            m_Desktop, m_Mode.dmPosition.x, m_Mode.dmPosition.y,
            m_Mode.dmPelsWidth, m_Mode.dmPelsHeight, SRCCOPY | CAPTUREBLT);
        if (!captured)
        {
            m_HaveMode = false;
            return false;
        }
        DrawCursor(m_CaptureMemory, m_Mode);
        if (!EncodeJpeg(static_cast<const uint8_t*>(m_CapturePixels),
                        kCaptureBytes, m_EncodeBuffer))
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_FrameMutex);
            if (m_FrameReady)
            {
                ++m_OverwrittenFrames;
            }
            m_PendingJpeg.swap(m_EncodeBuffer);
            m_PendingSequence = m_NextSequence++;
            m_FrameReady = true;
        }
        m_FrameCondition.notify_one();
        return true;
    }

private:
    bool EnsureCaptureResources()
    {
        if (m_Desktop && m_CaptureMemory && m_CaptureBitmap && m_CapturePixels)
        {
            return true;
        }
        ReleaseCaptureResources();
        m_Desktop = GetDC(nullptr);
        if (!m_Desktop)
        {
            return false;
        }
        m_CaptureMemory = CreateCompatibleDC(m_Desktop);
        if (!m_CaptureMemory)
        {
            ReleaseCaptureResources();
            return false;
        }
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(kLcdWidth);
        info.bmiHeader.biHeight = -static_cast<LONG>(kLcdHeight);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;
        m_CaptureBitmap = CreateDIBSection(
            m_CaptureMemory, &info, DIB_RGB_COLORS,
            &m_CapturePixels, nullptr, 0);
        if (!m_CaptureBitmap || !m_CapturePixels)
        {
            ReleaseCaptureResources();
            return false;
        }
        m_PreviousBitmap = SelectObject(m_CaptureMemory, m_CaptureBitmap);
        SetStretchBltMode(m_CaptureMemory, HALFTONE);
        SetBrushOrgEx(m_CaptureMemory, 0, 0, nullptr);
        return true;
    }

    void ReleaseCaptureResources()
    {
        if (m_CaptureMemory && m_PreviousBitmap)
        {
            SelectObject(m_CaptureMemory, m_PreviousBitmap);
        }
        if (m_CaptureBitmap)
        {
            DeleteObject(m_CaptureBitmap);
        }
        if (m_CaptureMemory)
        {
            DeleteDC(m_CaptureMemory);
        }
        if (m_Desktop)
        {
            ReleaseDC(nullptr, m_Desktop);
        }
        m_Desktop = nullptr;
        m_CaptureMemory = nullptr;
        m_CaptureBitmap = nullptr;
        m_PreviousBitmap = nullptr;
        m_CapturePixels = nullptr;
    }

    bool EncodeJpeg(const uint8_t* Pixels, size_t PixelBytes,
                    std::vector<uint8_t>& Encoded)
    {
        if (!m_WicFactory || !m_JpegStream)
        {
            return false;
        }
        LARGE_INTEGER start = {};
        HRESULT result = m_JpegStream->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(result))
        {
            return false;
        }

        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        result = m_WicFactory->CreateEncoder(GUID_ContainerFormatJpeg,
                                             nullptr, &encoder);
        if (SUCCEEDED(result))
        {
            result = encoder->Initialize(m_JpegStream,
                                         WICBitmapEncoderNoCache);
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
                static_cast<UINT>(PixelBytes),
                const_cast<BYTE*>(Pixels));
        }
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();

        HGLOBAL memory = nullptr;
        ULARGE_INTEGER position = {};
        if (SUCCEEDED(result))
        {
            LARGE_INTEGER offset = {};
            result = m_JpegStream->Seek(offset, STREAM_SEEK_CUR, &position);
        }
        if (SUCCEEDED(result))
        {
            result = GetHGlobalFromStream(m_JpegStream, &memory);
        }
        if (SUCCEEDED(result) && position.HighPart == 0)
        {
            const size_t size = position.LowPart;
            const void* data = GlobalLock(memory);
            if (data && size > 0 && size <= kMaxJpegBytes)
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
        return SUCCEEDED(result) && !Encoded.empty();
    }

    void SenderLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::vector<uint8_t> jpeg;
        jpeg.reserve(kMaxJpegBytes);
        while (!m_Stopping)
        {
            uint32_t sequence = 0;
            {
                std::unique_lock<std::mutex> lock(m_FrameMutex);
                m_FrameCondition.wait(lock, [this]
                {
                    return m_Stopping || m_FrameReady;
                });
                if (m_Stopping)
                {
                    break;
                }
                jpeg.swap(m_PendingJpeg);
                sequence = m_PendingSequence;
                m_FrameReady = false;
            }

            if (m_Socket == INVALID_SOCKET && !DiscoverAndConnect())
            {
                continue;
            }
            if (GetTickCount64() >= m_NextSessionRefresh)
            {
                if (!RefreshStreamSession())
                {
                    CloseSocket();
                    continue;
                }
                m_NextSessionRefresh = GetTickCount64() + 1000;
            }
            if (!SendUdpFrame(jpeg, sequence))
            {
                CloseSocket();
            }
        }
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
            return ConnectOnLocalSubnets();
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
        if (ConnectToAddress(board, false))
        {
            return true;
        }
        return ConnectOnLocalSubnets();
    }

    bool ConnectToAddress(sockaddr_in Board, bool NonBlocking,
                          uint32_t LocalAddress = 0)
    {
        UNREFERENCED_PARAMETER(NonBlocking);
        SOCKET stream = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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
        if (connect(stream, reinterpret_cast<sockaddr*>(&Board),
                    sizeof(Board)) == SOCKET_ERROR)
        {
            closesocket(stream);
            return false;
        }
        int sendBufferBytes = static_cast<int>(kMaxJpegBytes * 2);
        DWORD sendTimeout = 100;
        DWORD receiveTimeout = 600;
        setsockopt(stream, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&sendBufferBytes),
                   sizeof(sendBufferBytes));
        setsockopt(stream, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));

        // Connected UDP sockets on Windows report asynchronous ICMP errors on
        // a later receive/send by default. The v4 handshake already verifies
        // the endpoint, so keep transient ICMP state from tearing down video.
        BOOL reportUdpReset = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl(stream, SIO_UDP_CONNRESET, &reportUdpReset,
                 sizeof(reportUdpReset), nullptr, 0, &bytesReturned,
                 nullptr, nullptr);

        StreamHello hello = {{'B', '2', 'D', 'S'}, htons(4),
                             htons(static_cast<uint16_t>(kLcdWidth)),
                             htons(static_cast<uint16_t>(kLcdHeight)), htons(2)};
        if (send(stream, reinterpret_cast<const char*>(&hello), sizeof(hello), 0) !=
            sizeof(hello))
        {
            closesocket(stream);
            return false;
        }
        StreamAck ack = {};
        int received = recv(stream, reinterpret_cast<char*>(&ack), sizeof(ack), 0);
        if (received != sizeof(ack) || memcmp(ack.Magic, "B2DA", 4) != 0 ||
            ntohs(ack.Version) != 4 ||
            ntohs(ack.Status) != 0)
        {
            closesocket(stream);
            return false;
        }
        m_Socket = stream;
        m_NextSessionRefresh = GetTickCount64() + 1000;
        if (g_Verbose)
            fwprintf(stderr, L"Connected to BOX-2 fragmented UDP stream.\n");
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

        for (uint32_t localAddress : localAddresses)
        {
            SOCKET discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (discovery == INVALID_SOCKET)
            {
                continue;
            }
            BOOL broadcastEnabled = TRUE;
            DWORD timeout = 250;
            setsockopt(discovery, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcastEnabled),
                       sizeof(broadcastEnabled));
            setsockopt(discovery, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(localAddress);
            if (bind(discovery, reinterpret_cast<sockaddr*>(&local),
                     sizeof(local)) == SOCKET_ERROR)
            {
                closesocket(discovery);
                continue;
            }

            DiscoveryRequest request = {{'B', '2', 'D', 'Q'}, htons(1), 0};
            sockaddr_in destination = {};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(kDiscoveryPort);
            destination.sin_addr.s_addr =
                htonl((localAddress & 0xFFFFFF00u) | 0xFFu);
            sockaddr_in board = {};
            int boardLength = sizeof(board);
            int received = SOCKET_ERROR;
            for (int attempt = 0; attempt < 2 && received == SOCKET_ERROR;
                 ++attempt)
            {
                sendto(discovery, reinterpret_cast<const char*>(&request),
                       sizeof(request), 0,
                       reinterpret_cast<sockaddr*>(&destination),
                       sizeof(destination));
                DiscoveryReply reply = {};
                received = recvfrom(discovery, reinterpret_cast<char*>(&reply),
                                    sizeof(reply), 0,
                                    reinterpret_cast<sockaddr*>(&board),
                                    &boardLength);
                if (received == sizeof(reply) &&
                    memcmp(reply.Magic, "B2DR", 4) == 0 &&
                    ntohs(reply.Version) == 1 &&
                    ntohs(reply.Width) == kLcdWidth &&
                    ntohs(reply.Height) == kLcdHeight)
                {
                    board.sin_port = reply.StreamPort;
                    closesocket(discovery);
                    return ConnectToAddress(board, false, localAddress);
                }
                received = SOCKET_ERROR;
                boardLength = sizeof(board);
            }
            closesocket(discovery);
        }
        return false;
    }

    bool RefreshStreamSession()
    {
        // Drain acknowledgements from earlier keepalives without blocking.
        while (true)
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(m_Socket, &readable);
            timeval noWait = {0, 0};
            if (select(0, &readable, nullptr, nullptr, &noWait) <= 0)
            {
                break;
            }
            StreamAck ack = {};
            recv(m_Socket, reinterpret_cast<char*>(&ack), sizeof(ack), 0);
        }

        StreamHello hello = {{'B', '2', 'D', 'S'}, htons(4),
                             htons(static_cast<uint16_t>(kLcdWidth)),
                             htons(static_cast<uint16_t>(kLcdHeight)), htons(2)};
        int sent = send(m_Socket, reinterpret_cast<const char*>(&hello),
                        sizeof(hello), 0);
        if (sent == sizeof(hello))
        {
            return true;
        }
        int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
    }

    bool SendUdpFrame(const std::vector<uint8_t>& Jpeg, uint32_t Sequence)
    {
        if (Jpeg.empty() || Jpeg.size() > kMaxJpegBytes)
        {
            return true;
        }
        const size_t fragmentCount =
            (Jpeg.size() + kFragmentPayloadBytes - 1) /
            kFragmentPayloadBytes;
        for (size_t index = 0; index < fragmentCount; ++index)
        {
            const size_t offset = index * kFragmentPayloadBytes;
            const size_t payloadBytes =
                std::min(kFragmentPayloadBytes, Jpeg.size() - offset);
            UdpFragmentHeader header = {
                {'B', '2', 'U', '4'},
                htonl(static_cast<uint32_t>(Jpeg.size())),
                htonl(Sequence),
                htons(static_cast<uint16_t>(index)),
                htons(static_cast<uint16_t>(fragmentCount)),
                htonl(static_cast<uint32_t>(offset)),
                htons(static_cast<uint16_t>(payloadBytes)),
                0,
            };
            WSABUF buffers[2] = {
                {static_cast<ULONG>(sizeof(header)),
                 reinterpret_cast<char*>(&header)},
                {static_cast<ULONG>(payloadBytes),
                 reinterpret_cast<char*>(
                     const_cast<uint8_t*>(Jpeg.data() + offset))},
            };
            DWORD sentBytes = 0;
            int result = WSASend(m_Socket, buffers, 2, &sentBytes, 0,
                                 nullptr, nullptr);
            if (result == SOCKET_ERROR)
            {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT)
                {
                    ++m_CongestedFrames;
                    return true;
                }
                return false;
            }
            if (sentBytes != sizeof(header) + payloadBytes)
            {
                ++m_CongestedFrames;
                return true;
            }
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
    IStream* m_JpegStream = nullptr;
    HDC m_Desktop = nullptr;
    HDC m_CaptureMemory = nullptr;
    HBITMAP m_CaptureBitmap = nullptr;
    HGDIOBJ m_PreviousBitmap = nullptr;
    void* m_CapturePixels = nullptr;
    DEVMODE m_Mode = {};
    bool m_HaveMode = false;
    ULONGLONG m_NextModeRefresh = 0;
    std::vector<uint8_t> m_EncodeBuffer;
    std::vector<uint8_t> m_PendingJpeg;
    std::mutex m_FrameMutex;
    std::condition_variable m_FrameCondition;
    std::thread m_SenderThread;
    std::atomic<bool> m_Stopping = true;
    bool m_FrameReady = false;
    uint32_t m_PendingSequence = 0;
    uint32_t m_NextSequence = 0;
    uint64_t m_OverwrittenFrames = 0;
    uint64_t m_CongestedFrames = 0;
    ULONGLONG m_NextSessionRefresh = 0;
    SOCKET m_Socket = INVALID_SOCKET;
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
    const bool timerResolutionSet = timeBeginPeriod(1) == TIMERR_NOERROR;
    if (!streamer.Start())
    {
        fwprintf(stderr, L"BOX-2 capture pipeline initialization failed.\n");
        if (timerResolutionSet) timeEndPeriod(1);
        SwDeviceClose(softwareDevice);
        return 4;
    }
    while (true)
    {
        ULONGLONG start = GetTickCount64();
        bool captured = streamer.CaptureAndQueue();
        ULONGLONG elapsed = GetTickCount64() - start;
        DWORD delay = captured
                          ? static_cast<DWORD>(elapsed < 33 ? 33 - elapsed : 1)
                          : 500;
        if (WaitForSingleObject(g_StopEvent, delay) == WAIT_OBJECT_0)
        {
            break;
        }
    }
    streamer.Stop();
    if (timerResolutionSet) timeEndPeriod(1);

    SwDeviceClose(softwareDevice);
    CloseHandle(creationEvent);
    CloseHandle(g_StopEvent);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
