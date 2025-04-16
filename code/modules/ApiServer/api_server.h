#ifndef API_SERVER_H
#define API_SERVER_H

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include "httplib.h"
#include "../Video/roi_detector.h"
#include "../Video/alarm_pusher.h"
#include "../Control/Control.h"
#include "../Led/Led.h"
#include "../Pantilt/Pantilt.h"
#include "global.h"

// 告警历史记录结构
struct AlarmHistoryEntry {
    AlarmInfo info;
    std::string timestamp_str;
    
    AlarmHistoryEntry(const AlarmInfo& alarm_info);
};

class ApiServer {
public:
    ApiServer(int port = 8080);
    ~ApiServer();

    // 启动 API 服务器
    bool start();
    
    // 停止 API 服务器
    void stop();
    
    // 设置 ROI 检测器的引用
    void setRoiDetector(RoiDetector* detector) { roi_detector = detector; }
    
    // 注册告警回调，用于记录告警历史
    void registerAlarmCallback();
    
    // 处理告警事件，记录到历史记录中
    void onAlarm(const AlarmInfo& alarm);
    
    // 设置 API 密钥，用于认证
    void setApiKey(const std::string& key) { api_key = key; }

    // 设置控制模块的引用
    void setControl(Control* ctrl) { control = ctrl; }
    
    // 设置LED模块的引用
    void setLed(Led* led) { led_module = led; }
    
    // 设置云台模块的引用
    void setPantilt(Pantilt* pt) { pantilt = pt; }

private:
    // HTTP 服务器实例
    std::unique_ptr<httplib::Server> server;
    
    // 服务器线程
    std::thread server_thread;
    
    // 服务器运行标志
    std::atomic<bool> running;
    
    // 服务器端口号
    int port;
    
    // ROI 检测器的引用
    RoiDetector* roi_detector;
    
    // API 密钥，用于认证
    std::string api_key;
    
    // 告警历史记录
    std::vector<AlarmHistoryEntry> alarm_history;
    
    // 告警历史记录互斥锁
    std::mutex alarm_history_mutex;
    
    // 最大历史记录数量
    static const int MAX_ALARM_HISTORY = 100;
    
    // 控制模块的引用
    Control* control;
    
    // LED模块的引用
    Led* led_module;
    
    // 云台模块的引用
    Pantilt* pantilt;
    
    // 初始化 API 路由
    void initRoutes();
    
    // 验证 API 密钥
    bool validateApiKey(const httplib::Request& req, httplib::Response& res);
    
    // 处理 ROI 配置查询请求
    void handleGetRoiConfig(const httplib::Request& req, httplib::Response& res);
    
    // 处理 ROI 配置更新请求
    void handleUpdateRoiConfig(const httplib::Request& req, httplib::Response& res);
    
    // 处理热加载配置请求
    void handleReloadConfig(const httplib::Request& req, httplib::Response& res);
    
    // 处理系统状态查询请求
    void handleGetSystemStatus(const httplib::Request& req, httplib::Response& res);
    
    // 处理告警历史查询请求
    void handleGetAlarmHistory(const httplib::Request& req, httplib::Response& res);
    
    // LED控制相关
    void handleLedControl(const httplib::Request& req, httplib::Response& res);
    
    // 云台控制相关
    void handlePantiltControl(const httplib::Request& req, httplib::Response& res);
    
    // 视频流控制相关
    void handleVideoControl(const httplib::Request& req, httplib::Response& res);
    
    // 系统参数管理
    void handleGetSystemParams(const httplib::Request& req, httplib::Response& res);
    void handleSetSystemParam(const httplib::Request& req, httplib::Response& res);
    
    // 将 ROI 区域转换为 JSON 字符串
    std::string roiAreaToJson(const RoiArea& roi);
    
    // 将 ROI 组转换为 JSON 字符串
    std::string roiGroupToJson(const RoiGroup& group);
    
    // 将告警历史记录转换为 JSON 字符串
    std::string alarmHistoryToJson();
};

// 全局 API 服务器实例
extern ApiServer g_api_server;

#endif // API_SERVER_H