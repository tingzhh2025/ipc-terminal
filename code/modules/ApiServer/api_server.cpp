#include "api_server.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include "log.h"
#include "param.h"
#include <sys/sysinfo.h>

// 初始化全局 API 服务器实例
ApiServer g_api_server;

AlarmHistoryEntry::AlarmHistoryEntry(const AlarmInfo& alarm_info) : info(alarm_info) {
    // 格式化时间戳
    auto time_t_timestamp = std::chrono::system_clock::to_time_t(info.timestamp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_timestamp), "%Y-%m-%d %H:%M:%S");
    timestamp_str = ss.str();
}

ApiServer::ApiServer(int port) : port(port), running(false), roi_detector(nullptr), api_key(""),
                               control(nullptr), led_module(nullptr), pantilt(nullptr) {
}

ApiServer::~ApiServer() {
    stop();
}

bool ApiServer::start() {
    if (running) {
        LOG_WARN("API server already running\n");
        return false;
    }
    
    // 创建 HTTP 服务器实例
    server = std::make_unique<httplib::Server>();
    
    // 初始化路由
    initRoutes();
    
    // 启动服务器线程
    running = true;
    server_thread = std::thread([this]() {
        LOG_INFO("Starting API server on port %d\n", port);
        server->listen("0.0.0.0", port);
    });
    
    return true;
}

void ApiServer::stop() {
    if (!running) {
        return;
    }
    
    LOG_INFO("Stopping API server\n");
    
    // 停止 HTTP 服务器
    running = false;
    if (server) {
        server->stop();
    }
    
    // 等待服务器线程结束
    if (server_thread.joinable()) {
        server_thread.join();
    }
    
    LOG_INFO("API server stopped\n");
}

void ApiServer::registerAlarmCallback() {
    if (roi_detector) {
        roi_detector->registerAlarmCallback([this](const AlarmInfo& alarm) {
            this->onAlarm(alarm);
        });
    }
}

void ApiServer::onAlarm(const AlarmInfo& alarm) {
    // 记录告警到历史记录中
    std::lock_guard<std::mutex> lock(alarm_history_mutex);
    
    // 添加新的告警记录
    alarm_history.emplace_back(alarm);
    
    // 如果历史记录超过最大数量，删除最旧的记录
    if (alarm_history.size() > MAX_ALARM_HISTORY) {
        alarm_history.erase(alarm_history.begin());
    }
    
    LOG_DEBUG("Added alarm to history, class=%d(%s), total history: %zu\n",
              alarm.class_id, alarm.class_name.c_str(), alarm_history.size());
}

bool ApiServer::validateApiKey(const httplib::Request& req, httplib::Response& res) {
    // 如果未设置 API 密钥，则不进行验证
    if (api_key.empty()) {
        return true;
    }
    
    // 从请求头中获取 API 密钥
    auto it = req.headers.find("X-API-Key");
    if (it != req.headers.end() && it->second == api_key) {
        return true;
    }
    
    // 从查询参数中获取 API 密钥
    if (req.has_param("api_key") && req.get_param_value("api_key") == api_key) {
        return true;
    }
    
    // 身份验证失败，返回 401 错误
    res.status = 401;
    res.set_content("{\"error\": \"Unauthorized access, valid API key required\"}", "application/json");
    return false;
}

