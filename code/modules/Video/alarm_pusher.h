#ifndef ALARM_PUSHER_H
#define ALARM_PUSHER_H

#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include "roi_detector.h"

class AlarmPusher {
public:
    AlarmPusher();
    ~AlarmPusher();

    // 初始化推送模块
    bool init();
    
    // 接收并处理告警事件
    void onAlarm(const AlarmInfo& alarm);
    
    // 启动推送线程
    void start();
    
    // 停止推送线程
    void stop();
    
    // 注册回调函数处理告警事件
    using AlarmHandlerCallback = std::function<void(const AlarmInfo&)>;
    void registerAlarmHandler(AlarmHandlerCallback callback);

private:
    // 推送线程函数
    void pushThread();
    
    // HTTP推送函数
    bool pushToServer(const AlarmInfo& alarm);
    
    // 将图像转换为base64
    std::string imageToBase64(const cv::Mat& image);
    
    std::string server_url;
    std::string auth_token;
    
    std::thread push_thread;
    std::queue<AlarmInfo> alarm_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> running;
    
    AlarmHandlerCallback alarm_handler;
};

// 全局告警推送实例
extern AlarmPusher g_alarm_pusher;

#endif // ALARM_PUSHER_H
