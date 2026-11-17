#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")

// MAC のハードコード既定値は持たない。
// MAC は引数、または BonDriver_WebOS.ini の WakeMac / LastMac / LastEthMac / LastWlanMac から取得する。

static bool parseMac(const char *str, uint8_t mac[6]) {
    int v[6];
    if (sscanf_s(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 ||
        sscanf_s(str, "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(v[i]);
        return true;
    }
    if (strlen(str) == 12) {
        for (int i = 0; i < 6; ++i) {
            char buf[3] = { str[i * 2], str[i * 2 + 1], 0 };
            mac[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
        }
        return true;
    }
    return false;
}

static void sendMagicPacket(SOCKET u, const uint8_t mac[6], uint32_t bcastAddr, uint16_t port) {
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = bcastAddr;
    sendto(u, reinterpret_cast<const char *>(pkt), sizeof(pkt), 0,
           reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
}

static std::vector<uint32_t> getBroadcastAddresses() {
    std::vector<uint32_t> out;
    out.push_back(INADDR_BROADCAST); // 255.255.255.255

    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    ULONG ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size);
    }
    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family == AF_INET) {
                    auto *sin = reinterpret_cast<struct sockaddr_in *>(u->Address.lpSockaddr);
                    uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    ULONG mask = 0;
                    if (ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask) == NO_ERROR) {
                        uint32_t hostMask = ntohl(mask);
                        uint32_t bcast = (ip & hostMask) | (~hostMask);
                        out.push_back(htonl(bcast));
                    }
                }
            }
        }
    }
    return out;
}

static bool checkTcpPort(const char *ip, int port, int timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip, &dst.sin_addr);

    connect(s, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int res = select(0, nullptr, &wset, nullptr, &tv);
    closesocket(s);
    return res > 0;
}

