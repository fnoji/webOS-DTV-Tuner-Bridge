/**
 * BonDriver_WebOS.cpp: LG webOS TV 内蔵チューナー＆B-CAS 向け Windows BonDriver2 DLL
 *  - Host=auto / Mac= 指定によるマルチ NIC 対応 LAN 自動発見 (UDP :40581)
 *  - TV 本体チャンネル一覧自動取得 (TCP :40580 CHLIST)
 *  - TV 内蔵 B-CAS 自動復号受信・無通信切断検知＆自動再接続・ログ出力 (Log=1)
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include "IBonDriver2.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

HMODULE g_hModule = nullptr;

struct ChannelEntry {
    int          space = 0;
    int          media = 1;
    uint32_t     phy = 0;
    uint32_t     freqKhz = 0;
    uint16_t     tsid = 0;
    uint16_t     onid = 0;
    uint16_t     sid = 0;
    std::wstring displayName;
};

struct SpaceEntry {
    std::wstring              name;
    std::vector<ChannelEntry> channels;
};

struct DiscoveredTV {
    std::string ip;
    int         port = 40580;
    std::string mac;
    std::string macEth;
    std::string macWlan;
    std::string host;
};

static std::wstring utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

static std::string wideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

static std::string normalizeMac(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isxdigit(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    if (out == "000000000000") return "";
    return out;
}

static std::string formatMacColon(const std::string &hex12) {
    if (hex12.size() != 12) return hex12;
    std::string out;
    for (size_t i = 0; i < 12; i += 2) {
        if (i > 0) out.push_back(':');
        out.push_back(hex12[i]);
        out.push_back(hex12[i + 1]);
    }
    return out;
}

/** UP 状態の全 IPv4 NIC の指向性ブロードキャスト先 + INADDR_BROADCAST を収集する */
static std::vector<uint32_t> collectBroadcastAddrs(SOCKET u) {
    std::vector<uint32_t> addrs;
    INTERFACE_INFO ifList[32];
    DWORD retBytes = 0;
    if (WSAIoctl(u, SIO_GET_INTERFACE_LIST, nullptr, 0, ifList, sizeof(ifList), &retBytes, nullptr, nullptr) == 0) {
        DWORD count = retBytes / sizeof(INTERFACE_INFO);
        for (DWORD i = 0; i < count; i++) {
            if (!(ifList[i].iiFlags & IFF_UP) || (ifList[i].iiFlags & IFF_LOOPBACK)) continue;
            uint32_t ip = reinterpret_cast<const struct sockaddr_in *>(&ifList[i].iiAddress)->sin_addr.s_addr;
            uint32_t mask = reinterpret_cast<const struct sockaddr_in *>(&ifList[i].iiNetmask)->sin_addr.s_addr;
            uint32_t dirBcast = ip | (~mask);
            if (dirBcast != 0 && std::find(addrs.begin(), addrs.end(), dirBcast) == addrs.end()) {
                addrs.push_back(dirBcast);
            }
        }
    }
    addrs.push_back(INADDR_BROADCAST);
    return addrs;
}

static std::string extractTagValue(const std::string &line, const std::string &key) {
    std::string pat = key + "=";
    size_t pos = 0;
    while (pos < line.size()) {
        size_t found = line.find(pat, pos);
        if (found == std::string::npos) return "";
        if (found == 0 || line[found - 1] == '\t' || line[found - 1] == ' ') {
            size_t vs = found + pat.size();
            size_t ve = line.find_first_of("\t\r\n", vs);
            if (ve == std::string::npos) ve = line.size();
            return line.substr(vs, ve - vs);
        }
        pos = found + pat.size();
    }
    return "";
}

static SOCKET connectWithTimeout(const std::string &host, int port, int timeoutMs) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        return INVALID_SOCKET;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        fd_set wset, eset;
        FD_ZERO(&wset);
        FD_ZERO(&eset);
        FD_SET(s, &wset);
        FD_SET(s, &eset);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, nullptr, &wset, &eset, &tv);
        if (sel <= 0 || FD_ISSET(s, &eset)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        int soerr = 0;
        int optlen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &optlen);
        if (soerr != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
    DWORD rcvTimeout = 1000; /* 1秒タイムアウトで無通信検知を迅速化 */
    DWORD sndTimeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&rcvTimeout), sizeof(rcvTimeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sndTimeout), sizeof(sndTimeout));
    return s;
}

static bool recvLine(SOCKET s, std::string &lineOut, int maxWaitLoops = 5) {
    lineOut.clear();
    char ch = 0;
    int timeouts = 0;
    while (lineOut.size() < 4096) {
        int n = recv(s, &ch, 1, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (WSAGetLastError() == WSAETIMEDOUT && ++timeouts < maxWaitLoops) continue;
            return false;
        }
        if (ch == '\n') break;
        if (ch != '\r') lineOut.push_back(ch);
    }
    return true;
}

} // namespace

