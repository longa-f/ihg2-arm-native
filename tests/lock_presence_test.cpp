#include "ihg2_arm/lock_presence.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace ihg2::arm::presence;

int main() {
    const auto parsed = parse_mac("02:00:00:00:00:36");
    assert(parsed);
    assert(format_mac(*parsed) == "02:00:00:00:00:36");
    assert(!parse_mac("02:00:00:00:00"));
    assert(!parse_mac("02-00-00-00-00-36"));
    assert(!make_config("127.0.0.1", "02:00:00:00:00:36"));
    assert(!make_config("224.0.0.1", "02:00:00:00:00:36"));
    assert(!make_config("192.0.2.36", "01:00:5e:00:00:01"));
    const auto config = make_config("192.0.2.36", "02:00:00:00:00:36");
    assert(config);

    char directory[] = "/tmp/ihg2-lock-presence-test-XXXXXX";
    assert(::mkdtemp(directory));
    const std::string config_dir = std::string(directory) + "/config";
    assert(::mkdir(config_dir.c_str(), 0700) == 0);
    assert(!save_config(directory, "192.0.2.36", "01:00:5e:00:00:01"));
    assert(save_config(directory, "192.0.2.36", "02:00:00:00:00:36"));
    const auto loaded = load_config(directory);
    assert(loaded && *loaded == *config);

    struct stat st{};
    const std::string file = config_dir + "/lock-sensor.conf";
    assert(::stat(file.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    std::ifstream input(file);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(contents == "ip=192.0.2.36\nmac=02:00:00:00:00:36\n");

    assert(::chmod(file.c_str(), 0644) == 0);
    bool unsafe_rejected = false;
    try { (void)load_config(directory); }
    catch (...) { unsafe_rejected = true; }
    assert(unsafe_rejected);
    assert(::chmod(file.c_str(), 0600) == 0);

    const auto origin = std::chrono::steady_clock::time_point{};
    Detector detector;
    assert(!detector.online());
    assert(detector.observe(true, origin));
    assert(detector.online());
    assert(!detector.observe(false, origin + std::chrono::seconds(5)));
    assert(!detector.expire(origin + std::chrono::seconds(14), std::chrono::seconds(15)));
    assert(detector.expire(origin + std::chrono::seconds(15), std::chrono::seconds(15)));
    assert(!detector.online());
    assert(!detector.observe(false, origin + std::chrono::seconds(16)));
    assert(detector.observe(true, origin + std::chrono::seconds(17)));

    assert(::unlink(file.c_str()) == 0);
    assert(::rmdir(config_dir.c_str()) == 0);
    assert(::rmdir(directory) == 0);
    puts("lock presence tests passed");
}