int main(int argc, char *argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    bool doWait = false;
    bool doOpen = false;
    std::string customMac;
    std::string customIp;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" || arg == "--wait") {
            doWait = true;
        } else if (arg == "-o" || arg == "--open") {
            doOpen = true;
            doWait = true;
        } else if (arg == "-h" || arg == "--help" || arg == "/?") {
            printf("webOS DTV Tuner Bridge - Wake-on-LAN ユーティリティ\n");
            printf("使用法: wake_tv [MAC または IP] [オプション]\n\n");
            printf("オプション:\n");
            printf("  -w, --wait  TV の起動（ポート疎通）を待機します（最大 15 秒）\n");
            printf("  -o, --open  TV の起動後、既定ブラウザで WebUI (ポート :40772) を開きます\n");
            printf("  -h, --help  このヘルプを表示します\n\n");
            printf("MAC アドレスは引数で直接指定するか、同フォルダまたは TVTest フォルダの\n");
            printf("BonDriver_WebOS.ini から自動取得されます。\n");
            return 0;
        } else if (arg.find(':') != std::string::npos || arg.find('-') != std::string::npos || (arg.length() == 12 && strspn(arg.c_str(), "0123456789ABCDEFabcdef") == 12)) {
            customMac = arg;
        } else {
            customIp = arg;
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "エラー: WSAStartup に失敗しました\n");
        return 1;
    }

    // INI ファイルの検索パス (exe フォルダ -> カレント -> x64 -> x86 -> 親フォルダ -> C:\TVTest)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t *lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    std::wstring exeDir = exePath;

    std::vector<std::wstring> candidateInis = {
        exeDir + L"BonDriver_WebOS.ini",
        L"BonDriver_WebOS.ini",
        exeDir + L"x64\\BonDriver_WebOS.ini",
        exeDir + L"x86\\BonDriver_WebOS.ini",
        exeDir + L"..\\BonDriver_WebOS.ini",
        L"C:\\TVTest\\BonDriver_WebOS.ini"
    };

    std::wstring validIni;
    for (const auto &p : candidateInis) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            validIni = p;
            break;
        }
    }

    std::vector<std::string> macList;
    std::string tvIp = customIp;

    if (!customMac.empty()) {
        macList.push_back(customMac);
    } else if (!validIni.empty()) {
        auto addIniMac = [&](const wchar_t *key) {
            wchar_t buf[256];
            if (GetPrivateProfileStringW(L"Server", key, L"", buf, 256, validIni.c_str()) && buf[0]) {
                char m[256];
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, m, sizeof(m), nullptr, nullptr);
                std::string s(m);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
                size_t p = 0;
                while (p < s.length() && (s[p] == ' ' || s[p] == '\t')) p++;
                s = s.substr(p);
                if (!s.empty()) macList.push_back(s);
            }
        };

        addIniMac(L"WakeMac");
        addIniMac(L"LastMac");
        addIniMac(L"LastEthMac");
        addIniMac(L"LastWlanMac");

        if (tvIp.empty()) {
            wchar_t buf[256];
            if (GetPrivateProfileStringW(L"Server", L"Host", L"", buf, 256, validIni.c_str()) && buf[0]) {
                if (wcscmp(buf, L"auto") != 0) {
                    char ip[256];
                    WideCharToMultiByte(CP_UTF8, 0, buf, -1, ip, sizeof(ip), nullptr, nullptr);
                    tvIp = ip;
                }
            }
            if (tvIp.empty() && GetPrivateProfileStringW(L"Server", L"LastIp", L"", buf, 256, validIni.c_str()) && buf[0]) {
                char ip[256];
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, ip, sizeof(ip), nullptr, nullptr);
                tvIp = ip;
            }
        }
    }

    // MAC が見つからない場合は送信せずエラー終了する（ハードコードのフォールバックは持たない）
    if (macList.empty()) {
        fprintf(stderr,
                "エラー: MAC アドレスが見つかりませんでした。\n"
                "  BonDriver_WebOS.ini に LastMac / LastEthMac が記録された状態で、\n"
                "  一度 BonDriver_WebOS を起動してから再実行するか、\n"
                "  wake_tv.exe <MACアドレス> -w  のように MAC を引数で指定してください。\n");
        WSACleanup();
        return 1;
    }

    // 重複除去
    std::vector<std::string> uniqMacs;
    for (const auto &m : macList) {
        uint8_t dummy[6];
        if (parseMac(m.c_str(), dummy)) {
            if (std::find(uniqMacs.begin(), uniqMacs.end(), m) == uniqMacs.end()) {
                uniqMacs.push_back(m);
            }
        }
    }

    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) {
        fprintf(stderr, "エラー: UDP ソケット作成に失敗しました\n");
        WSACleanup();
        return 1;
    }
    BOOL bcast = TRUE;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&bcast), sizeof(bcast));

    auto bcastAddrs = getBroadcastAddresses();
    for (const auto &mstr : uniqMacs) {
        uint8_t mac[6];
        if (!parseMac(mstr.c_str(), mac)) continue;
        for (uint32_t ba : bcastAddrs) {
            sendMagicPacket(u, mac, ba, 9);
            sendMagicPacket(u, mac, ba, 7);
        }
        printf("[WOL] Magic Packet を送信しました -> %s\n", mstr.c_str());
    }
    closesocket(u);

    if (doWait) {
        if (tvIp.empty()) {
            printf("[WOL] TV の起動を待機中 (約 3 秒)...\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        } else {
            printf("[WOL] TV (%s:40772) の起動を待機中...", tvIp.c_str());
            fflush(stdout);
            bool online = false;
            for (int i = 0; i < 30; ++i) { // 最大 15 秒
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (checkTcpPort(tvIp.c_str(), 40772, 300)) {
                    online = true;
                    break;
                }
                printf(".");
                fflush(stdout);
            }
            if (online) {
                printf(" 起動を確認しました！ (WebUI 準備完了)\n");
            } else {
                printf(" タイムアウト (TV が完全シャットダウンの場合は起動に約 20 秒かかります)\n");
            }
        }
    }

    if (doOpen) {
        if (tvIp.empty()) {
            fprintf(stderr,
                    "エラー: TV の IP アドレスが不明なため WebUI を開けません。\n"
                    "  BonDriver_WebOS.ini の [Server] Host または LastIp を設定するか、\n"
                    "  wake_tv.exe <TVのIP> -o  のように IP を引数で指定してください。\n");
        } else {
            std::string url = "http://" + tvIp + ":40772/";
            printf("[WOL] ブラウザで WebUI を開きます: %s\n", url.c_str());
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    WSACleanup();
    return 0;
}
