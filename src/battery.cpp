#include <spdlog/spdlog.h>
#include <filesystem.h>
#include "battery.h"

namespace fs = ghc::filesystem;
using namespace std;

void BatteryStats::numBattery() {
    int batteryCount = 0;
    
    try {
        if (!fs::exists("/sys/class/power_supply/")) {
             batt_count = 0;
             batt_check = true;
             return;
        }
        
        fs::path path("/sys/class/power_supply/");
        for (auto& p : fs::directory_iterator(path)) {
            string fileName = p.path().filename();
            // 支持 BAT0, BAT1, battery (Android) 等路径
            if (fileName.find("BAT") != std::string::npos ||
                fileName == "battery") {
                battPath[batteryCount] = p.path();
                batteryCount += 1;
                if (batteryCount >= 2) break; // 最多支持2个电池
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("Battery detection failed: {}", ex.what());
        batteryCount = 0;
    }
    
    batt_count = batteryCount;
    batt_check = true;
}

void BatteryStats::update() {
    if (!batt_check) {
        numBattery();
        if (batt_count == 0) {
            SPDLOG_DEBUG("No battery found");
        }
    }

    if (batt_count > 0) {
        current_watt = getPower();
        current_percent = getPercent();
        remaining_time = getTimeRemaining();
    }
}

float BatteryStats::getPercent()
{
    float charge_n = 0;
    float charge_f = 0;
    
    for(int i = 0; i < batt_count; i++) {
        try {
            string syspath = battPath[i];
            string charge_now = syspath + "/charge_now";
            string charge_full = syspath + "/charge_full";
            string energy_now = syspath + "/energy_now";
            string energy_full = syspath + "/energy_full";
            string capacity = syspath + "/capacity";

            if (fs::exists(charge_now)) {
                std::ifstream input(charge_now);
                std::string line;
                if(std::getline(input, line)) {
                    charge_n += (stof(line) / 1000000);
                }
                std::ifstream input2(charge_full);
                if(std::getline(input2, line)) {
                    charge_f += (stof(line) / 1000000);
                }
            }
            else if (fs::exists(energy_now)) {
                std::ifstream input(energy_now);
                std::string line;
                if(std::getline(input, line)) {
                    charge_n += (stof(line) / 1000000);
                }
                std::ifstream input2(energy_full);
                if(std::getline(input2, line)) {
                    charge_f += (stof(line) / 1000000);
                }
            }
            else {
                std::ifstream input(capacity);
                std::string line;
                if(std::getline(input, line)) {
                    charge_n += stof(line) / 100;
                    charge_f = batt_count;
                }
            }
        } catch (const std::exception& ex) {
            SPDLOG_WARN("Battery percent read error: {}", ex.what());
        }
    }
    
    if (charge_f <= 0) return 0.0f;
    return (charge_n / charge_f) * 100;
}

float BatteryStats::getPower() {
    float power_w = 0.0f;

    for (int i = 0; i < batt_count; i++) {
        try {
            string syspath = battPath[i];
            string current_now = syspath + "/current_now";
            string voltage_now = syspath + "/voltage_now";
            string power_now = syspath + "/power_now";
            string status = syspath + "/status";

            {
                std::ifstream input(status);
                std::string line;
                if (std::getline(input, line)) {
                    current_status = line;
                    state[i] = current_status;
                }
            }

            // Prefer power_now (µW) when available.
            if (fs::exists(power_now)) {
                std::ifstream input(power_now);
                std::string line;
                if (std::getline(input, line)) {
                    power_w += std::fabs(stof(line)) / 1000000.0f;
                }
                continue;
            }

            if (fs::exists(current_now) && fs::exists(voltage_now)) {
                float i_ua = 0.0f;
                float v_uv = 0.0f;

                {
                    std::ifstream input(current_now);
                    std::string line;
                    if (std::getline(input, line)) {
                        i_ua = stof(line);
                    }
                }
                {
                    std::ifstream input(voltage_now);
                    std::string line;
                    if (std::getline(input, line)) {
                        v_uv = stof(line);
                    }
                }

                power_w += (std::fabs(i_ua) * std::fabs(v_uv)) * 1e-12f;
            }
        } catch (const std::exception& ex) {
            SPDLOG_WARN("Battery power read error: {}", ex.what());
        }
    }

    return power_w;
}

float BatteryStats::getTimeRemaining() {
    float current = 0.0f;
    float charge = 0.0f;

    for (int i = 0; i < batt_count; i++) {
        try {
            string syspath = battPath[i];
            string current_now = syspath + "/current_now";
            string charge_now = syspath + "/charge_now";
            string energy_now = syspath + "/energy_now";
            string voltage_now = syspath + "/voltage_now";
            string power_now = syspath + "/power_now";

            if (fs::exists(current_now)) {
                std::ifstream input(current_now);
                std::string line;
                if (std::getline(input, line)) {
                    current_now_vec.push_back(std::fabs(stof(line)));
                }
            } else if (fs::exists(power_now) && fs::exists(voltage_now)) {
                float voltage = 0.0f;
                float power = 0.0f;

                {
                    std::ifstream input_voltage(voltage_now);
                    std::string line;
                    if (std::getline(input_voltage, line)) {
                        voltage = stof(line);
                    }
                }
                {
                    std::ifstream input_power(power_now);
                    std::string line;
                    if (std::getline(input_power, line)) {
                        power = stof(line);
                    }
                }

                if (voltage > 0.0f) {
                    current_now_vec.push_back(std::fabs(power) / voltage);
                }
            }

            if (fs::exists(charge_now)) {
                std::ifstream input(charge_now);
                std::string line;
                if (std::getline(input, line)) {
                    charge += stof(line);
                }
            } else if (fs::exists(energy_now) && fs::exists(voltage_now)) {
                float energy = 0.0f;
                float voltage = 0.0f;

                {
                    std::ifstream input_energy(energy_now);
                    std::string line;
                    if (std::getline(input_energy, line)) {
                        energy = stof(line);
                    }
                }
                {
                    std::ifstream input_voltage(voltage_now);
                    std::string line;
                    if (std::getline(input_voltage, line)) {
                        voltage = stof(line);
                    }
                }

                if (voltage > 0.0f) {
                    charge += energy / voltage;
                }
            }

            if (current_now_vec.size() > 25) {
                current_now_vec.erase(current_now_vec.begin());
            }
        } catch (const std::exception& ex) {
            SPDLOG_WARN("Battery time remaining read error: {}", ex.what());
        }
    }

    if (current_now_vec.empty()) {
        return 0.0f;
    }

    for (const auto& current_now_sample : current_now_vec) {
        current += current_now_sample;
    }
    current /= static_cast<float>(current_now_vec.size());

    if (current <= 0.0f) {
        return 0.0f;
    }

    return charge / current;
}

BatteryStats Battery_Stats;