class BonDriverWebOS final : public IBonDriver2 {
public:
    BonDriverWebOS() {
        WSADATA wsa = {};
        m_wsaOk = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        m_tsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        loadIniConfig();
        logMsg("BonDriver_WebOS initialized (host=%s, port=%d, mac=%s, tuner=%s, decodeOnTV=%d)",
               m_hostCfg.c_str(), m_portCfg, m_targetMac.empty() ? "(any)" : m_targetMac.c_str(),
               m_tunerCfg.c_str(), m_decodeOnTV ? 1 : 0);
        initSpacesLayout();
        if (m_autoChannelList) {
            fetchChannelListFromTV();
        }
    }

    ~BonDriverWebOS() {
        CloseTuner();
        if (m_tsEvent) {
            CloseHandle(m_tsEvent);
            m_tsEvent = nullptr;
        }
        if (m_wsaOk) {
            WSACleanup();
        }
    }

    /* ---- IBonDriver ---- */
    const BOOL OpenTuner(void) override {
        std::lock_guard<std::mutex> lk(m_ctrlMtx);
        if (m_isOpen) return TRUE;

        std::string targetIp = m_resolvedIp;
        int targetPort = m_resolvedPort ? m_resolvedPort : m_portCfg;
        SOCKET s = INVALID_SOCKET;
        if (!targetIp.empty()) {
            s = connectWithTimeout(targetIp, targetPort, m_connectTimeoutMs);
        }
        if (s == INVALID_SOCKET) {
            if (!resolveTarget(targetIp, targetPort)) {
                logMsg("[OPEN] Failed to resolve target TV (host=%s, mac=%s)",
                       m_hostCfg.c_str(), m_targetMac.empty() ? "(any)" : m_targetMac.c_str());
                return FALSE;
            }
            s = connectWithTimeout(targetIp, targetPort, m_connectTimeoutMs);
            if (s == INVALID_SOCKET) {
                logMsg("[OPEN] TCP connect failed to %s:%d (err=%d)", targetIp.c_str(), targetPort, WSAGetLastError());
                return FALSE;
            }
        }

        const char *hello = "HELLO\n";
        send(s, hello, static_cast<int>(std::strlen(hello)), 0);
        std::string resp;
        bool ok = recvLine(s, resp) && (resp.rfind("OK ", 0) == 0);
        closesocket(s);
        if (!ok) {
            logMsg("[OPEN] HELLO handshake failed on %s:%d", targetIp.c_str(), targetPort);
            return FALSE;
        }

        m_resolvedIp = targetIp;
        m_resolvedPort = targetPort;
        std::string helloMac = normalizeMac(extractTagValue(resp, "mac"));
        persistDiscoveredTV(targetIp, helloMac, "", "");
        logMsg("[OPEN] Connected to %s:%d (%s)", targetIp.c_str(), targetPort, resp.c_str());

        if (m_autoChannelList && !m_chLoaded) {
            fetchChannelListFromTV();
        }
        m_isOpen = true;
        return TRUE;
    }

    void CloseTuner(void) override {
        stopStreamThread();
        std::lock_guard<std::mutex> lk(m_ctrlMtx);
        if (m_isOpen) logMsg("[CLOSE] Tuner closed (total bytes=%llu)", static_cast<unsigned long long>(m_bytesReceived.load()));
        m_isOpen = false;
        m_signalLocked = false;
        m_curSpace = 0;
        m_curChannel = 0;
        PurgeTsStream();
    }

    const BOOL SetChannel(const BYTE bCh) override {
        return SetChannel(0, static_cast<DWORD>(bCh));
    }

    const float GetSignalLevel(void) override {
        if (!m_isOpen || !m_signalLocked.load()) return 0.0f;
        return (m_bytesReceived.load() > 0) ? 28.5f : 22.0f;
    }

    const DWORD WaitTsStream(const DWORD dwTimeOut = 0) override {
        {
            std::lock_guard<std::mutex> lk(m_queueMtx);
            if (!m_tsQueue.empty()) return WAIT_OBJECT_0;
        }
        if (!m_tsEvent) return WAIT_FAILED;
        return WaitForSingleObject(m_tsEvent, dwTimeOut);
    }

    const DWORD GetReadyCount(void) override {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        return static_cast<DWORD>(m_tsQueue.size());
    }

    const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override {
        BYTE *pSrc = nullptr;
        DWORD srcSize = 0;
        if (!GetTsStream(&pSrc, &srcSize, pdwRemain)) {
            if (pdwSize) *pdwSize = 0;
            return FALSE;
        }
        if (pDst && pSrc && srcSize > 0) {
            std::memcpy(pDst, pSrc, srcSize);
        }
        if (pdwSize) *pdwSize = srcSize;
        return TRUE;
    }

