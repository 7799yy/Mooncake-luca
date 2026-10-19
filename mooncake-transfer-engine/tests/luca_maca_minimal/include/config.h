#pragma once
namespace mooncake {
struct GlobalConfig {
    int slice_timeout = 0;
    bool trace = false;
};
inline GlobalConfig& globalConfig() {
    static GlobalConfig config;
    return config;
}
}  // namespace mooncake
