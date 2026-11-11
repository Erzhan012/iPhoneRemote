#include "DeviceInfo.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>

// IOKit has no public headers on iOS: declare the four calls we need
extern "C" {
CFMutableDictionaryRef IOServiceMatching(const char *name);
mach_port_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IORegistryEntryCreateCFProperties(mach_port_t entry, CFMutableDictionaryRef *props,
                                                CFAllocatorRef alloc, uint32_t options);
kern_return_t IOObjectRelease(mach_port_t obj);
}

namespace {

std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
std::string q(const std::string &s) { return "\"" + esc(s) + "\""; }
const char *tf(bool b) { return b ? "true" : "false"; }

std::string sysstr(const char *name) {
    size_t l = 0;
    if (sysctlbyname(name, nullptr, &l, nullptr, 0) || !l) return "";
    std::string s(l, '\0');
    if (sysctlbyname(name, &s[0], &l, nullptr, 0)) return "";
    s.resize(strlen(s.c_str()));
    return s;
}
template <class T> bool sysnum(const char *name, T &v) {
    size_t l = sizeof v;
    return sysctlbyname(name, &v, &l, nullptr, 0) == 0;
}

bool dnum(CFDictionaryRef d, const char *k, long long &out) {
    CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, key);
    CFRelease(key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
    return CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &out);
}
bool dbool(CFDictionaryRef d, const char *k, bool &out) {
    CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, key);
    CFRelease(key);
    if (!v || CFGetTypeID(v) != CFBooleanGetTypeID()) return false;
    out = CFBooleanGetValue((CFBooleanRef)v);
    return true;
}

bool cpuload(host_cpu_load_info_data_t &o) {
    mach_msg_type_number_t c = HOST_CPU_LOAD_INFO_COUNT;
    return host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&o, &c) == KERN_SUCCESS;
}

bool reach() {  // TCP connect to 1.1.1.1:443, 1.5s budget
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(443);
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    bool ok = false;
    int r = connect(fd, (sockaddr *)&a, sizeof a);
    if (r == 0) ok = true;
    else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, 1500) > 0) {
            int e = 0;
            socklen_t l = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
            ok = (e == 0);
        }
    }
    close(fd);
    return ok;
}

}  // namespace

