#include <unordered_set>
#include <algorithm>
#include <sstream>
#include "param.h"
#include "rknn/yolov5.h" // 确保在 roi_detector.h 前包含
#include "roi_detector.h"
#include "global.h"

RoiDetector::RoiDetector() {
    // 创建目标跟踪器
    BYTETrackerParams params;
    params.track_buffer = 30;  // 跟踪缓冲区大小
    params.track_thresh = 0.25f;  // 跟踪阈值
    params.high_thresh = 0.6f;  // 高质量目标阈值
    params.match_thresh = 0.8f;  // 匹配阈值
    
    tracker = std::make_unique<BYTETracker>(params);
    
    // 从配置文件加载参数
    loadConfig();
}

RoiDetector::~RoiDetector() {
    // 清理资源
}

std::vector<int> RoiDetector::parseClassesString(const std::string& classes_str) {
    std::vector<int> result;
    std::stringstream ss(classes_str);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // 去除空格
        item.erase(remove_if(item.begin(), item.end(), ::isspace), item.end());
        
        if (!item.empty()) {
            try {
                int class_id = std::stoi(item);
                result.push_back(class_id);
            } catch (const std::exception& e) {
                LOG_ERROR("Invalid class ID: %s", item.c_str());
            }
        }
    }
    
    return result;
}

std::vector<int> RoiDetector::parseRoiIdsString(const std::string& rois_str) {
    std::vector<int> result;
    std::stringstream ss(rois_str);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // 去除空格
        item.erase(remove_if(item.begin(), item.end(), ::isspace), item.end());
        
        if (!item.empty()) {
            try {
                int roi_id = std::stoi(item);
                result.push_back(roi_id);
            } catch (const std::exception& e) {
                LOG_ERROR("Invalid ROI ID: %s", item.c_str());
            }
        }
    }
    
    return result;
}

cv::Scalar RoiDetector::generateColorByClassId(int class_id) {
    // 根据类别ID生成一个稳定的颜色（相同类别始终是相同颜色）
    srand(class_id * 123456);  // 用类别ID作为随机种子
    int b = 100 + rand() % 155;
    int g = 100 + rand() % 155;
    int r = 100 + rand() % 155;
    return cv::Scalar(b, g, r);
}