    const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        if (m_tsQueue.empty()) {
            if (ppDst) *ppDst = nullptr;
            if (pdwSize) *pdwSize = 0;
            if (pdwRemain) *pdwRemain = 0;
            return FALSE;
        }
        m_lastChunk = std::move(m_tsQueue.front());
        m_tsQueue.pop_front();
        m_queuedBytes -= m_lastChunk.size();

        if (ppDst) *ppDst = m_lastChunk.data();
        if (pdwSize) *pdwSize = static_cast<DWORD>(m_lastChunk.size());
        if (pdwRemain) *pdwRemain = static_cast<DWORD>(m_tsQueue.size());
        return TRUE;
    }

    void PurgeTsStream(void) override {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_tsQueue.clear();
        m_queuedBytes = 0;
        m_lastChunk.clear();
    }

    void Release(void) override {
        delete this;
    }

    /* ---- IBonDriver2 ---- */
    LPCWSTR GetTunerName(void) override {
        return m_tunerName.c_str();
    }

    const BOOL IsTunerOpening(void) override {
        return m_isOpen ? TRUE : FALSE;
    }

    LPCWSTR EnumTuningSpace(const DWORD dwSpace) override {
        if (dwSpace >= m_spaces.size()) return nullptr;
        return m_spaces[dwSpace].name.c_str();
    }

    LPCWSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override {
        if (dwSpace >= m_spaces.size()) return nullptr;
        if (dwChannel >= m_spaces[dwSpace].channels.size()) return nullptr;
        return m_spaces[dwSpace].channels[dwChannel].displayName.c_str();
    }

    const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override {
        ChannelEntry target;
        {
            std::lock_guard<std::mutex> lk(m_ctrlMtx);
            if (!m_isOpen) return FALSE;
            if (dwSpace >= m_spaces.size() || dwChannel >= m_spaces[dwSpace].channels.size()) {
                return FALSE;
            }
            target = m_spaces[dwSpace].channels[dwChannel];
        }

        stopStreamThread();
        PurgeTsStream();

        std::lock_guard<std::mutex> lk(m_ctrlMtx);
        SOCKET s = connectAndStartStream(target);
        if (s == INVALID_SOCKET) {
            m_signalLocked.store(false);
            return FALSE;
        }

        m_activeCh = target;
        m_curSpace = dwSpace;
        m_curChannel = dwChannel;
        m_signalLocked.store(true);
        m_bytesReceived.store(0);
        startStreamThread(s, target);
        return TRUE;
    }

    const DWORD GetCurSpace(void) override {
        return m_curSpace;
    }

    const DWORD GetCurChannel(void) override {
        return m_curChannel;
    }

