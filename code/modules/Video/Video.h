#pragma once

#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <chrono>
#include <unordered_set>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "luckfox_rtsp.h"
#include "luckfox_video.h"
#include "luckfox_rgn_draw.h"
#include "luckfox_osd.h"

#include "yolov5.h"
#include "postprocess.h"

#include "Signal.h"
#include "roi_detector.h"
#include "alarm_pusher.h"

// 前向声明
class ApiServer;

class Video {
public:
    Video();
    ~Video();

    Signal<const cv::Mat&> signal_video_frame;
    Signal<int, int> signal_adjust_pantilt;

    void video_pipe0_start();
    void video_pipe0_stop();
    void video_pipe0_restart();

    void video_pipe1_start();
    void video_pipe1_stop();
    void video_pipe1_restart();

    void video_pipe2_start();
    void video_pipe2_stop();
    void video_pipe2_restart();
    
    // 重新加载 ROI 配置
    bool reload_roi_config();
    
    // 获取 ROI 检测器的引用（用于调试和配置）
    RoiDetector* get_roi_detector() { return roi_detector.get(); }

private:
    void video_pipe0();
    void video_pipe1();
    void video_pipe2();

    bool video_run_;
    bool pipe0_run_;
    bool pipe1_run_;
    bool pipe2_run_;

    std::mutex mtx_video;
    std::unique_ptr<std::thread> video_thread0;
    std::unique_ptr<std::thread> video_thread1;
    std::unique_ptr<std::thread> video_thread2;

    // ROI目标检测器
    std::unique_ptr<RoiDetector> roi_detector;

    // 处理告警事件
    void handleAlarm(const AlarmInfo& alarm);

    // 视频预处理函数
    cv::Mat letterbox(const cv::Mat &image, int w, int h);
    void mapCoordinates(int *x, int *y);
};