bool RoiDetector::loadConfig() {
    // 从配置文件加载检测阈值
    detection_threshold = rk_param_get_float("ai.roi:detection_threshold", 0.4f);
    
    // 清空现有配置
    roi_areas.clear();
    roi_groups.clear();
    roi_to_group.clear();
    group_classes.clear();
    
    // 检查ROI功能是否启用
    int roi_enable = rk_param_get_int("ai.roi:enable", 0);
    if (!roi_enable) {
        LOG_INFO("ROI detection is disabled");
        return false;
    }
    
    // 读取有多少个ROI区域和组
    int roi_count = rk_param_get_int("ai.roi:count", 0);
    int group_count = rk_param_get_int("ai.roi:groups", 0);
    
    LOG_INFO("Loading %d ROI areas and %d ROI groups", roi_count, group_count);
    
    // 先加载所有ROI区域
    for (int i = 0; i < roi_count; i++) {
        RoiArea roi;
        char param_name[64];
        
        // 读取ROI基础信息
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:enabled", i);
        roi.enabled = rk_param_get_int(param_name, 1) != 0;
        
        if (!roi.enabled) {
            continue;
        }
        
        roi.id = i;
        roi.group_id = -1;  // 默认不属于任何组
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:name", i);
        roi.name = rk_param_get_string(param_name, "");
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:x", i);
        roi.x = rk_param_get_int(param_name, 0);
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:y", i);
        roi.y = rk_param_get_int(param_name, 0);
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:width", i);
        roi.width = rk_param_get_int(param_name, 100);
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:height", i);
        roi.height = rk_param_get_int(param_name, 100);
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:stay_time", i);
        roi.stay_time = rk_param_get_int(param_name, 0);
        
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:cooldown_time", i);
        roi.cooldown_time = rk_param_get_int(param_name, 10);
        
        // 读取ROI关注的目标类别（如果没有组，单个ROI可以有自己的类别）
        snprintf(param_name, sizeof(param_name), "ai.roi.%d:classes", i);
        std::string classes_str = rk_param_get_string(param_name, "");
        
        if (!classes_str.empty()) {
            roi.classes = parseClassesString(classes_str);
        } else {
            // 如果没有指定类别，保持classes为空，稍后从组中获取
            roi.classes.clear();
        }
        
        // 设置ROI颜色（随机颜色）
        int b = 100 + rand() % 155;
        int g = 100 + rand() % 155;
        int r = 100 + rand() % 155;
        roi.color = cv::Scalar(b, g, r);
        
        roi_areas.push_back(roi);
        
        LOG_INFO("Loaded ROI %d: %s (%d,%d,%d,%d)", 
                 roi.id, roi.name.c_str(), roi.x, roi.y, roi.width, roi.height);
    }
    
    // 加载ROI组
    for (int i = 0; i < group_count; i++) {
        RoiGroup group;
        char param_name[64];
        
        group.id = i;
        
        // 读取组名称
        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:name", i);
        std::string default_name = "Group " + std::to_string(i);
        group.name = rk_param_get_string(param_name, default_name.c_str());
        
        // 读取组关注的目标类别
        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:classes", i);
        std::string classes_str = rk_param_get_string(param_name, "");
        
        group.classes = parseClassesString(classes_str);
        
        // 读取组包含的ROI ID列表
        snprintf(param_name, sizeof(param_name), "ai.roi.group.%d:rois", i);
        std::string rois_str = rk_param_get_string(param_name, "");
        
        group.roi_ids = parseRoiIdsString(rois_str);
        
        // 设置组颜色（使用第一个类别的颜色）
        if (!group.classes.empty()) {
            group.color = generateColorByClassId(group.classes[0]);
        } else {
            int b = 100 + rand() % 155;
            int g = 100 + rand() % 155;
            int r = 100 + rand() % 155;
            group.color = cv::Scalar(b, g, r);
        }
        
        // 建立ROI与组的关联关系
        for (int roi_id : group.roi_ids) {
            // 检查ROI ID是否有效
            bool roi_found = false;
            for (auto& roi : roi_areas) {
                if (roi.id == roi_id) {
                    roi.group_id = group.id;  // 设置ROI所属的组ID
                    
                    // 如果ROI没有自己的类别，使用组的类别
                    if (roi.classes.empty()) {
                        roi.classes = group.classes;
                    }
                    
                    // 使用组的颜色
                    roi.color = group.color;
                    
                    roi_to_group[roi_id] = group.id;
                    roi_found = true;
                    break;
                }
            }
            
            if (!roi_found) {
                LOG_ERROR("Invalid ROI ID %d in group %d", roi_id, group.id);
            }
        }
        
        // 构建组关注的类别集合（用于快速查找）
        std::unordered_set<int> class_set(group.classes.begin(), group.classes.end());
        group_classes[group.id] = class_set;
        
        roi_groups.push_back(group);
        
        LOG_INFO("Loaded ROI Group %d: %s with %zu ROIs and %zu classes", 
                 group.id, group.name.c_str(), group.roi_ids.size(), group.classes.size());
    }
    
    return !roi_areas.empty();
}

bool RoiDetector::reloadConfig() {
    LOG_INFO("Reloading ROI configuration...");
    
    // 暂存旧配置，如果加载失败可以回滚
    auto old_threshold = detection_threshold;
    auto old_roi_areas = roi_areas;
    auto old_roi_groups = roi_groups;
    auto old_roi_to_group = roi_to_group;
    auto old_group_classes = group_classes;
    
    // 尝试加载新配置
    if (!loadConfig()) {
        LOG_ERROR("Failed to reload configuration, rolling back");
        detection_threshold = old_threshold;
        roi_areas = old_roi_areas;
        roi_groups = old_roi_groups;
        roi_to_group = old_roi_to_group;
        group_classes = old_group_classes;
        return false;
    }
    
    LOG_INFO("Configuration reloaded successfully");
    return true;
}