void ApiServer::initRoutes() {
    if (!server) {
        LOG_ERROR("Server not initialized\n");
        return;
    }
    
    // 设置 CORS 响应头
    server->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key"}
    });
    
    // 处理 OPTIONS 请求（CORS 预检请求）
    server->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 200;
    });
    
    // ROI 相关 API
    server->Get("/api/roi/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleGetRoiConfig(req, res);
        }
    });
    
    server->Post("/api/roi/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleUpdateRoiConfig(req, res);
        }
    });
    
    server->Post("/api/roi/reload", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleReloadConfig(req, res);
        }
    });
    
    // 系统状态 API
    server->Get("/api/system/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleGetSystemStatus(req, res);
        }
    });
    
    // 告警历史 API
    server->Get("/api/alarm/history", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleGetAlarmHistory(req, res);
        }
    });
    
    // LED 控制 API
    server->Post("/api/led/control", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleLedControl(req, res);
        }
    });
    
    // 云台控制 API
    server->Post("/api/pantilt/control", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handlePantiltControl(req, res);
        }
    });
    
    // 视频控制 API
    server->Post("/api/video/control", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleVideoControl(req, res);
        }
    });
    
    // 系统参数 API
    server->Get("/api/system/params", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleGetSystemParams(req, res);
        }
    });
    
    server->Post("/api/system/params", [this](const httplib::Request& req, httplib::Response& res) {
        if (this->validateApiKey(req, res)) {
            this->handleSetSystemParam(req, res);
        }
    });
    
    // 欢迎页面不需要身份验证
    server->Get("/", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(
            "<html>"
            "<head>"
            "<title>SharpEye API Server</title>"
            "<style>"
            "body { font-family: Arial, sans-serif; margin: 40px; line-height: 1.6; }"
            "h1 { color: #333; }"
            "ul { list-style-type: none; padding: 0; }"
            "li { margin-bottom: 10px; }"
            "code { background-color: #f4f4f4; padding: 2px 5px; border-radius: 3px; }"
            "</style>"
            "</head>"
            "<body>"
            "<h1>SharpEye API Server</h1>"
            "<p>Available API endpoints:</p>"
            "<ul>"
            "<li><code>GET /api/roi/config</code> - Get current ROI configuration</li>"
            "<li><code>POST /api/roi/config</code> - Update ROI configuration</li>"
            "<li><code>POST /api/roi/reload</code> - Reload ROI configuration from file</li>"
            "<li><code>GET /api/system/status</code> - Get system status</li>"
            "<li><code>GET /api/alarm/history</code> - Get alarm history</li>"
            "<li><code>POST /api/led/control</code> - Control LED</li>"
            "<li><code>POST /api/pantilt/control</code> - Control pan/tilt</li>"
            "<li><code>POST /api/video/control</code> - Control video streams</li>"
            "<li><code>GET /api/system/params</code> - Get system parameters</li>"
            "<li><code>POST /api/system/params</code> - Set system parameters</li>"
            "</ul>"
            "<p>Authentication is required for all API endpoints. Please provide your API key using either:</p>"
            "<ul>"
            "<li>HTTP header: <code>X-API-Key: your_api_key</code></li>"
            "<li>Query parameter: <code>?api_key=your_api_key</code></li>"
            "</ul>"
            "</body>"
            "</html>",
            "text/html");
    });
}

void ApiServer::handleGetRoiConfig(const httplib::Request& req, httplib::Response& res) {
    if (!roi_detector) {
        res.status = 500;
        res.set_content("{\"error\": \"ROI detector not initialized\"}", "application/json");
        return;
    }
    
    // 获取 ROI 区域和组
    const auto& roi_areas = roi_detector->getRoiAreas();
    const auto& roi_groups = roi_detector->getRoiGroups();
    
    // 构建 JSON 响应
    std::stringstream json;
    json << "{";
    
    // ROI 区域
    json << "\"roi_areas\": [";
    for (size_t i = 0; i < roi_areas.size(); ++i) {
        json << roiAreaToJson(roi_areas[i]);
        if (i < roi_areas.size() - 1) {
            json << ",";
        }
    }
    json << "],";
    
    // ROI 组
    json << "\"roi_groups\": [";
    for (size_t i = 0; i < roi_groups.size(); ++i) {
        json << roiGroupToJson(roi_groups[i]);
        if (i < roi_groups.size() - 1) {
            json << ",";
        }
    }
    json << "]";
    
    json << "}";
    
    res.set_content(json.str(), "application/json");
}

