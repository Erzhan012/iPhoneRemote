#pragma once
#include <string>

// every function returns a JSON object as a string
namespace dev {
std::string json_info();     // model, iOS, build, arch, cores, disk
std::string json_battery();  // percent, charging, plugged, cycles, temp
std::string json_system();   // RAM + CPU load
std::string json_network();  // wifi, ip, cellular, internet
std::string json_uptime();   // seconds since boot
std::string json_all();
}