void RoiDetector::processDetectionResult(const cv::Mat& frame, object_detect_result_list& od_results) {
    // 转换检测结果为ByteTrack可接受的格式
    std::vector<Object> detections;
    
    for (int i = 0; i < od_results.count; i++) {
        object_detect_result* det = &od_results.results[i];
        
        // 检查置信度是否满足阈值要求
        if (det->prop < detection_threshold) {
            continue;
        }
        
        // 检查是否有ROI或ROI组关注这个类别
        bool class_monitored = false;
        
        // 首先检查独立ROI（不属于任何组）
        for (const auto& roi : roi_areas) {
            if (roi.enabled && roi.group_id == -1 && 
                std::find(roi.classes.begin(), roi.classes.end(), det->cls_id) != roi.classes.end()) {
                class_monitored = true;
                break;
            }
        }
        
        // 然后检查ROI组
        if (!class_monitored) {
            for (const auto& group : roi_groups) {
                if (std::find(group.classes.begin(), group.classes.end(), det->cls_id) != group.classes.end()) {
                    class_monitored = true;
                    break;
                }
            }
        }
        
        if (!class_monitored) {
            continue;
        }
        
        Object obj;
        obj.rect = cv::Rect(det->box.left, det->box.top, 
                           det->box.right - det->box.left, 
                           det->box.bottom - det->box.top);
        obj.prob = det->prop;
        obj.label = det->cls_id;
        
        detections.push_back(obj);
    }
    
    // 使用ByteTrack进行目标跟踪
    auto tracked_objects = tracker->update(detections);
    
    // 更新跟踪状态
    std::unordered_set<int> current_track_ids;
    
    for (const auto& obj : tracked_objects) {
        int track_id = obj.track_id;
        current_track_ids.insert(track_id);
        
        // 更新或创建目标状态
        if (this->tracked_objects.find(track_id) == this->tracked_objects.end()) {
            RoiObject roi_obj;
            roi_obj.track_id = track_id;
            roi_obj.class_id = obj.label;
            roi_obj.box = obj.rect;
            roi_obj.confidence = obj.prob;
            roi_obj.in_roi = false;
            roi_obj.roi_id = -1;
            roi_obj.group_id = -1;
            roi_obj.alarm_triggered = false;
            
            this->tracked_objects[track_id] = roi_obj;
        } else {
            // 更新现有目标
            auto& roi_obj = this->tracked_objects[track_id];
            roi_obj.box = obj.rect;
            roi_obj.confidence = obj.prob;
            roi_obj.class_id = obj.label; // 更新类别ID，防止跟踪算法错误匹配
        }
    }
    
    // 检查每个目标是否在ROI内
    for (auto& [track_id, obj] : this->tracked_objects) {
        // 如果目标不在当前帧的跟踪列表中，跳过
        if (current_track_ids.find(track_id) == current_track_ids.end()) {
            continue;
        }
        
        bool was_in_roi = obj.in_roi;
        int prev_roi_id = obj.roi_id;
        int prev_group_id = obj.group_id;
        
        obj.in_roi = false;
        obj.roi_id = -1;
        obj.group_id = -1;
        
        // 检查目标是否在任何ROI内
        for (const auto& roi : roi_areas) {
            if (!roi.enabled) {
                continue;
            }
            
            // 检查目标类别是否被该ROI关注
            bool class_monitored = false;
            
            // 如果ROI属于组，检查类别是否在组中被监控
            if (roi.group_id != -1) {
                const auto& group_it = group_classes.find(roi.group_id);
                if (group_it != group_classes.end()) {
                    class_monitored = group_it->second.count(obj.class_id) > 0;
                }
            } else {
                // 如果ROI不属于组，检查其自身是否关注该类别
                class_monitored = std::find(roi.classes.begin(), roi.classes.end(), obj.class_id) != roi.classes.end();
            }
            
            if (!class_monitored) {
                continue;
            }
            
            // 判断目标是否在ROI内
            if (isObjectInRoi(obj.box, roi)) {
                obj.in_roi = true;
                obj.roi_id = roi.id;
                obj.group_id = roi.group_id;  // 可能为-1，表示不属于任何组
                
                // 如果是首次进入ROI，或者进入了不同的ROI，记录时间
                if (!was_in_roi || prev_roi_id != roi.id || prev_group_id != roi.group_id) {
                    obj.first_seen = std::chrono::steady_clock::now();
                    obj.alarm_triggered = false;
                }
                
                // 检查是否需要触发告警
                checkAlarm(frame, track_id);
                break;
            }
        }
        
        // 如果目标离开了ROI，重置状态
        if (was_in_roi && !obj.in_roi) {
            obj.alarm_triggered = false;
        }
    }
    
    // 清理过期的目标
    cleanExpiredObjects();
}