void ApiServer::handleUpdateRoiConfig(const httplib::Request& req, httplib::Response& res) {
    if (!roi_detector) {
        res.status = 500;
        res.set_content("{\"error\": \"ROI detector not initialized\"}", "application/json");
        return;
    }
    
    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Empty request body\"}", "application/json");
        return;
    }
    
    // 解析 JSON 请求体
    try {
        // 这里我们使用简单的方式解析 JSON，未使用第三方 JSON 库
        // 在实际项目中，建议使用 nlohmann/json 等库进行更健壮的解析
        std::string config_file = rk_param_get_string("common:config_file", "ipc-terminal.ini");
        bool success = false;
        
        // 解析 ROI 区域更新
        if (req.body.find("\"roi_areas\"") != std::string::npos) {
            std::string roi_areas_str = req.body.substr(req.body.find("\"roi_areas\""));
            if (roi_areas_str.find('[') != std::string::npos && 
                roi_areas_str.find(']') != std::string::npos) {
                
                // 提取数组内容
                size_t start_pos = roi_areas_str.find('[');
                size_t end_pos = roi_areas_str.find(']', start_pos);
                std::string areas_array = roi_areas_str.substr(start_pos + 1, end_pos - start_pos - 1);
                
                // 处理每个 ROI 区域对象
                size_t pos = 0;
                size_t obj_start, obj_end;
                int roi_count = 0;
                
                // 首先更新 ROI 区域的数量
                while ((obj_start = areas_array.find('{', pos)) != std::string::npos) {
                    obj_end = areas_array.find('}', obj_start);
                    if (obj_end == std::string::npos) break;
                    
                    std::string roi_obj = areas_array.substr(obj_start, obj_end - obj_start + 1);
                    roi_count++;
                    pos = obj_end + 1;
                }
                
                // 更新配置文件中的 ROI 数量
                char value[16];
                snprintf(value, sizeof(value), "%d", roi_count);
                rk_param_set_string("ai.roi:count", value);
                
                // 现在处理每个 ROI 区域的内容
                pos = 0;
                int roi_index = 0;
                while ((obj_start = areas_array.find('{', pos)) != std::string::npos) {
                    obj_end = areas_array.find('}', obj_start);
                    if (obj_end == std::string::npos) break;
                    
                    std::string roi_obj = areas_array.substr(obj_start, obj_end - obj_start + 1);
                    
                    // 解析 ROI 属性
                    auto parseValue = [&](const std::string& key, const std::string& obj, bool is_string = false) -> std::string {
                        std::string search = "\"" + key + "\":";
                        size_t key_pos = obj.find(search);
                        if (key_pos == std::string::npos) return "";
                        
                        size_t value_start = key_pos + search.length();
                        // 跳过前导空格
                        while (value_start < obj.length() && std::isspace(obj[value_start])) value_start++;
                        
                        if (is_string) {
                            if (obj[value_start] != '"') return "";
                            size_t value_end = obj.find('"', value_start + 1);
                            if (value_end == std::string::npos) return "";
                            return obj.substr(value_start + 1, value_end - value_start - 1);
                        } else {
                            size_t value_end = obj.find_first_of(",}", value_start);
                            if (value_end == std::string::npos) return "";
                            return obj.substr(value_start, value_end - value_start);
                        }
                    };
                    
                    // 解析基本属性
                    std::string id = parseValue("id", roi_obj);
                    std::string name = parseValue("name", roi_obj, true);
                    std::string x = parseValue("x", roi_obj);
                    std::string y = parseValue("y", roi_obj);
                    std::string width = parseValue("width", roi_obj);
                    std::string height = parseValue("height", roi_obj);
                    std::string stay_time = parseValue("stay_time", roi_obj);
                    std::string cooldown_time = parseValue("cooldown_time", roi_obj);
                    std::string enabled = parseValue("enabled", roi_obj);
                    std::string group_id = parseValue("group_id", roi_obj);
                    
                    // 解析类别数组
                    std::string classes_str;
                    size_t classes_start = roi_obj.find("\"classes\":");
                    if (classes_start != std::string::npos) {
                        size_t array_start = roi_obj.find('[', classes_start);
                        size_t array_end = roi_obj.find(']', array_start);
                        if (array_start != std::string::npos && array_end != std::string::npos) {
                            classes_str = roi_obj.substr(array_start + 1, array_end - array_start - 1);
                        }
                    }
                    
                    // 更新配置文件
                    char param_name[64];
                    
                    snprintf(param_name, sizeof(param_name), "ai.roi.%d:enabled", roi_index);
                    rk_param_set_string(param_name, enabled.empty() ? "1" : enabled.c_str());
                    
                    if (!name.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:name", roi_index);
                        rk_param_set_string(param_name, name.c_str());
                    }
                    
                    if (!x.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:x", roi_index);
                        rk_param_set_string(param_name, x.c_str());
                    }
                    
                    if (!y.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:y", roi_index);
                        rk_param_set_string(param_name, y.c_str());
                    }
                    
                    if (!width.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:width", roi_index);
                        rk_param_set_string(param_name, width.c_str());
                    }
                    
                    if (!height.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:height", roi_index);
                        rk_param_set_string(param_name, height.c_str());
                    }
                    
                    if (!stay_time.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:stay_time", roi_index);
                        rk_param_set_string(param_name, stay_time.c_str());
                    }
                    
                    if (!cooldown_time.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:cooldown_time", roi_index);
                        rk_param_set_string(param_name, cooldown_time.c_str());
                    }
                    
                    if (!classes_str.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.%d:classes", roi_index);
                        rk_param_set_string(param_name, classes_str.c_str());
                    }
                    
                    roi_index++;
                    pos = obj_end + 1;
                }
                
                success = true;
            }
        }
        
        // 解析 ROI 组更新
        if (req.body.find("\"roi_groups\"") != std::string::npos) {
            std::string roi_groups_str = req.body.substr(req.body.find("\"roi_groups\""));
            if (roi_groups_str.find('[') != std::string::npos && 
                roi_groups_str.find(']') != std::string::npos) {
                
                // 提取数组内容
                size_t start_pos = roi_groups_str.find('[');
                size_t end_pos = roi_groups_str.find(']', start_pos);
                std::string groups_array = roi_groups_str.substr(start_pos + 1, end_pos - start_pos - 1);
                
                // 处理每个 ROI 组对象
                size_t pos = 0;
                size_t obj_start, obj_end;
                int group_index = 0;
                
                // 首先更新 ROI 组的数量
                while ((obj_start = groups_array.find('{', pos)) != std::string::npos) {
                    obj_end = groups_array.find('}', obj_start);
                    if (obj_end == std::string::npos) break;
                    
                    std::string group_obj = groups_array.substr(obj_start, obj_end - obj_start + 1);
                    group_index++;
                    pos = obj_end + 1;
                }
                
                // 更新配置文件中的 ROI 组数量
                char value[16];
                snprintf(value, sizeof(value), "%d", group_index);
                rk_param_set_string("ai.roi.group:count", value);
                
                // 现在处理每个 ROI 组的内容
                pos = 0;
                group_index = 0;
                while ((obj_start = groups_array.find('{', pos)) != std::string::npos) {
                    obj_end = groups_array.find('}', obj_start);
                    if (obj_end == std::string::npos) break;
                    
                    std::string group_obj = groups_array.substr(obj_start, obj_end - obj_start + 1);
                    
                    // 解析 ROI 组属性
                    auto parseValue = [&](const std::string& key, const std::string& obj, bool is_string = false) -> std::string {
                        std::string search = "\"" + key + "\":";
                        size_t key_pos = obj.find(search);
                        if (key_pos == std::string::npos) return "";
                        
                        size_t value_start = key_pos + search.length();
                        // 跳过前导空格
                        while (value_start < obj.length() && std::isspace(obj[value_start])) value_start++;
                        
                        if (is_string) {
                            if (obj[value_start] != '"') return "";
                            size_t value_end = obj.find('"', value_start + 1);
                            if (value_end == std::string::npos) return "";
                            return obj.substr(value_start + 1, value_end - value_start - 1);
                        } else {
                            size_t value_end = obj.find_first_of(",}", value_start);
                            if (value_end == std::string::npos) return "";
                            return obj.substr(value_start, value_end - value_start);
                        }
                    };
                    
                    // 解析基本属性
                    std::string id = parseValue("id", group_obj);
                    std::string name = parseValue("name", group_obj, true);
                    
                    // 解析类别数组
                    std::string classes_str;
                    size_t classes_start = group_obj.find("\"classes\":");
                    if (classes_start != std::string::npos) {
                        size_t array_start = group_obj.find('[', classes_start);
                        size_t array_end = group_obj.find(']', array_start);
                        if (array_start != std::string::npos && array_end != std::string::npos) {
                            classes_str = group_obj.substr(array_start + 1, array_end - array_start - 1);
                        }
                    }
                    
                    // 解析 ROI ID 数组
                    std::string roi_ids_str;
                    size_t roi_ids_start = group_obj.find("\"roi_ids\":");
                    if (roi_ids_start != std::string::npos) {
                        size_t array_start = group_obj.find('[', roi_ids_start);
                        size_t array_end = group_obj.find(']', array_start);
                        if (array_start != std::string::npos && array_end != std::string::npos) {
                            roi_ids_str = group_obj.substr(array_start + 1, array_end - array_start - 1);
                        }
                    }
                    
                    // 更新配置文件
                    char param_name[64];
                    
                    if (!name.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:name", group_index);
                        rk_param_set_string(param_name, name.c_str());
                    }
                    
                    if (!classes_str.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:classes", group_index);
                        rk_param_set_string(param_name, classes_str.c_str());
                    }
                    
                    if (!roi_ids_str.empty()) {
                        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:rois", group_index);
                        rk_param_set_string(param_name, roi_ids_str.c_str());
                    }
                    
                    group_index++;
                    pos = obj_end + 1;
                }
                
                success = true;
            }
        }
        
        // 保存配置文件
        rk_param_save();
        
        if (success) {
            // 重新加载配置
            bool reload_success = roi_detector->reloadConfig();
            if (reload_success) {
                res.set_content("{\"message\": \"Configuration updated and reloaded successfully\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"error\": \"Configuration updated but failed to reload\"}", "application/json");
            }
        } else {
            res.status = 400;
            res.set_content("{\"error\": \"Invalid request format\"}", "application/json");
        }
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content("{\"error\": \"Exception occurred: " + std::string(e.what()) + "\"}", "application/json");
    }
}

