#ifndef CONFIDENCE_SMOOTHER_H
#define CONFIDENCE_SMOOTHER_H

#include <vector>
#include <unordered_map>
#include <chrono>

// 记录每个目标的历史置信度，用于平滑显示
struct ConfidenceHistory {
    std::vector<float> history;
    std::chrono::steady_clock::time_point last_update;
    
    // 添加新的置信度到历史记录中
    void add(float conf) {
        // 限制历史记录长度，保留最近5帧数据
        const size_t MAX_HISTORY = 5;
        if (history.size() >= MAX_HISTORY) {
            history.erase(history.begin());
        }
        history.push_back(conf);
        last_update = std::chrono::steady_clock::now();
    }
    
    // 获取平滑后的置信度
    float get_smoothed() const {
        if (history.empty()) {
            return 0.0f;
        }
        
        float sum = 0.0f;
        for (float conf : history) {
            sum += conf;
        }
        return sum / history.size();
    }
    
    // 检查记录是否过期
    bool is_expired() const {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_update).count();
        return duration > 2; // 2秒不更新认为目标已消失
    }
};

// 使用目标类别+位置作为key的结构
struct ObjectKey {
    int class_id;
    int x;
    int y;
    int width;
    int height;
    
    bool operator==(const ObjectKey& other) const {
        // 位置允许有一定范围的偏差，认为是同一个目标
        const int POS_THRESHOLD = 50;
        const int SIZE_THRESHOLD = 50;
        
        return class_id == other.class_id &&
               std::abs(x - other.x) < POS_THRESHOLD &&
               std::abs(y - other.y) < POS_THRESHOLD &&
               std::abs(width - other.width) < SIZE_THRESHOLD &&
               std::abs(height - other.height) < SIZE_THRESHOLD;
    }
};

// 自定义哈希函数
struct ObjectKeyHash {
    std::size_t operator()(const ObjectKey& key) const {
        // 简化版哈希函数：将位置四舍五入到最接近50的倍数
        return std::hash<int>{}(key.class_id) ^ 
               std::hash<int>{}(key.x / 50 * 50) ^ 
               std::hash<int>{}(key.y / 50 * 50) ^
               std::hash<int>{}(key.width / 50 * 50) ^
               std::hash<int>{}(key.height / 50 * 50);
    }
};

// 记录每个目标的历史置信度
static std::unordered_map<ObjectKey, ConfidenceHistory, ObjectKeyHash> confidence_history;

// 清理过期的置信度记录
static void clean_expired_confidence_history() {
    auto it = confidence_history.begin();
    while (it != confidence_history.end()) {
        if (it->second.is_expired()) {
            it = confidence_history.erase(it);
        } else {
            ++it;
        }
    }
}

// 添加和获取平滑置信度的辅助函数
static float get_smoothed_confidence(int class_id, int x, int y, int w, int h, float raw_confidence) {
    ObjectKey key = {class_id, x, y, w, h};
    confidence_history[key].add(raw_confidence);
    return confidence_history[key].get_smoothed();
}

#endif // CONFIDENCE_SMOOTHER_H