bool RoiDetector::isObjectInRoi(const cv::Rect& obj_box, const RoiArea& roi) {
    cv::Rect roi_rect(roi.x, roi.y, roi.width, roi.height);
    
    // 计算目标中心点
    cv::Point2f obj_center(
        obj_box.x + obj_box.width / 2.0f,
        obj_box.y + obj_box.height / 2.0f
    );
    
    // 检查中心点是否在ROI内
    if (roi_rect.contains(obj_center)) {
        return true;
    }
    
    // 计算交集面积
    cv::Rect intersection = roi_rect & obj_box;
    float intersection_area = intersection.area();
    float obj_area = obj_box.area();
    
    // 如果交集面积超过物体面积的30%，认为物体在ROI内
    return (intersection_area / obj_area) > 0.3f;
}

bool RoiDetector::isClassInGroup(int class_id, const RoiGroup& group) {
    return std::find(group.classes.begin(), group.classes.end(), class_id) != group.classes.end();
}

int RoiDetector::getRoiGroup(int roi_id) {
    auto it = roi_to_group.find(roi_id);
    if (it != roi_to_group.end()) {
        return it->second;
    }
    return -1;
}

void RoiDetector::checkAlarm(const cv::Mat& frame, int track_id) {
    auto& obj = tracked_objects[track_id];
    if (obj.alarm_triggered) {
        return; // 已经触发过告警，等待冷却
    }
    
    // 获取当前ROI区域
    const RoiArea* roi = nullptr;
    for (const auto& r : roi_areas) {
        if (r.id == obj.roi_id) {
            roi = &r;
            break;
        }
    }
    
    if (!roi) {
        return; // 未找到匹配的ROI
    }
    
    // 获取当前组（如果有）
    const RoiGroup* group = nullptr;
    if (obj.group_id >= 0) {
        for (const auto& g : roi_groups) {
            if (g.id == obj.group_id) {
                group = &g;
                break;
            }
        }
    }
    
    auto now = std::chrono::steady_clock::now();
    
    // 检查是否满足停留时间要求
    auto stay_duration = std::chrono::duration_cast<std::chrono::seconds>(now - obj.first_seen).count();
    if (stay_duration < roi->stay_time) {
        return; // 停留时间不够
    }
    
    // 检查是否在冷却期内
    auto last_alarm_duration = std::chrono::duration_cast<std::chrono::seconds>(now - obj.last_alarm).count();
    if (obj.last_alarm != std::chrono::steady_clock::time_point() && 
        last_alarm_duration < roi->cooldown_time) {
        return; // 在冷却期内
    }
    
    // 触发告警
    obj.alarm_triggered = true;
    obj.last_alarm = now;
    
    // 如果注册了告警回调，调用它
    if (alarm_callback) {
        AlarmInfo alarm;
        alarm.roi_id = obj.roi_id;
        alarm.roi_name = roi->name;
        alarm.group_id = obj.group_id;
        alarm.group_name = group ? group->name : "";
        alarm.track_id = obj.track_id;
        alarm.class_id = obj.class_id;
        alarm.class_name = std::string(coco_cls_to_name(obj.class_id));
        alarm.box = obj.box;
        alarm.confidence = obj.confidence;
        alarm.timestamp = std::chrono::system_clock::now();
        
        // 裁剪当前帧作为告警截图
        cv::Rect crop_rect = obj.box;
        // 扩大截图范围，包括更多上下文
        crop_rect.x = std::max(0, crop_rect.x - crop_rect.width / 4);
        crop_rect.y = std::max(0, crop_rect.y - crop_rect.height / 4);
        crop_rect.width = std::min(frame.cols - crop_rect.x, crop_rect.width * 3 / 2);
        crop_rect.height = std::min(frame.rows - crop_rect.y, crop_rect.height * 3 / 2);
        
        alarm.snapshot = frame(crop_rect).clone();
        
        // 调用告警回调
        alarm_callback(alarm);
        
        if (group) {
            LOG_INFO("Alarm triggered: Group %d (%s), ROI %d (%s), Class %s, Track ID %d", 
                    obj.group_id, group->name.c_str(), obj.roi_id, roi->name.c_str(), 
                    coco_cls_to_name(obj.class_id), obj.track_id);
        } else {
            LOG_INFO("Alarm triggered: ROI %d (%s), Class %s, Track ID %d", 
                    obj.roi_id, roi->name.c_str(), coco_cls_to_name(obj.class_id), obj.track_id);
        }
    }
}