void ApiServer::handleReloadConfig(const httplib::Request& req, httplib::Response& res) {
    if (!roi_detector) {
        res.status = 500;
        res.set_content("{\"error\": \"ROI detector not initialized\"}", "application/json");
        return;
    }
    
    bool success = roi_detector->reloadConfig();
    
    if (success) {
        res.set_content("{\"message\": \"Configuration reloaded successfully\"}", "application/json");
    } else {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to reload configuration\"}", "application/json");
    }
}

void ApiServer::handleGetSystemStatus(const httplib::Request& req, httplib::Response& res) {
    struct sysinfo info;
    
    if (sysinfo(&info) != 0) {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to get system information\"}", "application/json");
        return;
    }
    
    // 计算内存使用情况
    long total_ram = info.totalram * info.mem_unit;
    long free_ram = info.freeram * info.mem_unit;
    long used_ram = total_ram - free_ram;
    double ram_usage = (double)used_ram / total_ram * 100.0;
    
    // 计算负载
    double load_1 = info.loads[0] / 65536.0;
    double load_5 = info.loads[1] / 65536.0;
    double load_15 = info.loads[2] / 65536.0;
    
    // 计算正常运行时间
    int uptime_days = info.uptime / 86400;
    int uptime_hours = (info.uptime % 86400) / 3600;
    int uptime_minutes = (info.uptime % 3600) / 60;
    int uptime_seconds = info.uptime % 60;
    
    // 获取告警数量
    size_t alarm_count = 0;
    {
        std::lock_guard<std::mutex> lock(alarm_history_mutex);
        alarm_count = alarm_history.size();
    }
    
    // 构建 JSON 响应
    std::stringstream json;
    json << "{";
    json << "\"memory\": {";
    json << "\"total\": " << total_ram << ",";
    json << "\"free\": " << free_ram << ",";
    json << "\"used\": " << used_ram << ",";
    json << "\"usage_percent\": " << ram_usage;
    json << "},";
    
    json << "\"load\": {";
    json << "\"1min\": " << load_1 << ",";
    json << "\"5min\": " << load_5 << ",";
    json << "\"15min\": " << load_15;
    json << "},";
    
    json << "\"uptime\": {";
    json << "\"days\": " << uptime_days << ",";
    json << "\"hours\": " << uptime_hours << ",";
    json << "\"minutes\": " << uptime_minutes << ",";
    json << "\"seconds\": " << uptime_seconds << ",";
    json << "\"total_seconds\": " << info.uptime;
    json << "},";
    
    json << "\"alarm_count\": " << alarm_count;
    
    json << "}";
    
    res.set_content(json.str(), "application/json");
}

