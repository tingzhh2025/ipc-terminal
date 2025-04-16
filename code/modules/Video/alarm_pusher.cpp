#include "log.h"
#include "param.h"
#include <iostream>
#include <vector>
#include "httplib.h"  // 使用httplib库进行HTTP推送
#include <chrono>
#include <iomanip>
#include <sstream>
#include "rknn/yolov5.h" 
#include "alarm_pusher.h"

// Base64编码表
static const std::string base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// 定义全局告警推送实例
AlarmPusher g_alarm_pusher;

// 重试策略配置
const int MAX_RETRY_COUNT = 3;
const int RETRY_INTERVAL_MS = 2000;

AlarmPusher::AlarmPusher() : running(false) {
    server_url = "";
    auth_token = "";
}

AlarmPusher::~AlarmPusher() {
    stop();
}

bool AlarmPusher::init() {
    // 从配置文件读取服务器地址和认证令牌
    server_url = rk_param_get_string("alarm:server_url", "http://localhost:8080");
    auth_token = rk_param_get_string("alarm:auth_token", "");
    
    LOG_DEBUG("AlarmPusher initialized with server_url: %s\n", server_url.c_str());
    return true;
}

void AlarmPusher::onAlarm(const AlarmInfo& alarm) {
    if (!running) {
        LOG_WARN("AlarmPusher not running, alarm dropped\n");
        return;
    }
    
    // 添加告警到队列
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        alarm_queue.push(alarm);
    }
    
    // 通知推送线程
    queue_cv.notify_one();
    
    // 如果有注册回调函数，调用它
    if (alarm_handler) {
        alarm_handler(alarm);
    }
    
    LOG_DEBUG("Alarm queued for pushing: class=%d(%s), confidence=%.2f\n", 
              alarm.class_id, alarm.class_name.c_str(), alarm.confidence);
}

void AlarmPusher::start() {
    if (running) {
        LOG_WARN("AlarmPusher already running\n");
        return;
    }
    
    running = true;
    push_thread = std::thread(&AlarmPusher::pushThread, this);
    LOG_DEBUG("AlarmPusher started\n");
}

void AlarmPusher::stop() {
    if (!running) {
        return;
    }
    
    running = false;
    queue_cv.notify_all();
    
    if (push_thread.joinable()) {
        push_thread.join();
    }
    
    LOG_DEBUG("AlarmPusher stopped\n");
}

void AlarmPusher::registerAlarmHandler(AlarmHandlerCallback callback) {
    alarm_handler = callback;
}

void AlarmPusher::pushThread() {
    LOG_DEBUG("AlarmPusher push thread started\n");
    
    while (running) {
        AlarmInfo alarm;
        bool has_alarm = false;
        
        // 从队列中获取告警
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (alarm_queue.empty()) {
                // 等待新的告警或者停止信号
                queue_cv.wait(lock, [this]() {
                    return !alarm_queue.empty() || !running;
                });
            }
            
            if (!running && alarm_queue.empty()) {
                break;
            }
            
            if (!alarm_queue.empty()) {
                alarm = alarm_queue.front();
                alarm_queue.pop();
                has_alarm = true;
            }
        }
        
        // 处理告警
        if (has_alarm) {
            // 尝试推送，如果失败则重试
            bool success = false;
            for (int retry = 0; retry < MAX_RETRY_COUNT && !success; retry++) {
                if (retry > 0) {
                    LOG_INFO("Retrying push to server (attempt %d of %d)...\n", 
                             retry + 1, MAX_RETRY_COUNT);
                    // 等待一段时间再重试
                    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
                }
                
                success = pushToServer(alarm);
            }
            
            if (!success) {
                LOG_ERROR("Failed to push alarm to server after %d attempts\n", MAX_RETRY_COUNT);
            }
        }
    }
    
    LOG_DEBUG("AlarmPusher push thread stopped\n");
}

