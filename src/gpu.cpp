#include "gpu.h"
#include "file_utils.h"
#include "hud_elements.h"
#include "overlay_params.h"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <thread>
#include <chrono>

namespace fs = ghc::filesystem;

GPUS::GPUS(const overlay_params* early_params) {
    std::set<std::string> gpu_entries;
    auto params = early_params ? early_params : get_params().get();

    try {
        if (fs::exists("/sys/class/drm")) {
            for (const auto& entry : fs::directory_iterator("/sys/class/drm")) {
                if (!entry.is_directory())
                    continue;

                std::string node_name = entry.path().filename().string();

                // Check if the directory is a render node (e.g., renderD128, renderD129, etc.)
                if (node_name.find("renderD") == 0 && node_name.length() > 7) {
                    // Ensure the rest of the string after "renderD" is numeric
                    std::string render_number = node_name.substr(7);
                    if (std::all_of(render_number.begin(), render_number.end(), ::isdigit)) {
                        gpu_entries.insert(node_name);  // Store the render entry
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& ex) {
        SPDLOG_WARN("Failed to access /sys/class/drm: {}. GPU detection disabled.", ex.what());
    } catch (const std::exception& ex) {
        SPDLOG_WARN("Error during GPU detection: {}. GPU detection disabled.", ex.what());
    }

    // Now process the sorted GPU entries
    uint8_t idx = 0, total_active = 0;

    for (const auto& node_name : gpu_entries) {
        const std::string driver = get_driver(node_name);

        if (driver.empty()) {
            SPDLOG_DEBUG("Failed to query driver name of node \"{}\"", node_name);
            continue;
        }

        {
            const std::string* d =
                std::find(std::begin(supported_drivers), std::end(supported_drivers), driver);

            if (d == std::end(supported_drivers)) {
                SPDLOG_WARN(
                    "node \"{}\" is using driver \"{}\" which is unsupported by MangoHud. Skipping...",
                    node_name, driver
                );
                continue;
            }
        }

        std::string path = "/sys/class/drm/" + node_name;
        std::string device_address = get_pci_device_address(path);  // Store the result
        const char* pci_dev = device_address.c_str();

        uint32_t vendor_id = 0;
        uint32_t device_id = 0;

        if (!device_address.empty())
        {
            try {
                vendor_id = std::stoul(read_line("/sys/bus/pci/devices/" + device_address + "/vendor"), nullptr, 16);
            } catch(...) {
                SPDLOG_ERROR("stoul failed on: {}", "/sys/bus/pci/devices/" + device_address + "/vendor");
            }

            try {
                device_id = std::stoul(read_line("/sys/bus/pci/devices/" + device_address + "/device"), nullptr, 16);
            } catch (...) {
                SPDLOG_ERROR("stoul failed on: {}", "/sys/bus/pci/devices/" + device_address + "/device");
            }
        }

        std::shared_ptr<GPU> ptr =
            std::make_shared<GPU>(node_name, vendor_id, device_id, pci_dev, driver);

        if (params->gpu_list.size() == 1 && params->gpu_list[0] == idx++)
            ptr->is_active = true;

        if (!params->pci_dev.empty() && pci_dev == params->pci_dev)
            ptr->is_active = true;

        available_gpus.emplace_back(ptr);

        SPDLOG_DEBUG(
            "GPU Found: node_name: {}, driver: {}, vendor_id: {:x} device_id: {:x} pci_dev: {}",
            node_name, driver, vendor_id, device_id, pci_dev
        );

        if (ptr->is_active) {
            SPDLOG_INFO(
                "Set {} as active GPU (driver={} id={:x}:{:x} pci_dev={})",
                node_name, driver, vendor_id, device_id, pci_dev
            );
            total_active++;
        }
    }

    // 注入 Adreno GPU（如果 drm 没有检测到任何 GPU）
    {
        bool already_added = false;
        for (auto& g : available_gpus) {
            if (g->driver == "adreno" || g->driver == "freedreno" || 
                g->driver == "msm_drm" || g->driver == "msm_dpu") {
                already_added = true;
                break;
            }
        }

        if (!already_added) {
            auto adreno = std::make_shared<GPU>("Adreno", 0x5143, 0, "0000:00:00.0", "adreno");
            
            if (total_active == 0) {
                adreno->is_active = true;
                total_active++;
            }
            available_gpus.emplace_back(adreno);

            // 启动后台线程监控 GPU 使用率
            // 使用持久文件流 + weak_ptr，避免频繁 open/close 和线程泄漏
            std::thread([weak = std::weak_ptr<GPU>(adreno)](){
                std::ifstream load_stream;
                std::ifstream temp_stream;
                std::ifstream freq_stream;
                int thermal_zone = -1;
                bool use_gpubusy = false;
                bool initialized = false;

                while (true) {
                    auto gpu = weak.lock();
                    if (!gpu)
                        return; // GPU 对象已销毁，退出线程

                    if (!initialized) {
                        initialized = true;

                        // 尝试 gpu_busy_percentage（直接返回百分比）
                        load_stream.open("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
                        if (!load_stream.is_open())
                            load_stream.open("/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load");

                        // 尝试 gpubusy（格式: "busy_time total_time"）
                        if (!load_stream.is_open()) {
                            load_stream.open("/sys/class/kgsl/kgsl-3d0/gpubusy");
                            if (load_stream.is_open())
                                use_gpubusy = true;
                        }

                        // load 流无法打开 → 标记 N/A（仅初始化时）
                        if (!load_stream.is_open())
                            gpu->metrics.load = -1;

                        // 温度
                        temp_stream.open("/sys/class/kgsl/kgsl-3d0/temp");
                        if (!temp_stream.is_open()) {
                            // 回退: 搜索 thermal_zone 中含 "gpuss" 的区域
                            for (int i = 0; i < 20; i++) {
                                std::string type_path = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/type";
                                std::ifstream type_stream(type_path);
                                std::string type;
                                if (type_stream.is_open() && std::getline(type_stream, type)) {
                                    if (type.find("gpuss") != std::string::npos) {
                                        thermal_zone = i;
                                        break;
                                    }
                                }
                            }
                        }

                        // 频率
                        const char* freq_paths[] = {
                            "/sys/class/kgsl/kgsl-3d0/gpuclk",
                            "/sys/class/kgsl/kgsl-3d0/cur_freq",
                            "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
                            "/sys/kernel/gpu/gpu_clock",
                            nullptr
                        };
                        for (int i = 0; freq_paths[i]; i++) {
                            freq_stream.open(freq_paths[i]);
                            if (freq_stream.is_open())
                                break;
                        }

                        // 所有路径都不可用 → 退避 5 秒后重试（A8xx 场景）
                        if (!load_stream.is_open() && !temp_stream.is_open() &&
                            !freq_stream.is_open() && thermal_zone < 0) {
                            SPDLOG_WARN("Adreno: KGSL sysfs 路径不可用，5 秒后重试");
                            std::this_thread::sleep_for(std::chrono::seconds(5));
                            load_stream.close();
                            temp_stream.close();
                            freq_stream.close();
                            thermal_zone = -1;
                            use_gpubusy = false;
                            initialized = false;
                            continue;
                        }
                    }

                    // 读取 GPU 占用率
                    if (load_stream.is_open()) {
                        load_stream.clear();
                        load_stream.seekg(0);
                        std::string line;
                        if (std::getline(load_stream, line) && !line.empty()) {
                            if (use_gpubusy) {
                                long long used = 0, total = 0;
                                if (sscanf(line.c_str(), "%lld %lld", &used, &total) == 2 && total > 0) {
                                    int val = (int)((float)used / total * 100);
                                    if (val > 100) val = 100;
                                    if (val < 0) val = 0;
                                    gpu->metrics.load = val;
                                }
                            } else {
                                try {
                                    int val = std::stoi(line);
                                    if (val >= 0 && val <= 100)
                                        gpu->metrics.load = val;
                                } catch (...) {}
                            }
                        }
                        // 瞬态读取失败时保留上一个有效值，不设 -1（修复 A7xx N/A 跳变）
                    }

                    // 读取 GPU 温度
                    if (temp_stream.is_open()) {
                        temp_stream.clear();
                        temp_stream.seekg(0);
                        std::string temp_str;
                        if (std::getline(temp_stream, temp_str) && !temp_str.empty()) {
                            try {
                                int temp = std::stoi(temp_str);
                                if (temp > 1000) temp /= 1000;
                                gpu->metrics.temp = temp;
                            } catch (...) {}
                        }
                    } else if (thermal_zone >= 0) {
                        std::string tz_path = "/sys/class/thermal/thermal_zone" + std::to_string(thermal_zone) + "/temp";
                        std::ifstream tz_stream(tz_path);
                        std::string temp_str;
                        if (tz_stream.is_open() && std::getline(tz_stream, temp_str) && !temp_str.empty()) {
                            try {
                                int temp = std::stoi(temp_str);
                                if (temp > 1000) temp /= 1000;
                                gpu->metrics.temp = temp;
                            } catch (...) {}
                        }
                    }

                    // 读取 GPU 频率
                    if (freq_stream.is_open()) {
                        freq_stream.clear();
                        freq_stream.seekg(0);
                        std::string freq_str;
                        if (std::getline(freq_stream, freq_str) && !freq_str.empty()) {
                            try {
                                double freq = std::stod(freq_str);
                                if (freq > 1e7) freq /= 1e6;      // Hz -> MHz
                                else if (freq > 1e4) freq /= 1e3; // KHz -> MHz
                                gpu->metrics.CoreClock = (int)freq;
                            } catch (...) {}
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }).detach();

            SPDLOG_INFO("Adreno GPU injected with background monitoring thread");
        }
    }

    if (total_active < 2)
        return;

    for (auto& gpu : available_gpus) {
        if (!gpu->is_active)
            continue;

        SPDLOG_WARN(
            "You have more than 1 active GPU, check if you use both pci_dev "
            "and gpu_list. If you use fps logging, MangoHud will log only "
            "this GPU: name = {}, driver = {}, vendor = {:x}, pci_dev = {}",
            gpu->drm_node, gpu->driver, gpu->vendor_id, gpu->pci_dev
        );

        break;
    }

}

std::string GPUS::get_driver(const std::string& node) {
    try {
        std::string path = "/sys/class/drm/" + node + "/device/driver";

        if (!fs::exists(path)) {
            SPDLOG_ERROR("{} doesn't exist", path);
            return "";
        }

        if (!fs::is_symlink(path)) {
            SPDLOG_ERROR("{} is not a symlink (it should be)", path);
            return "";
        }

        std::string driver = fs::read_symlink(path).string();
        driver = driver.substr(driver.rfind("/") + 1);

        return driver;
    } catch (const fs::filesystem_error& ex) {
        SPDLOG_ERROR("filesystem_error in get_driver: {}", ex.what());
        return "";
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Error in get_driver: {}", ex.what());
        return "";
    }
}

std::string GPUS::get_pci_device_address(const std::string& drm_card_path) {
    try {
        // /sys/class/drm/renderD128/device/subsystem -> /sys/bus/pci
        auto subsystem = fs::canonical(drm_card_path + "/device/subsystem").string();
        auto idx = subsystem.rfind("/") + 1; // /sys/bus/pci
                                             //         ^
                                             //         |- find this guy
        if (subsystem.substr(idx) != "pci")
            return "";

        // /sys/class/drm/renderD128/device -> /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0/0000:02:01.0/0000:03:00.0
        auto pci_addr = fs::read_symlink(drm_card_path + "/device").string();
        idx = pci_addr.rfind("/") + 1; // /sys/.../0000:03:00.0
                                       //         ^
                                       //         |- find this guy

        return pci_addr.substr(idx); // 0000:03:00.0
    } catch (const fs::filesystem_error& ex) {
        SPDLOG_DEBUG("filesystem_error in get_pci_device_address: {}", ex.what());
        return "";
    } catch (const std::exception& ex) {
        SPDLOG_DEBUG("Error in get_pci_device_address: {}", ex.what());
        return "";
    }
}

int GPU::index_in_selected_gpus() {
    auto selected_gpus = gpus->selected_gpus();
    auto it = std::find_if(selected_gpus.begin(), selected_gpus.end(),
                        [this](const std::shared_ptr<GPU>& gpu) {
                            return gpu.get() == this;
                        });
    if (it != selected_gpus.end()) {
        return std::distance(selected_gpus.begin(), it);
    }
    return -1;
}

std::string GPU::gpu_text() {
    std::string gpu_text;
    size_t index = this->index_in_selected_gpus();

    if (gpus->selected_gpus().size() == 1) {
        // When there's exactly one selected GPU, return "GPU" without index
        gpu_text = "GPU";
        if (gpus->params()->gpu_text.size() > 0) {
            gpu_text = gpus->params()->gpu_text[0];
        }
    } else if (gpus->selected_gpus().size() > 1) {
        // When there are multiple selected GPUs, use GPU+index or matching gpu_text
        gpu_text = "GPU" + std::to_string(index);
        if (gpus->params()->gpu_text.size() > index) {
            gpu_text = gpus->params()->gpu_text[index];
        }
    } else {
        // Default case for no selected GPUs
        gpu_text = "GPU";
    }

    return gpu_text;
}

std::string GPU::vram_text() {
    std::string vram_text;
    size_t index = this->index_in_selected_gpus();
    if (gpus->selected_gpus().size() > 1)
        vram_text = "VRAM" + std::to_string(index);
    else
        vram_text = "VRAM";
    return vram_text;
}

std::shared_ptr<const overlay_params> GPUS::params() {
    return get_params();
}

std::unique_ptr<GPUS> gpus = nullptr;