void ApiServer::handleGetAlarmHistory(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(alarm_history_mutex);
    
    if (alarm_history.empty()) {
        res.set_content("{\"history\": []}", "application/json");
        return;
    }
    
    res.set_content("{\"history\": " + alarmHistoryToJson() + "}", "application/json");
}

void ApiServer::handleLedControl(const httplib::Request& req, httplib::Response& res) {
    if (!led_module || !control) {
        res.status = 503;
        res.set_content("{\"error\": \"LED module not available\"}", "application/json");
        return;
    }
    
    // 解析请求体，获取操作类型
    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Empty request body\"}", "application/json");
        return;
    }
    
    // 简单解析 JSON
    std::string action;
    int duration = 0;
    
    // 解析 action 字段
    size_t action_pos = req.body.find("\"action\"");
    if (action_pos != std::string::npos) {
        size_t value_start = req.body.find(':', action_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        if (req.body[value_start] == '"') {
            // 字符串值
            size_t value_end = req.body.find('"', value_start + 1);
            if (value_end != std::string::npos) {
                action = req.body.substr(value_start + 1, value_end - value_start - 1);
            }
        }
    }
    
    // 解析 duration 字段（如果有）
    size_t duration_pos = req.body.find("\"duration\"");
    if (duration_pos != std::string::npos) {
        size_t value_start = req.body.find(':', duration_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        size_t value_end = req.body.find_first_of(",}", value_start);
        if (value_end != std::string::npos) {
            std::string duration_str = req.body.substr(value_start, value_end - value_start);
            try {
                duration = std::stoi(duration_str);
            } catch (const std::exception& e) {
                duration = 0;
            }
        }
    }
    
    if (action.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Missing 'action' field\"}", "application/json");
        return;
    }
    
    bool success = false;
    
    if (action == "on") {
        control->signal_control_received.emit({ ID_LED, OP_LED_ON, 0 });
        success = true;
    } else if (action == "off") {
        control->signal_control_received.emit({ ID_LED, OP_LED_OFF, 0 });
        success = true;
    } else if (action == "toggle") {
        control->signal_control_received.emit({ ID_LED, OP_LED_TOGGLE, 0 });
        success = true;
    } else if (action == "blink" && duration > 0) {
        control->signal_control_received.emit({ ID_LED, OP_LED_BLINK, duration });
        success = true;
    } else {
        res.status = 400;
        res.set_content("{\"error\": \"Invalid action or missing required parameters\"}", "application/json");
        return;
    }
    
    if (success) {
        res.set_content("{\"message\": \"LED control command sent successfully\"}", "application/json");
    } else {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to send LED control command\"}", "application/json");
    }
}

