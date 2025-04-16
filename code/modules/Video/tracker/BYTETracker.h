#ifndef BYTETRACKER_H
#define BYTETRACKER_H

#include <vector>
#include <memory>
#include <map>
#include <opencv2/opencv.hpp>
#include <chrono>

struct Object {
    cv::Rect rect;
    int label;
    float prob;
    int track_id;
};

struct BYTETrackerParams {
    float track_thresh;     // 跟踪阈值
    float high_thresh;      // 高质量跟踪阈值
    float match_thresh;     // 匹配阈值
    int track_buffer;       // 跟踪缓冲区大小
    int frame_rate;         // 帧率
    
    BYTETrackerParams() : 
        track_thresh(0.5),
        high_thresh(0.6),
        match_thresh(0.8),
        track_buffer(30),
        frame_rate(30) {}
};

// 用于表示跟踪状态
enum TrackState { New = 0, Tracked, Lost, Removed };

// 表示单个跟踪目标的类
class STrack {
public:
    STrack(const cv::Rect& rect, float score, int class_id);
    ~STrack() = default;
    
    // 更新跟踪目标
    void updateTrack(const cv::Rect& rect, float score, int class_id);
    
    // 获取目标状态
    TrackState state() const { return _state; }
    int trackId() const { return _track_id; }
    cv::Rect getRect() const { return _rect; }
    float getScore() const { return _score; }
    int getClassId() const { return _class_id; }
    
    // 使用指针代替std::optional
    std::chrono::steady_clock::time_point* getLastSeen() const { return _last_seen.get(); }
    
    // 标记为"已跟踪"
    void markTracked();
    // 标记为"丢失"
    void markLost();
    // 标记为"已删除"
    void markRemoved();
    
private:
    static int next_id;
    int _track_id;
    cv::Rect _rect;
    float _score;
    int _class_id;
    TrackState _state;
    
    // 使用unique_ptr代替std::optional
    std::unique_ptr<std::chrono::steady_clock::time_point> _last_seen;
};

// ByteTrack跟踪器类
class BYTETracker {
public:
    BYTETracker(const BYTETrackerParams& params = BYTETrackerParams());
    ~BYTETracker() = default;
    
    // 更新跟踪器，传入检测结果，返回跟踪结果
    std::vector<Object> update(const std::vector<Object>& objects);
    
    // 使用指针代替std::optional
    std::chrono::steady_clock::time_point* getTrackLastSeen(int track_id);
    
private:
    BYTETrackerParams params;
    std::vector<std::shared_ptr<STrack>> tracked_stracks;
    std::vector<std::shared_ptr<STrack>> lost_stracks;
    std::vector<std::shared_ptr<STrack>> removed_stracks;
    
    // 匹配跟踪目标与检测目标
    void match(const std::vector<std::shared_ptr<STrack>>& detections);
    
    // 计算IoU距离
    float iou_distance(const std::shared_ptr<STrack>& track, const std::shared_ptr<STrack>& detect);
    
    // 线性分配问题求解
    void linear_assignment(const std::vector<std::vector<float>>& cost_matrix, 
                          float thresh,
                          std::vector<std::pair<int, int>>& matches,
                          std::vector<int>& unmatched_a,
                          std::vector<int>& unmatched_b);
};

#endif // BYTETRACKER_H