private:
    void logMsg(const char *fmt, ...) {
        if (!m_logEnabled || m_logPath.empty()) return;
        char body[768];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[832];
        std::snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

        std::lock_guard<std::mutex> lk(m_logMtx);
        FILE *fp = nullptr;
        if (_wfopen_s(&fp, m_logPath.c_str(), L"ab") == 0 && fp) {
            std::fwrite(line, 1, std::strlen(line), fp);
            std::fclose(fp);
        }
    }

    void loadIniConfig() {
        wchar_t dllPath[MAX_PATH] = {};
        if (g_hModule && GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) > 0) {
            wchar_t iniBuf[MAX_PATH] = {};
            wcscpy_s(iniBuf, MAX_PATH, dllPath);
            wchar_t *dot = std::wcsrchr(iniBuf, L'.');
            if (dot) wcscpy_s(dot, MAX_PATH - (dot - iniBuf), L".ini");
            if (GetFileAttributesW(iniBuf) == INVALID_FILE_ATTRIBUTES) {
                /* 別名コピー時 (例: BonDriver_WebOS_2.dll) に専用 .ini が無ければ共有 BonDriver_WebOS.ini を探す */
                wchar_t fbBuf[MAX_PATH] = {};
                wcscpy_s(fbBuf, MAX_PATH, dllPath);
                wchar_t *slash = std::wcsrchr(fbBuf, L'\\');
                if (slash) {
                    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - fbBuf), L"BonDriver_WebOS.ini");
                    if (GetFileAttributesW(fbBuf) != INVALID_FILE_ATTRIBUTES) {
                        wcscpy_s(iniBuf, MAX_PATH, fbBuf);
                    }
                }
            }
            m_iniPath = iniBuf;

            wchar_t logBuf[MAX_PATH] = {};
            wcscpy_s(logBuf, MAX_PATH, dllPath);
            dot = std::wcsrchr(logBuf, L'.');
            if (dot) wcscpy_s(dot, MAX_PATH - (dot - logBuf), L".log");
            m_logPath = logBuf;
        } else {
            m_iniPath = L"BonDriver_WebOS.ini";
            m_logPath = L"BonDriver_WebOS.log";
        }

        const wchar_t *ini = m_iniPath.c_str();
        wchar_t hostW[128] = {}, macW[64] = {}, tunerW[32] = {};
        wchar_t lastIpW[64] = {}, lastMacW[64] = {}, lastEthW[64] = {}, lastWlanW[64] = {};
        GetPrivateProfileStringW(L"Server", L"Host", L"auto", hostW, 128, ini);
        GetPrivateProfileStringW(L"Server", L"Mac", L"", macW, 64, ini);
        GetPrivateProfileStringW(L"Server", L"Tuner", L"auto", tunerW, 32, ini);
        GetPrivateProfileStringW(L"Server", L"LastIp", L"", lastIpW, 64, ini);
        GetPrivateProfileStringW(L"Server", L"LastMac", L"", lastMacW, 64, ini);
        GetPrivateProfileStringW(L"Server", L"LastEthMac", L"", lastEthW, 64, ini);
        GetPrivateProfileStringW(L"Server", L"LastWlanMac", L"", lastWlanW, 64, ini);
        m_hostCfg = wideToUtf8(hostW);
        m_targetMac = normalizeMac(wideToUtf8(macW));
        m_tunerCfg = wideToUtf8(tunerW);
        if (m_tunerCfg != "0" && m_tunerCfg != "1") m_tunerCfg = "auto";
        m_lastIp = wideToUtf8(lastIpW);
        m_lastMac = normalizeMac(wideToUtf8(lastMacW));
        m_lastEthMac = normalizeMac(wideToUtf8(lastEthW));
        m_lastWlanMac = normalizeMac(wideToUtf8(lastWlanW));
        m_portCfg = static_cast<int>(GetPrivateProfileIntW(L"Server", L"Port", 40580, ini));
        m_decodeOnTV = (GetPrivateProfileIntW(L"Server", L"DecodeOnTV", 1, ini) != 0);
        m_autoChannelList = (GetPrivateProfileIntW(L"Server", L"AutoChannelList", 1, ini) != 0);
        m_uniqueTransponder = (GetPrivateProfileIntW(L"Server", L"UniqueTransponder", 1, ini) != 0);
        m_wakeOnLan = (GetPrivateProfileIntW(L"Server", L"WakeOnLan", 1, ini) != 0);
        m_connectTimeoutMs = static_cast<int>(GetPrivateProfileIntW(L"Server", L"ConnectTimeoutMs", 2000, ini));
        m_logEnabled = (GetPrivateProfileIntW(L"Server", L"Log", 1, ini) != 0);
        if (m_connectTimeoutMs < 300) m_connectTimeoutMs = 300;
    }

    void persistDiscoveredTV(const std::string &ip, const std::string &mac,
                             const std::string &macEth, const std::string &macWlan) {
        if (m_iniPath.empty()) return;
        const wchar_t *ini = m_iniPath.c_str();
        if (!ip.empty() && ip != m_lastIp) {
            m_lastIp = ip;
            WritePrivateProfileStringW(L"Server", L"LastIp", utf8ToWide(ip).c_str(), ini);
        }
        if (mac.size() == 12 && mac != m_lastMac) {
            m_lastMac = mac;
            WritePrivateProfileStringW(L"Server", L"LastMac", utf8ToWide(formatMacColon(mac)).c_str(), ini);
        }
        if (macEth.size() == 12 && macEth != m_lastEthMac) {
            m_lastEthMac = macEth;
            WritePrivateProfileStringW(L"Server", L"LastEthMac", utf8ToWide(formatMacColon(macEth)).c_str(), ini);
        }
        if (macWlan.size() == 12 && macWlan != m_lastWlanMac) {
            m_lastWlanMac = macWlan;
            WritePrivateProfileStringW(L"Server", L"LastWlanMac", utf8ToWide(formatMacColon(macWlan)).c_str(), ini);
        }
    }

    void sendWolMagicPackets(const std::vector<std::string> &macs) {
        if (!m_wsaOk || macs.empty()) return;
        SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (u == INVALID_SOCKET) return;
        BOOL bcast = TRUE;
        setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&bcast), sizeof(bcast));
        std::vector<uint32_t> bcastAddrs = collectBroadcastAddrs(u);

        for (const auto &hex : macs) {
            if (hex.size() != 12) continue;
            uint8_t macBytes[6] = {};
            for (int i = 0; i < 6; i++) {
                macBytes[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
            }
            uint8_t pkt[102];
            std::memset(pkt, 0xFF, 6);
            for (int r = 0; r < 16; r++) {
                std::memcpy(pkt + 6 + r * 6, macBytes, 6);
            }
            for (uint32_t ba : bcastAddrs) {
                for (uint16_t wolPort : { uint16_t(9), uint16_t(7) }) {
                    struct sockaddr_in dst = {};
                    dst.sin_family = AF_INET;
                    dst.sin_port = htons(wolPort);
                    dst.sin_addr.s_addr = ba;
                    sendto(u, reinterpret_cast<const char *>(pkt), sizeof(pkt), 0,
                           reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
                }
            }
        }
        closesocket(u);
    }

    std::vector<std::string> collectWakeMacs() const {
        std::vector<std::string> out;
        auto pushUniq = [&](const std::string &m) {
            if (m.size() == 12 && std::find(out.begin(), out.end(), m) == out.end()) {
                out.push_back(m);
            }
        };
        pushUniq(m_targetMac);
        pushUniq(m_lastMac);
        pushUniq(m_lastWlanMac);
        pushUniq(m_lastEthMac);
        return out;
    }

    void initSpacesLayout() {
        m_spaces.resize(3);
        m_spaces[0].name = L"地デジ";
        m_spaces[1].name = L"BS";
        m_spaces[2].name = L"CS";
    }

    /** PC の全有効 IPv4 NIC へ指向性ブロードキャストを送信して LAN 内の LG TV を探索する */
    std::vector<DiscoveredTV> discoverTVs(int waitMs) {
        std::vector<DiscoveredTV> found;
        if (!m_wsaOk) return found;

        SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (u == INVALID_SOCKET) return found;

        BOOL bcast = TRUE;
        setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&bcast), sizeof(bcast));

        struct sockaddr_in bindAddr = {};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddr.sin_port = 0;
        bind(u, reinterpret_cast<struct sockaddr *>(&bindAddr), sizeof(bindAddr));

        const char *probe = "WEBOS_TUNER_DISCOVER\n";
        int plen = static_cast<int>(std::strlen(probe));

        /* 各 UP IPv4 インターフェースのサブネットブロードキャスト先を収集 */
        std::vector<uint32_t> bcastAddrs = collectBroadcastAddrs(u);

        for (uint32_t ba : bcastAddrs) {
            struct sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(40581);
            dst.sin_addr.s_addr = ba;
            sendto(u, probe, plen, 0, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        while (true) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (rem <= 0) break;

            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(u, &rset);
            struct timeval tv;
            tv.tv_sec = static_cast<long>(rem / 1000);
            tv.tv_usec = static_cast<long>((rem % 1000) * 1000);
            if (select(0, &rset, nullptr, nullptr, &tv) <= 0) break;

            char rbuf[512] = {};
            struct sockaddr_in from = {};
            int flen = sizeof(from);
            int rn = recvfrom(u, rbuf, sizeof(rbuf) - 1, 0, reinterpret_cast<struct sockaddr *>(&from), &flen);
            if (rn <= 0) continue;
            rbuf[rn] = '\0';
            std::string line(rbuf);
            if (line.rfind("WEBOS_TUNER_HERE", 0) != 0) continue;

            char ipStr[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));

            DiscoveredTV tvInfo;
            tvInfo.ip = ipStr;
            std::string pStr = extractTagValue(line, "port");
            if (!pStr.empty()) tvInfo.port = std::atoi(pStr.c_str());
            tvInfo.mac = normalizeMac(extractTagValue(line, "mac"));
            tvInfo.macEth = normalizeMac(extractTagValue(line, "mac_eth"));
            tvInfo.macWlan = normalizeMac(extractTagValue(line, "mac_wlan"));
            tvInfo.host = extractTagValue(line, "host");

            bool dup = false;
            for (const auto &ex : found) {
                if (ex.ip == tvInfo.ip) { dup = true; break; }
            }
            if (!dup) {
                logMsg("[DISCOVER] Found TV: ip=%s port=%d host=%s mac=%s (eth=%s, wlan=%s)",
                       tvInfo.ip.c_str(), tvInfo.port, tvInfo.host.c_str(),
                       tvInfo.mac.c_str(), tvInfo.macEth.c_str(), tvInfo.macWlan.c_str());
                bool macMatch = !m_targetMac.empty() &&
                    (tvInfo.mac == m_targetMac || tvInfo.macEth == m_targetMac || tvInfo.macWlan == m_targetMac);
                found.push_back(std::move(tvInfo));
                if (macMatch) break;
                auto early = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                if (early < deadline) deadline = early;
            }
        }
        closesocket(u);
        return found;
    }

    bool pickMatchingTV(const std::vector<DiscoveredTV> &tvs, std::string &ipOut, int &portOut) {
        if (tvs.empty()) return false;
        const DiscoveredTV *picked = &tvs[0];
        if (!m_targetMac.empty()) {
            picked = nullptr;
            for (const auto &t : tvs) {
                if (t.mac == m_targetMac || t.macEth == m_targetMac || t.macWlan == m_targetMac) {
                    picked = &t;
                    break;
                }
            }
            if (!picked) {
                logMsg("[DISCOVER] Found %zu TV(s), but none matched Mac=%s", tvs.size(), m_targetMac.c_str());
                return false;
            }
            logMsg("[DISCOVER] Matched MAC %s -> %s:%d", m_targetMac.c_str(), picked->ip.c_str(),
                   (m_portCfg > 0) ? m_portCfg : picked->port);
        } else if (tvs.size() > 1) {
            logMsg("[DISCOVER] WARNING: %zu LG TVs found on LAN! Using first (%s). Set Mac= in .ini to pin a specific TV.",
                   tvs.size(), tvs[0].ip.c_str());
        }
        ipOut = picked->ip;
        portOut = (m_portCfg > 0) ? m_portCfg : picked->port;
        persistDiscoveredTV(picked->ip, picked->mac, picked->macEth, picked->macWlan);
        return true;
    }

    bool resolveTarget(std::string &ipOut, int &portOut) {
        bool isAuto = m_hostCfg.empty() || (_stricmp(m_hostCfg.c_str(), "auto") == 0);
        if (!isAuto) {
            ipOut = m_hostCfg;
            portOut = m_portCfg;
            SOCKET testS = connectWithTimeout(ipOut, portOut, 800);
            if (testS != INVALID_SOCKET) {
                closesocket(testS);
                return true;
            }
            logMsg("[DISCOVER] Configured Host=%s unreachable, trying auto discovery / WOL...", ipOut.c_str());
        }

        auto tvs = discoverTVs(1500);
        if (pickMatchingTV(tvs, ipOut, portOut)) return true;

        /* スタンバイ状態の可能性があるため Wake-on-LAN を送信して復帰を待機 */
        if (m_wakeOnLan) {
            std::vector<std::string> macs = collectWakeMacs();
            if (!macs.empty()) {
                std::string macSummary;
                for (size_t i = 0; i < macs.size(); i++) {
                    if (i > 0) macSummary += ",";
                    macSummary += formatMacColon(macs[i]);
                }
                logMsg("[WOL] TV unreachable; sending magic packet to %s ...", macSummary.c_str());
                auto startWait = std::chrono::steady_clock::now();
                for (int attempt = 0; attempt < 12; attempt++) {
                    if (attempt % 3 == 0) sendWolMagicPackets(macs);
                    if (!isAuto) {
                        SOCKET ws = connectWithTimeout(m_hostCfg, m_portCfg, 400);
                        if (ws != INVALID_SOCKET) {
                            closesocket(ws);
                            ipOut = m_hostCfg;
                            portOut = m_portCfg;
                            logMsg("[WOL] TV woke up at %s:%d", ipOut.c_str(), portOut);
                            return true;
                        }
                    }
                    auto wtvs = discoverTVs(550);
                    if (pickMatchingTV(wtvs, ipOut, portOut)) {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startWait).count();
                        logMsg("[WOL] TV woke up in %lld ms (%s:%d)", static_cast<long long>(ms), ipOut.c_str(), portOut);
                        return true;
                    }
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startWait).count() >= 8000) {
                        break;
                    }
                }
                logMsg("[WOL] Timed out waiting for TV wakeup");
            }
        }
        return false;
    }

    bool fetchChannelListFromTV() {
        if (!m_wsaOk) return false;
        std::string ip = m_resolvedIp;
        int port = m_resolvedPort ? m_resolvedPort : m_portCfg;
        if (ip.empty()) {
            if (!resolveTarget(ip, port)) return false;
            m_resolvedIp = ip;
            m_resolvedPort = port;
        }

        SOCKET s = connectWithTimeout(ip, port, m_connectTimeoutMs);
        if (s == INVALID_SOCKET) return false;

        const char *cmd = "CHLIST\n";
        send(s, cmd, static_cast<int>(std::strlen(cmd)), 0);

        for (auto &sp : m_spaces) sp.channels.clear();

        std::string line;
        while (recvLine(s, line)) {
            if (line == "END") break;
            std::vector<std::string> cols;
            size_t start = 0;
            while (start <= line.size()) {
                size_t tab = line.find('\t', start);
                if (tab == std::string::npos) {
                    cols.push_back(line.substr(start));
                    break;
                }
                cols.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            if (cols.size() < 9) continue;

            ChannelEntry ch;
            ch.space   = std::atoi(cols[0].c_str());
            ch.media   = std::atoi(cols[1].c_str());
            ch.phy     = static_cast<uint32_t>(std::strtoul(cols[2].c_str(), nullptr, 10));
            ch.freqKhz = static_cast<uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 10));
            ch.tsid    = static_cast<uint16_t>(std::strtoul(cols[4].c_str(), nullptr, 10));
            ch.onid    = static_cast<uint16_t>(std::strtoul(cols[5].c_str(), nullptr, 10));
            ch.sid     = static_cast<uint16_t>(std::strtoul(cols[6].c_str(), nullptr, 10));
            if (ch.space < 0 || ch.space >= 3 || ch.freqKhz == 0) continue;

            if (m_uniqueTransponder) {
                bool dup = false;
                for (const auto &ex : m_spaces[ch.space].channels) {
                    if (ex.media == ch.media && ex.phy == ch.phy && ex.tsid == ch.tsid) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
            }

            std::string label = (ch.space == 0)
                ? (cols[7] + " " + cols[8] + " (" + cols[2] + "ch)")
                : (cols[7] + " " + cols[8]);
            ch.displayName = utf8ToWide(label);
            m_spaces[ch.space].channels.push_back(std::move(ch));
        }

        closesocket(s);
        m_chLoaded = !(m_spaces[0].channels.empty() && m_spaces[1].channels.empty() && m_spaces[2].channels.empty());
        logMsg("[CHLIST] Loaded GR=%zu BS=%zu CS=%zu from %s:%d",
               m_spaces[0].channels.size(), m_spaces[1].channels.size(), m_spaces[2].channels.size(),
               ip.c_str(), port);
        return m_chLoaded;
    }

    SOCKET connectAndStartStream(const ChannelEntry &target) {
        std::string ip = m_resolvedIp;
        int port = m_resolvedPort ? m_resolvedPort : m_portCfg;
        if (ip.empty()) {
            if (!resolveTarget(ip, port)) return INVALID_SOCKET;
            m_resolvedIp = ip;
            m_resolvedPort = port;
        }

        SOCKET s = connectWithTimeout(ip, port, m_connectTimeoutMs + 2000);
        if (s == INVALID_SOCKET) {
            /* IPが変わっていた可能性に備え、auto/MAC再発見を1回試す */
            std::string newIp;
            int newPort = m_portCfg;
            if (resolveTarget(newIp, newPort) && newIp != ip) {
                m_resolvedIp = newIp;
                m_resolvedPort = newPort;
                s = connectWithTimeout(newIp, newPort, m_connectTimeoutMs + 2000);
            }
            if (s == INVALID_SOCKET) {
                logMsg("[TUNE] Connect failed to %s:%d", ip.c_str(), port);
                return INVALID_SOCKET;
            }
        }

        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "TUNE %d %u %u %u 1 %s\n",
                      target.media, target.phy, target.freqKhz, static_cast<unsigned>(target.tsid),
                      m_tunerCfg.c_str());
        send(s, cmd, static_cast<int>(std::strlen(cmd)), 0);

        std::string resp;
        if (!recvLine(s, resp) || resp.rfind("OK ", 0) != 0) {
            logMsg("[TUNE] Failed: %s -> %s", cmd, resp.empty() ? "(timeout/eof)" : resp.c_str());
            closesocket(s);
            return INVALID_SOCKET;
        }

        char scmd[32];
        std::snprintf(scmd, sizeof(scmd), "STREAM %d\n", m_decodeOnTV ? 1 : 0);
        send(s, scmd, static_cast<int>(std::strlen(scmd)), 0);
        std::string sresp;
        if (!recvLine(s, sresp) || sresp.rfind("OK", 0) != 0) {
            logMsg("[STREAM] Failed: %s", sresp.empty() ? "(timeout/eof)" : sresp.c_str());
            closesocket(s);
            return INVALID_SOCKET;
        }

        logMsg("[TUNE] OK (%s) space=%d media=%d phy=%u freq=%u tsid=0x%04x (%s)",
               resp.c_str(), target.space, target.media, target.phy, target.freqKhz, target.tsid,
               wideToUtf8(target.displayName).c_str());
        return s;
    }

    void startStreamThread(SOCKET s, const ChannelEntry &ch) {
        m_streamStop.store(false);
        m_streamSock.store(s);
        m_streamThread = std::thread(&BonDriverWebOS::streamLoop, this, s, ch);
    }

    void stopStreamThread() {
        m_streamStop.store(true);
        SOCKET s = m_streamSock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }
    }

    void streamLoop(SOCKET s, ChannelEntry ch) {
        constexpr size_t CHUNK_BYTES = 188u * 256u;
        constexpr size_t MAX_QUEUE_BYTES = 16u * 1024u * 1024u;
        std::vector<BYTE> recvBuf(65536);
        std::vector<BYTE> carry;
        carry.reserve(CHUNK_BYTES * 2);
        int consecTimeouts = 0;

        while (!m_streamStop.load()) {
            int n = recv(s, reinterpret_cast<char *>(recvBuf.data()), static_cast<int>(recvBuf.size()), 0);
            if (n <= 0) {
                if (m_streamStop.load()) break;
                int err = (n < 0) ? WSAGetLastError() : 0;
                if (n < 0 && err == WSAETIMEDOUT && ++consecTimeouts < 4) {
                    continue; /* 最大4秒まで無通信を許容 */
                }

                /* 切断または4秒無通信検知 -> 信号断通知＆自動再接続ループ */
                m_signalLocked.store(false);
                logMsg("[STREAM] Lost connection (n=%d err=%d timeouts=%d). Attempting reconnect...",
                       n, err, consecTimeouts);
                consecTimeouts = 0;
                carry.clear();

                SOCKET cur = m_streamSock.exchange(INVALID_SOCKET);
                if (cur != INVALID_SOCKET) closesocket(cur);

                SOCKET ns = INVALID_SOCKET;
                while (!m_streamStop.load()) {
                    Sleep(1500);
                    if (m_streamStop.load()) break;
                    std::string dummyIp;
                    int dummyPort = m_portCfg;
                    if (_stricmp(m_hostCfg.c_str(), "auto") == 0 || m_hostCfg.empty()) {
                        if (resolveTarget(dummyIp, dummyPort)) {
                            m_resolvedIp = dummyIp;
                            m_resolvedPort = dummyPort;
                        }
                    }
                    ns = connectAndStartStream(ch);
                    if (ns != INVALID_SOCKET) break;
                }
                if (ns == INVALID_SOCKET || m_streamStop.load()) {
                    if (ns != INVALID_SOCKET) closesocket(ns);
                    break;
                }
                s = ns;
                m_streamSock.store(s);
                m_signalLocked.store(true);
                logMsg("[STREAM] Reconnected successfully.");
                continue;
            }

            consecTimeouts = 0;
            m_signalLocked.store(true);
            m_bytesReceived.fetch_add(static_cast<uint64_t>(n));
            carry.insert(carry.end(), recvBuf.begin(), recvBuf.begin() + n);

            size_t aligned = (carry.size() / 188u) * 188u;
            if (aligned >= 188u * 16u || (aligned > 0 && carry.size() >= CHUNK_BYTES)) {
                size_t take = (aligned > CHUNK_BYTES) ? CHUNK_BYTES : aligned;
                std::vector<BYTE> chunk(carry.begin(), carry.begin() + static_cast<ptrdiff_t>(take));
                carry.erase(carry.begin(), carry.begin() + static_cast<ptrdiff_t>(take));

                {
                    std::lock_guard<std::mutex> lk(m_queueMtx);
                    while (m_queuedBytes + chunk.size() > MAX_QUEUE_BYTES && !m_tsQueue.empty()) {
                        m_queuedBytes -= m_tsQueue.front().size();
                        m_tsQueue.pop_front();
                    }
                    m_queuedBytes += chunk.size();
                    m_tsQueue.push_back(std::move(chunk));
                }
                if (m_tsEvent) SetEvent(m_tsEvent);
            }
        }
    }

    bool m_wsaOk = false;
    std::wstring m_iniPath;
    std::wstring m_logPath;
    std::mutex m_logMtx;

    std::string m_hostCfg = "auto";
    std::string m_targetMac;
    std::string m_tunerCfg = "auto";
    std::string m_lastIp;
    std::string m_lastMac;
    std::string m_lastEthMac;
    std::string m_lastWlanMac;
    int m_portCfg = 40580;
    bool m_decodeOnTV = true;
    bool m_autoChannelList = true;
    bool m_uniqueTransponder = true;
    bool m_wakeOnLan = true;
    int m_connectTimeoutMs = 2000;
    bool m_logEnabled = true;

    std::string m_resolvedIp;
    int m_resolvedPort = 40580;

    std::wstring m_tunerName = L"LG webOS TV Tuner";
    std::vector<SpaceEntry> m_spaces;
    bool m_chLoaded = false;

    std::mutex m_ctrlMtx;
    bool m_isOpen = false;
    std::atomic<bool> m_signalLocked{false};
    ChannelEntry m_activeCh;
    DWORD m_curSpace = 0;
    DWORD m_curChannel = 0;

    HANDLE m_tsEvent = nullptr;
    std::mutex m_queueMtx;
    std::deque<std::vector<BYTE>> m_tsQueue;
    size_t m_queuedBytes = 0;
    std::vector<BYTE> m_lastChunk;

    std::atomic<bool> m_streamStop{false};
    std::atomic<SOCKET> m_streamSock{INVALID_SOCKET};
    std::atomic<uint64_t> m_bytesReceived{0};
    std::thread m_streamThread;
};

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver() {
    return new BonDriverWebOS();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