void ApiServer::handlePantiltControl(const httplib::Request& req, httplib::Response& res) {
    if (!pantilt || !control) {
        res.status = 503;
        res.set_content("{\"error\": \"Pantilt module not available\"}", "application/json");
        return;
    }
    
    // 解析请求体，获取操作类型
    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Empty request body\"}", "application/json");
        return;
    }
    
    // 简单解析 JSON
    std::string action;
    int degree = 0;
    
    // 解析 action 字段
    size_t action_pos = req.body.find("\"action\"");
    if (action_pos != std::string::npos) {
        size_t value_start = req.body.find(':', action_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        if (req.body[value_start] == '"') {
            // 字符串值
            size_t value_end = req.body.find('"', value_start + 1);
            if (value_end != std::string::npos) {
                action = req.body.substr(value_start + 1, value_end - value_start - 1);
            }
        }
    }
    
    // 解析 degree 字段（如果有）
    size_t degree_pos = req.body.find("\"degree\"");
    if (degree_pos != std::string::npos) {
        size_t value_start = req.body.find(':', degree_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        size_t value_end = req.body.find_first_of(",}", value_start);
        if (value_end != std::string::npos) {
            std::string degree_str = req.body.substr(value_start, value_end - value_start);
            try {
                degree = std::stoi(degree_str);
            } catch (const std::exception& e) {
                degree = 0;
            }
        }
    }
    
    if (action.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Missing 'action' field\"}", "application/json");
        return;
    }
    
    bool success = false;
    
    if (action == "up") {
        control->signal_control_received.emit({ ID_PANTILT, OP_PANTILT_UP, degree > 0 ? degree : 5 });
        success = true;
    } else if (action == "down") {
        control->signal_control_received.emit({ ID_PANTILT, OP_PANTILT_DOWN, degree > 0 ? degree : 5 });
        success = true;
    } else if (action == "left") {
        control->signal_control_received.emit({ ID_PANTILT, OP_PANTILT_LEFT, degree > 0 ? degree : 5 });
        success = true;
    } else if (action == "right") {
        control->signal_control_received.emit({ ID_PANTILT, OP_PANTILT_RIGHT, degree > 0 ? degree : 5 });
        success = true;
    } else if (action == "reset") {
        control->signal_control_received.emit({ ID_PANTILT, OP_PANTILT_RESET, 0 });
        success = true;
    } else {
        res.status = 400;
        res.set_content("{\"error\": \"Invalid action\"}", "application/json");
        return;
    }
    
    if (success) {
        res.set_content("{\"message\": \"Pantilt control command sent successfully\"}", "application/json");
    } else {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to send pantilt control command\"}", "application/json");
    }
}