cv::Mat RoiDetector::drawResults(const cv::Mat& frame) {
    cv::Mat result = frame.clone();
    
    // 绘制ROI组和区域
    // 先绘制组，以便单独的ROI可以覆盖在上面
    for (const auto& group : roi_groups) {
        // 为每个组绘制一个半透明的背景，包含所有ROI
        cv::Mat overlay = result.clone();
        
        for (int roi_id : group.roi_ids) {
            const RoiArea* roi = nullptr;
            for (const auto& r : roi_areas) {
                if (r.id == roi_id && r.enabled) {
                    roi = &r;
                    break;
                }
            }
            
            if (roi) {
                cv::Rect roi_rect(roi->x, roi->y, roi->width, roi->height);
                cv::rectangle(overlay, roi_rect, group.color, -1);  // 填充矩形
            }
        }
        
        // 应用半透明效果
        cv::addWeighted(overlay, 0.2, result, 0.8, 0, result);
        
        // 绘制组名称（在所有ROI区域的中心位置）
        if (!group.roi_ids.empty()) {
            // 计算组内所有ROI的中心点
            int center_x = 0, center_y = 0;
            int count = 0;
            
            for (int roi_id : group.roi_ids) {
                const RoiArea* roi = nullptr;
                for (const auto& r : roi_areas) {
                    if (r.id == roi_id && r.enabled) {
                        roi = &r;
                        break;
                    }
                }
                
                if (roi) {
                    center_x += roi->x + roi->width / 2;
                    center_y += roi->y + roi->height / 2;
                    count++;
                }
            }
            
            if (count > 0) {
                center_x /= count;
                center_y /= count;
                
                // 在组的中心位置显示组名
                cv::putText(result, group.name, cv::Point(center_x, center_y),
                            cv::FONT_HERSHEY_SIMPLEX, 1.0, group.color, 2);
                
                // 显示组关注的类别
                std::string classes_str = "Classes: ";
                for (size_t i = 0; i < std::min(group.classes.size(), size_t(3)); i++) {
                    classes_str += std::string(coco_cls_to_name(group.classes[i])) + ", ";
                }
                if (group.classes.size() > 3) {
                    classes_str += "...";
                } else if (!group.classes.empty()) {
                    classes_str = classes_str.substr(0, classes_str.length() - 2);
                }
                
                cv::putText(result, classes_str, cv::Point(center_x, center_y + 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.7, group.color, 2);
            }
        }
    }
    
    // 绘制单独的ROI区域（不属于任何组）
    for (const auto& roi : roi_areas) {
        if (!roi.enabled || roi.group_id != -1) {
            continue;
        }
        
        cv::Rect roi_rect(roi.x, roi.y, roi.width, roi.height);
        cv::rectangle(result, roi_rect, roi.color, 2);
        
        // 绘制ROI名称及其关注的类别
        std::string classes_str = "Classes: ";
        for (size_t i = 0; i < std::min(roi.classes.size(), size_t(3)); i++) {
            classes_str += std::string(coco_cls_to_name(roi.classes[i])) + ", ";
        }
        if (roi.classes.size() > 3) {
            classes_str += "...";
        } else if (!roi.classes.empty()) {
            classes_str = classes_str.substr(0, classes_str.length() - 2);
        }
        
        cv::putText(result, roi.name, cv::Point(roi.x, roi.y - 25),
                   cv::FONT_HERSHEY_SIMPLEX, 0.7, roi.color, 2);
        cv::putText(result, classes_str, cv::Point(roi.x, roi.y - 5),
                   cv::FONT_HERSHEY_SIMPLEX, 0.5, roi.color, 2);
    }
    
    // 绘制组内的ROI区域
    for (const auto& roi : roi_areas) {
        if (!roi.enabled || roi.group_id == -1) {
            continue;
        }
        
        cv::Rect roi_rect(roi.x, roi.y, roi.width, roi.height);
        cv::rectangle(result, roi_rect, roi.color, 2);
        
        // 绘制ROI名称
        cv::putText(result, roi.name, cv::Point(roi.x, roi.y - 5),
                   cv::FONT_HERSHEY_SIMPLEX, 0.7, roi.color, 2);
    }
    
    // 绘制跟踪的目标
    for (const auto& [track_id, obj] : tracked_objects) {
        // 根据状态选择颜色
        cv::Scalar color;
        if (obj.alarm_triggered) {
            color = cv::Scalar(0, 0, 255); // 红色，已触发告警
        } else if (obj.in_roi) {
            color = cv::Scalar(255, 165, 0); // 橙色，在ROI内但未触发告警
        } else {
            color = cv::Scalar(0, 255, 0); // 绿色，不在ROI内
        }
        
        cv::rectangle(result, obj.box, color, 2);
        
        // 显示更多信息
        std::string label = std::string(coco_cls_to_name(obj.class_id)) + 
                          " - ID:" + std::to_string(track_id) + 
                          " - " + std::to_string(int(obj.confidence * 100)) + "%";
        
        if (obj.in_roi) {
            auto now = std::chrono::steady_clock::now();
            auto stay_time = std::chrono::duration_cast<std::chrono::seconds>(now - obj.first_seen).count();
            label += " - Stay:" + std::to_string(stay_time) + "s";
            
            // 获取ROI和组的名称
            if (obj.roi_id >= 0) {
                for (const auto& roi : roi_areas) {
                    if (roi.id == obj.roi_id) {
                        label += " - ROI:" + roi.name;
                        break;
                    }
                }
            }
            
            if (obj.group_id >= 0) {
                for (const auto& group : roi_groups) {
                    if (group.id == obj.group_id) {
                        label += " - Group:" + group.name;
                        break;
                    }
                }
            }
        }
        
        cv::putText(result, label, cv::Point(obj.box.x, obj.box.y - 5),
                   cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 2);
    }
    
    return result;
}

std::vector<int> RoiDetector::getDetectionClasses() {
    std::unordered_set<int> unique_classes;
    
    // 收集所有组中的类别
    for (const auto& group : roi_groups) {
        unique_classes.insert(group.classes.begin(), group.classes.end());
    }
    
    // 收集所有独立ROI（不属于任何组）的类别
    for (const auto& roi : roi_areas) {
        if (roi.enabled && roi.group_id == -1) {
            unique_classes.insert(roi.classes.begin(), roi.classes.end());
        }
    }
    
    std::vector<int> classes(unique_classes.begin(), unique_classes.end());
    return classes;
}

void RoiDetector::registerAlarmCallback(AlarmCallback callback) {
    alarm_callback = callback;
}

void RoiDetector::cleanExpiredObjects() {
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = tracked_objects.begin(); it != tracked_objects.end(); ) {
        auto& obj = it->second;
        auto last_seen = tracker->getTrackLastSeen(obj.track_id);
        
        // 如果跟踪器已经5秒没有看到这个目标，删除它
        if (last_seen && std::chrono::duration_cast<std::chrono::seconds>(now - *last_seen).count() > 5) {
            it = tracked_objects.erase(it);
        } else {
            ++it;
        }
    }
}
