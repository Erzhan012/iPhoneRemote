// iphoneremoted: token-auth HTTP API for device info + confirmed reboot/shutdown
// no shell, no arbitrary exec. fixed routes only.
#include "DeviceInfo.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

static std::string gToken;
static int gPort = 8787;
static std::string gBind = "0.0.0.0";
static std::string gConfirm = "none";  // none | device (CFUserNotification alert on the phone)
static std::atomic<int> gActive{0};

static void logmsg(const char *fmt, ...) {
    time_t t = time(nullptr);
    char ts[32];
    strftime(ts, sizeof ts, "%F %T", localtime(&t));
    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static bool loadConf(const char *path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto e = line.find('=');
        if (e == std::string::npos || line[0] == '#') continue;
        std::string k = line.substr(0, e), v = line.substr(e + 1);
        if (k == "token") gToken = v;
        else if (k == "port") gPort = atoi(v.c_str());
        else if (k == "bind") gBind = v;
        else if (k == "confirm") gConfirm = v;
    }
    return gToken.size() >= 16;
}

// ---- rate limit: 20 req / 10s, 5 bad tokens -> 60s lockout
static std::mutex gMu;
static std::deque<long long> gHits;
static long long gLockUntil = 0;
static int gFails = 0;
static long long nowms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static bool allow() {
    std::lock_guard<std::mutex> l(gMu);
    long long n = nowms();
    if (n < gLockUntil) return false;
    while (!gHits.empty() && n - gHits.front() > 10000) gHits.pop_front();
    if (gHits.size() >= 20) return false;
    gHits.push_back(n);
    return true;
}
static void authFail() {
    std::lock_guard<std::mutex> l(gMu);
    if (++gFails >= 5) { gLockUntil = nowms() + 60000; gFails = 0; }
}
static void authOk() {
    std::lock_guard<std::mutex> l(gMu);
    gFails = 0;
}

static bool eq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static void sendAll(int fd, const std::string &s) {
    size_t o = 0;
    while (o < s.size()) {
        ssize_t n = send(fd, s.data() + o, s.size() - o, 0);
        if (n <= 0) return;
        o += (size_t)n;
    }
}
static void reply(int fd, int code, const char *txt, const std::string &body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + txt +
                    "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n";
    sendAll(fd, h + body);
}
static std::string err(const char *m) { return std::string("{\"error\":\"") + m + "\"}"; }

static std::string header(const std::string &req, const char *lname) {
    std::string low = req;
    for (auto &c : low) c = (char)tolower((unsigned char)c);
    std::string key = std::string("\r\n") + lname + ":";
    size_t p = low.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = req.find("\r\n", p);
    std::string v = req.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    return v;
}

// alert on the device. Cancel is the DEFAULT button, Confirm the alternate:
// timeout / dismiss / stray tap can never confirm.
static bool confirmOnDevice(const char *msg) {
    CFStringRef m = CFStringCreateWithCString(nullptr, msg, kCFStringEncodingUTF8);
    CFOptionFlags resp = 0;
    SInt32 rc = CFUserNotificationDisplayAlert(30, kCFUserNotificationCautionAlertLevel, nullptr, nullptr,
                                               nullptr, CFSTR("iPhoneRemote"), m, CFSTR("Cancel"),
                                               CFSTR("Confirm"), nullptr, &resp);
    CFRelease(m);
    logmsg("alert rc=%d resp=%lu", (int)rc, (unsigned long)(resp & 0x3));
    return rc == 0 && (resp & 0x3) == kCFUserNotificationAlternateResponse;
}

static std::mutex gConfirmMu;
static void power(int fd, bool doReboot) {
    std::unique_lock<std::mutex> lk(gConfirmMu, std::try_to_lock);
    if (!lk.owns_lock()) { reply(fd, 409, "Conflict", err("confirmation already pending")); return; }
    if (gConfirm == "device") {
        bool ok = confirmOnDevice(doReboot ? "Remote request: REBOOT this iPhone?"
                                           : "Remote request: SHUT DOWN this iPhone?");
        if (!ok) {
            logmsg("%s declined/timeout on device", doReboot ? "reboot" : "shutdown");
            reply(fd, 403, "Forbidden", err("declined or timed out on device"));
            return;
        }
    }
    logmsg("%s confirmed", doReboot ? "reboot" : "shutdown");
    reply(fd, 200, "OK", "{\"ok\":true}");
    sync();
    sleep(1);
    // dlsym: header availability of reboot() varies between SDKs. RB_AUTOBOOT=0, RB_HALT=0x08
    auto fn = (int (*)(int))dlsym(RTLD_DEFAULT, "reboot");
    if (fn) fn(doReboot ? 0 : 0x08);
    logmsg("reboot() returned, did nothing");
}

static void serve(int fd, const std::string &peer) {
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[2048];
    while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
    }
    char m[8] = {0}, p[128] = {0};
    if (sscanf(req.c_str(), "%7s %127s", m, p) != 2) { reply(fd, 400, "Bad Request", err("bad request")); return; }
    if (!allow()) { reply(fd, 429, "Too Many Requests", err("rate limited")); return; }
    if (!eq(header(req, "x-token"), gToken)) {
        authFail();
        logmsg("401 %s %s %s", peer.c_str(), m, p);
        reply(fd, 401, "Unauthorized", err("unauthorized"));
        return;
    }
    authOk();
    std::string method = m, path = p;
    logmsg("%s %s %s", peer.c_str(), m, p);

    if (method == "GET") {
        if (path == "/ping") reply(fd, 200, "OK", "{\"ok\":true}");
        else if (path == "/info") reply(fd, 200, "OK", dev::json_info());
        else if (path == "/battery") reply(fd, 200, "OK", dev::json_battery());
        else if (path == "/system") reply(fd, 200, "OK", dev::json_system());
        else if (path == "/network") reply(fd, 200, "OK", dev::json_network());
        else if (path == "/uptime") reply(fd, 200, "OK", dev::json_uptime());
        else if (path == "/all") reply(fd, 200, "OK", dev::json_all());
        else reply(fd, 404, "Not Found", err("not found"));
    } else if (method == "POST" && (path == "/reboot" || path == "/shutdown")) {
        power(fd, path == "/reboot");
    } else {
        reply(fd, 404, "Not Found", err("not found"));
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *cp = argc > 1 ? argv[1] : "/var/jb/etc/iphoneremote.conf";
    if (!loadConf(cp)) { logmsg("bad config %s (token must be >= 16 chars)", cp); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)gPort);
    if (inet_pton(AF_INET, gBind.c_str(), &a.sin_addr) != 1) { logmsg("bad bind addr"); return 1; }
    if (bind(s, (sockaddr *)&a, sizeof a) < 0 || listen(s, 16) < 0) { logmsg("bind/listen failed"); return 1; }
    logmsg("listening on %s:%d", gBind.c_str(), gPort);
    logmsg("iphoneremoted v0.1.1 confirm=%s", gConfirm.c_str());

    for (;;) {
        sockaddr_in c{};
        socklen_t cl = sizeof c;
        int fd = accept(s, (sockaddr *)&c, &cl);
        if (fd < 0) continue;
        if (gActive >= 8) { close(fd); continue; }
        gActive++;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &c.sin_addr, ip, sizeof ip);
        std::thread([fd, peer = std::string(ip)] {
            serve(fd, peer);
            close(fd);
            gActive--;
        }).detach();
    }
}