void ApiServer::handleVideoControl(const httplib::Request& req, httplib::Response& res) {
    if (!control) {
        res.status = 503;
        res.set_content("{\"error\": \"Video module not available\"}", "application/json");
        return;
    }
    
    // 解析请求体，获取操作类型
    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Empty request body\"}", "application/json");
        return;
    }
    
    // 简单解析 JSON
    std::string action;
    int pipe = 0; // 默认为 pipe0
    
    // 解析 action 字段
    size_t action_pos = req.body.find("\"action\"");
    if (action_pos != std::string::npos) {
        size_t value_start = req.body.find(':', action_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        if (req.body[value_start] == '"') {
            // 字符串值
            size_t value_end = req.body.find('"', value_start + 1);
            if (value_end != std::string::npos) {
                action = req.body.substr(value_start + 1, value_end - value_start - 1);
            }
        }
    }
    
    // 解析 pipe 字段（如果有）
    size_t pipe_pos = req.body.find("\"pipe\"");
    if (pipe_pos != std::string::npos) {
        size_t value_start = req.body.find(':', pipe_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        size_t value_end = req.body.find_first_of(",}", value_start);
        if (value_end != std::string::npos) {
            std::string pipe_str = req.body.substr(value_start, value_end - value_start);
            try {
                pipe = std::stoi(pipe_str);
            } catch (const std::exception& e) {
                pipe = 0;
            }
        }
    }
    
    if (action.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Missing 'action' field\"}", "application/json");
        return;
    }
    
    bool success = false;
    
    if (action == "start") {
        if (pipe == 0) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE0_START, 0 });
        } else if (pipe == 1) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE1_START, 0 });
        } else {
            res.status = 400;
            res.set_content("{\"error\": \"Invalid pipe number, must be 0 or 1\"}", "application/json");
            return;
        }
        success = true;
    } else if (action == "stop") {
        if (pipe == 0) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE0_STOP, 0 });
        } else if (pipe == 1) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE1_STOP, 0 });
        } else {
            res.status = 400;
            res.set_content("{\"error\": \"Invalid pipe number, must be 0 or 1\"}", "application/json");
            return;
        }
        success = true;
    } else if (action == "restart") {
        if (pipe == 0) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE0_RESTART, 0 });
        } else if (pipe == 1) {
            control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_PIPE1_RESTART, 0 });
        } else {
            res.status = 400;
            res.set_content("{\"error\": \"Invalid pipe number, must be 0 or 1\"}", "application/json");
            return;
        }
        success = true;
    } else if (action == "reload_config") {
        control->signal_control_received.emit({ ID_VIDEO, OP_VIDEO_RELOAD_CONFIG, 0 });
        success = true;
    } else {
        res.status = 400;
        res.set_content("{\"error\": \"Invalid action\"}", "application/json");
        return;
    }
    
    if (success) {
        res.set_content("{\"message\": \"Video control command sent successfully\"}", "application/json");
    } else {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to send video control command\"}", "application/json");
    }
}

void ApiServer::handleGetSystemParams(const httplib::Request& req, httplib::Response& res) {
    // 查询特定部分参数
    std::string section;
    if (req.has_param("section")) {
        section = req.get_param_value("section");
    }
    
    // 获取所有参数
    std::vector<std::pair<std::string, std::string>> params;
    if (!section.empty()) {
        // 获取特定部分的参数
        params = rk_param_get_section(section.c_str());
    } else {
        // 获取所有参数
        params = rk_param_get_all();
    }
    
    // 构建 JSON 响应
    std::stringstream json;
    json << "{";
    
    if (!section.empty()) {
        json << "\"section\": \"" << section << "\",";
    }
    
    json << "\"params\": {";
    
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        json << "\"" << param.first << "\": \"" << param.second << "\"";
        if (i < params.size() - 1) {
            json << ",";
        }
    }
    
    json << "}}";
    
    res.set_content(json.str(), "application/json");
}

void ApiServer::handleSetSystemParam(const httplib::Request& req, httplib::Response& res) {
    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Empty request body\"}", "application/json");
        return;
    }
    
    // 解析请求体，获取参数名和值
    std::string param_name;
    std::string param_value;
    bool save_to_file = false;
    
    // 解析 param_name 字段
    size_t name_pos = req.body.find("\"param_name\"");
    if (name_pos != std::string::npos) {
        size_t value_start = req.body.find(':', name_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        if (req.body[value_start] == '"') {
            // 字符串值
            size_t value_end = req.body.find('"', value_start + 1);
            if (value_end != std::string::npos) {
                param_name = req.body.substr(value_start + 1, value_end - value_start - 1);
            }
        }
    }
    
    // 解析 param_value 字段
    size_t value_pos = req.body.find("\"param_value\"");
    if (value_pos != std::string::npos) {
        size_t value_start = req.body.find(':', value_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        if (req.body[value_start] == '"') {
            // 字符串值
            size_t value_end = req.body.find('"', value_start + 1);
            if (value_end != std::string::npos) {
                param_value = req.body.substr(value_start + 1, value_end - value_start - 1);
            }
        } else if (isdigit(req.body[value_start]) || req.body[value_start] == '-') {
            // 数字值
            size_t value_end = req.body.find_first_of(",}", value_start);
            if (value_end != std::string::npos) {
                param_value = req.body.substr(value_start, value_end - value_start);
            }
        }
    }
    
    // 解析 save 字段（如果有）
    size_t save_pos = req.body.find("\"save\"");
    if (save_pos != std::string::npos) {
        size_t value_start = req.body.find(':', save_pos) + 1;
        // 跳过空白字符
        while (value_start < req.body.length() && isspace(req.body[value_start])) {
            value_start++;
        }
        
        size_t value_end = req.body.find_first_of(",}", value_start);
        if (value_end != std::string::npos) {
            std::string save_str = req.body.substr(value_start, value_end - value_start);
            save_to_file = (save_str == "true");
        }
    }
    
    if (param_name.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Missing 'param_name' field\"}", "application/json");
        return;
    }
    
    if (param_value.empty()) {
        res.status = 400;
        res.set_content("{\"error\": \"Missing 'param_value' field\"}", "application/json");
        return;
    }
    
    // 检查是否有权限修改该参数
    if (param_name.find("api:key") != std::string::npos) {
        // 修改 API 密钥是特殊情况，需要额外处理
        api_key = param_value;
        LOG_INFO("API key updated\n");
    }
    
    // 设置参数值
    bool success = rk_param_set_string(param_name.c_str(), param_value.c_str());
    
    if (success) {
        // 如果需要，保存参数到文件
        if (save_to_file) {
            rk_param_save();
        }
        
        // 构建包含更新后参数值的响应
        std::stringstream json;
        json << "{";
        json << "\"message\": \"Parameter updated successfully\",";
        json << "\"param_name\": \"" << param_name << "\",";
        json << "\"param_value\": \"" << param_value << "\",";
        json << "\"saved_to_file\": " << (save_to_file ? "true" : "false");
        json << "}";
        
        res.set_content(json.str(), "application/json");
    } else {
        res.status = 500;
        res.set_content("{\"error\": \"Failed to update parameter\"}", "application/json");
    }
}

