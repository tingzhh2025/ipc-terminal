#ifndef ROI_DETECTOR_H
#define ROI_DETECTOR_H

#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <chrono>
#include <opencv2/opencv.hpp>
#include "postprocess.h"
#include "tracker/BYTETracker.h"

// ROI区域定义
struct RoiArea {
    int id;                     // ROI的唯一标识
    int x, y, width, height;    // ROI位置和大小
    std::vector<int> classes;   // 该ROI关注的目标类别
    int stay_time;              // 进入ROI后触发告警的停留时间(秒)，0表示立即触发
    int cooldown_time;          // 告警冷却时间(秒)，默认10秒
    std::string name;           // ROI名称，用于日志和推送
    cv::Scalar color;           // ROI显示颜色
    bool enabled;               // ROI是否启用
    int group_id;               // 所属组ID，-1表示不属于任何组
};

// ROI组定义
struct RoiGroup {
    int id;                     // 组ID
    std::string name;           // 组名称
    std::vector<int> classes;   // 关注的目标类别
    std::vector<int> roi_ids;   // 组内包含的ROI ID列表
    cv::Scalar color;           // 组显示颜色
};

// 目标对象状态
struct RoiObject {
    int track_id;           // 跟踪ID
    int class_id;           // 目标类别ID
    cv::Rect box;           // 目标框位置
    float confidence;       // 置信度
    bool in_roi;            // 当前是否在ROI内
    int roi_id;             // 位于哪个ROI内，-1表示不在任何ROI内
    int group_id;           // 位于哪个组内，-1表示不在任何组内
    std::chrono::steady_clock::time_point first_seen;  // 首次进入ROI的时间
    std::chrono::steady_clock::time_point last_alarm;  // 上次触发告警的时间
    bool alarm_triggered;   // 是否已触发告警
};

// 告警信息
struct AlarmInfo {
    int roi_id;                 // 触发告警的ROI ID
    std::string roi_name;       // ROI名称
    int group_id;               // 触发告警的组ID，-1表示不属于任何组
    std::string group_name;     // 组名称，如果有
    int track_id;               // 触发告警的目标ID
    int class_id;               // 目标类别
    std::string class_name;     // 目标类别名称
    cv::Rect box;               // 目标位置
    float confidence;           // 置信度
    cv::Mat snapshot;           // 告警截图
    std::chrono::system_clock::time_point timestamp; // 告警时间
};

class RoiDetector {
public:
    RoiDetector();
    ~RoiDetector();

    // 从配置文件加载ROI定义
    bool loadConfig();
    
    // 重新加载配置（热加载）
    bool reloadConfig();
    
    // 处理检测结果
    void processDetectionResult(const cv::Mat& frame, object_detect_result_list& od_results);
    
    // 判断目标是否在ROI内
    bool isObjectInRoi(const cv::Rect& obj_box, const RoiArea& roi);
    
    // 判断目标类别是否在组关注的类别中
    bool isClassInGroup(int class_id, const RoiGroup& group);
    
    // 获取ROI所属的组
    int getRoiGroup(int roi_id);
    
    // 绘制ROI区域和目标框
    cv::Mat drawResults(const cv::Mat& frame);
    
    // 获取所有需要检测的类别
    std::vector<int> getDetectionClasses();
    
    // 获取ROI组列表
    const std::vector<RoiGroup>& getRoiGroups() const { return roi_groups; }
    
    // 获取ROI区域列表
    const std::vector<RoiArea>& getRoiAreas() const { return roi_areas; }
    
    // 注册告警回调函数
    using AlarmCallback = std::function<void(const AlarmInfo&)>;
    void registerAlarmCallback(AlarmCallback callback);

private:
    // 目标跟踪器
    std::unique_ptr<BYTETracker> tracker;
    
    // 配置参数
    float detection_threshold;
    
    // ROI区域列表
    std::vector<RoiArea> roi_areas;
    
    // ROI组列表
    std::vector<RoiGroup> roi_groups;
    
    // ROI ID到组ID的映射
    std::unordered_map<int, int> roi_to_group;
    
    // 组关注的类别集合（用于快速查找）
    std::unordered_map<int, std::unordered_set<int>> group_classes;
    
    // 目标状态映射表 (track_id -> object)
    std::unordered_map<int, RoiObject> tracked_objects;
    
    // 告警回调函数
    AlarmCallback alarm_callback;
    
    // 检查并触发告警
    void checkAlarm(const cv::Mat& frame, int track_id);
    
    // 处理目标状态
    void updateObjectStatus(const cv::Mat& frame);
    
    // 清理过期的目标
    void cleanExpiredObjects();
    
    // 解析类别字符串
    std::vector<int> parseClassesString(const std::string& classes_str);
    
    // 解析ROI ID列表
    std::vector<int> parseRoiIdsString(const std::string& rois_str);
    
    // 根据类别ID生成随机颜色
    cv::Scalar generateColorByClassId(int class_id);
};

#endif // ROI_DETECTOR_H