namespace dev {

std::string json_info() {
    static const std::map<std::string, std::string> kModels = {
        {"iPhone8,4", "iPhone SE (1st gen)"}, {"iPhone8,1", "iPhone 6s"},
        {"iPhone8,2", "iPhone 6s Plus"},      {"iPhone9,1", "iPhone 7"},
        {"iPhone9,3", "iPhone 7"},            {"iPhone9,2", "iPhone 7 Plus"},
        {"iPhone9,4", "iPhone 7 Plus"}};
    std::string id = sysstr("hw.machine");
    auto it = kModels.find(id);
    std::string model = it != kModels.end() ? it->second : id;
    std::string ios = sysstr("kern.osproductversion");
    if (ios.empty()) ios = sysstr("kern.osrelease");
    int sub = 0, cores = 0;
    sysnum("hw.cpusubtype", sub);
    sysnum("hw.ncpu", cores);
    struct statfs fs{};
    unsigned long long free_b = 0, total_b = 0;
    if (statfs("/var", &fs) == 0) {
        free_b = (unsigned long long)fs.f_bavail * fs.f_bsize;
        total_b = (unsigned long long)fs.f_blocks * fs.f_bsize;
    }
    std::ostringstream o;
    o << "{\"model\":" << q(model) << ",\"model_id\":" << q(id) << ",\"ios\":" << q(ios)
      << ",\"build\":" << q(sysstr("kern.osversion")) << ",\"arch\":" << q(sub == 2 ? "arm64e" : "arm64")
      << ",\"cores\":" << cores << ",\"disk_free\":" << free_b << ",\"disk_total\":" << total_b << "}";
    return o.str();
}

std::string json_battery() {
    mach_port_t s = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
    if (!s) return "{\"error\":\"no power source\"}";
    CFMutableDictionaryRef p = nullptr;
    if (IORegistryEntryCreateCFProperties(s, &p, kCFAllocatorDefault, 0) != KERN_SUCCESS || !p) {
        IOObjectRelease(s);
        return "{\"error\":\"battery read failed\"}";
    }
    long long cur = 0, mx = 100, cyc = -1, temp = -1, volt = -1;
    bool chg = false, ext = false, full = false;
    dnum(p, "CurrentCapacity", cur);  // percent on iOS
    dnum(p, "MaxCapacity", mx);
    dnum(p, "CycleCount", cyc);
    dnum(p, "Temperature", temp);  // 0.01 C
    dnum(p, "Voltage", volt);
    dbool(p, "IsCharging", chg);
    dbool(p, "ExternalConnected", ext);
    dbool(p, "FullyCharged", full);
    CFRelease(p);
    IOObjectRelease(s);
    int pct = mx > 0 ? (int)(cur * 100 / mx) : (int)cur;
    std::ostringstream o;
    o << "{\"percent\":" << pct << ",\"charging\":" << tf(chg) << ",\"plugged\":" << tf(ext)
      << ",\"full\":" << tf(full) << ",\"cycles\":" << cyc << ",\"temp\":" << temp
      << ",\"voltage\":" << volt << "}";
    return o.str();
}

std::string json_system() {
    unsigned long long total = 0;
    sysnum("hw.memsize", total);
    vm_size_t ps = 0;
    host_page_size(mach_host_self(), &ps);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    unsigned long long used = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &c) == KERN_SUCCESS)
        used = (unsigned long long)(vm.active_count + vm.wire_count + vm.compressor_page_count) * ps;

    double pct = 0;
    host_cpu_load_info_data_t a{}, b{};
    if (cpuload(a)) {
        usleep(250000);
        if (cpuload(b)) {
            double busy = 0, all = 0;
            for (int i = 0; i < CPU_STATE_MAX; i++) {
                double d = (double)b.cpu_ticks[i] - a.cpu_ticks[i];
                all += d;
                if (i != CPU_STATE_IDLE) busy += d;
            }
            if (all > 0) pct = 100.0 * busy / all;
        }
    }
    char cb[16];
    snprintf(cb, sizeof cb, "%.1f", pct);
    std::ostringstream o;
    o << "{\"ram_total\":" << total << ",\"ram_used\":" << used << ",\"cpu_pct\":" << cb << "}";
    return o.str();
}

std::string json_network() {
    bool wifi = false, cell = false;
    std::string ip;
    ifaddrs *l = nullptr;
    if (getifaddrs(&l) == 0) {
        for (auto *a = l; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
            if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_RUNNING)) continue;
            if (!strcmp(a->ifa_name, "en0")) {
                wifi = true;
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((sockaddr_in *)a->ifa_addr)->sin_addr, buf, sizeof buf);
                ip = buf;
            } else if (!strncmp(a->ifa_name, "pdp_ip", 6)) cell = true;
        }
        freeifaddrs(l);
    }
    std::ostringstream o;
    o << "{\"wifi\":" << tf(wifi) << ",\"ip\":" << q(ip) << ",\"cellular\":" << tf(cell)
      << ",\"internet\":" << tf(reach()) << "}";
    return o.str();
}

std::string json_uptime() {
    timeval bt{};
    size_t l = sizeof bt;
    long long up = 0;
    if (sysctlbyname("kern.boottime", &bt, &l, nullptr, 0) == 0) up = (long long)time(nullptr) - bt.tv_sec;
    return "{\"uptime\":" + std::to_string(up) + "}";
}

std::string json_all() {
    return "{\"info\":" + json_info() + ",\"battery\":" + json_battery() + ",\"system\":" + json_system() +
           ",\"network\":" + json_network() + ",\"uptime\":" + json_uptime() + "}";
}

}  // namespace dev