bool AlarmPusher::pushToServer(const AlarmInfo& alarm) {
    if (server_url.empty()) {
        LOG_ERROR("Server URL is not set\n");
        return false;
    }

    try {
        // 解析服务器URL
        std::string url = server_url;
        std::string host;
        std::string path = "/api/alarms";
        int port = 80;
        bool is_ssl = false;

        // 解析URL格式, 例如: http://example.com:8080/api/path
        if (url.find("http://") == 0) {
            host = url.substr(7);
            is_ssl = false;
        } else if (url.find("https://") == 0) {
            host = url.substr(8);
            is_ssl = true;
            port = 443;
        } else {
            host = url;
        }

        // 获取端口和路径
        size_t port_pos = host.find(':');
        if (port_pos != std::string::npos) {
            port = std::stoi(host.substr(port_pos + 1));
            host = host.substr(0, port_pos);
        }

        size_t path_pos = host.find('/');
        if (path_pos != std::string::npos) {
            path = host.substr(path_pos);
            host = host.substr(0, path_pos);
        }

        // 构建请求体
        std::string json_body = "{";
        json_body += "\"roi_id\": " + std::to_string(alarm.roi_id) + ",";
        json_body += "\"roi_name\": \"" + alarm.roi_name + "\",";
        json_body += "\"track_id\": " + std::to_string(alarm.track_id) + ",";
        json_body += "\"class_id\": " + std::to_string(alarm.class_id) + ",";
        json_body += "\"class_name\": \"" + alarm.class_name + "\",";
        json_body += "\"confidence\": " + std::to_string(alarm.confidence) + ",";
        
        // 转换时间戳为ISO8601格式
        auto timestamp = alarm.timestamp;
        auto time_t_timestamp = std::chrono::system_clock::to_time_t(timestamp);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t_timestamp), "%FT%TZ");
        json_body += "\"timestamp\": \"" + ss.str() + "\"";
        
        // 如果有图像，则转换为Base64并添加
        if (!alarm.snapshot.empty()) {
            json_body += ",\"image\": \"" + imageToBase64(alarm.snapshot) + "\"";
        }
        
        json_body += "}";

        // 创建HTTP客户端
        httplib::Client cli(host, port);
        cli.set_connection_timeout(10); // 10秒连接超时
        cli.set_read_timeout(30);       // 30秒读取超时
        cli.set_write_timeout(30);      // 30秒写入超时
        
        // 设置请求头
        httplib::Headers headers = {
            {"Content-Type", "application/json"}
        };
        
        // 如果有认证令牌，添加到请求头
        if (!auth_token.empty()) {
            headers.emplace("Authorization", "Bearer " + auth_token);
        }

        // 发送POST请求
        auto res = cli.Post(path.c_str(), headers, json_body, "application/json");
        
        if (res) {
            if (res->status >= 200 && res->status < 300) {
                LOG_INFO("Alarm successfully pushed to server, status code: %d\n", res->status);
                return true;
            } else {
                LOG_ERROR("Error pushing alarm to server, status code: %d, response: %s\n", 
                          res->status, res->body.c_str());
                return false;
            }
        } else {
            LOG_ERROR("Failed to connect to server: %s\n", httplib::to_string(res.error()).c_str());
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during HTTP push: %s\n", e.what());
        return false;
    }

    return false;
}

std::string AlarmPusher::imageToBase64(const cv::Mat& image) {
    if (image.empty()) {
        return "";
    }

    // 首先将图像编码为JPEG格式
    std::vector<uchar> buffer;
    cv::imencode(".jpg", image, buffer);
    
    // 然后进行Base64编码
    std::string encoded;
    encoded.reserve(((buffer.size() + 2) / 3) * 4); // Base64编码后的大小
    
    int val = 0, valb = -6;
    for (uchar c : buffer) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    
    // 添加填充字符
    if (valb > -6) {
        encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (encoded.size() % 4) {
        encoded.push_back('=');
    }
    
    return encoded;
}