std::string ApiServer::roiAreaToJson(const RoiArea& roi) {
    std::stringstream json;
    
    json << "{";
    json << "\"id\": " << roi.id << ",";
    json << "\"name\": \"" << roi.name << "\",";
    json << "\"x\": " << roi.x << ",";
    json << "\"y\": " << roi.y << ",";
    json << "\"width\": " << roi.width << ",";
    json << "\"height\": " << roi.height << ",";
    json << "\"stay_time\": " << roi.stay_time << ",";
    json << "\"cooldown_time\": " << roi.cooldown_time << ",";
    json << "\"enabled\": " << (roi.enabled ? "true" : "false") << ",";
    json << "\"group_id\": " << roi.group_id << ",";
    
    // 类别数组
    json << "\"classes\": [";
    for (size_t i = 0; i < roi.classes.size(); ++i) {
        json << roi.classes[i];
        if (i < roi.classes.size() - 1) {
            json << ",";
        }
    }
    json << "]";
    
    json << "}";
    
    return json.str();
}

std::string ApiServer::roiGroupToJson(const RoiGroup& group) {
    std::stringstream json;
    
    json << "{";
    json << "\"id\": " << group.id << ",";
    json << "\"name\": \"" << group.name << "\",";
    
    // 类别数组
    json << "\"classes\": [";
    for (size_t i = 0; i < group.classes.size(); ++i) {
        json << group.classes[i];
        if (i < group.classes.size() - 1) {
            json << ",";
        }
    }
    json << "],";
    
    // ROI ID 数组
    json << "\"roi_ids\": [";
    for (size_t i = 0; i < group.roi_ids.size(); ++i) {
        json << group.roi_ids[i];
        if (i < group.roi_ids.size() - 1) {
            json << ",";
        }
    }
    json << "]";
    
    json << "}";
    
    return json.str();
}

std::string ApiServer::alarmHistoryToJson() {
    std::stringstream json;
    
    json << "[";
    for (size_t i = 0; i < alarm_history.size(); ++i) {
        const auto& entry = alarm_history[i];
        const auto& alarm = entry.info;
        
        json << "{";
        json << "\"timestamp\": \"" << entry.timestamp_str << "\",";
        json << "\"roi_id\": " << alarm.roi_id << ",";
        json << "\"roi_name\": \"" << alarm.roi_name << "\",";
        json << "\"group_id\": " << alarm.group_id << ",";
        json << "\"group_name\": \"" << alarm.group_name << "\",";
        json << "\"track_id\": " << alarm.track_id << ",";
        json << "\"class_id\": " << alarm.class_id << ",";
        json << "\"class_name\": \"" << alarm.class_name << "\",";
        json << "\"confidence\": " << alarm.confidence << ",";
        json << "\"box\": {";
        json << "\"x\": " << alarm.box.x << ",";
        json << "\"y\": " << alarm.box.y << ",";
        json << "\"width\": " << alarm.box.width << ",";
        json << "\"height\": " << alarm.box.height;
        json << "}";
        json << "}";
        
        if (i < alarm_history.size() - 1) {
            json << ",";
        }
    }
    json << "]";
    
    return json.str();
}