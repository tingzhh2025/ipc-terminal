#include "BYTETracker.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

// 静态成员初始化
int STrack::next_id = 1;

STrack::STrack(const cv::Rect& rect, float score, int class_id) :
    _track_id(next_id++),
    _rect(rect),
    _score(score),
    _class_id(class_id),
    _state(TrackState::New) {
        _last_seen = std::make_unique<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    }

void STrack::updateTrack(const cv::Rect& rect, float score, int class_id) {
    _rect = rect;
    _score = score;
    _class_id = class_id;
    _last_seen = std::make_unique<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
}

void STrack::markTracked() {
    _state = TrackState::Tracked;
    _last_seen = std::make_unique<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
}

void STrack::markLost() {
    _state = TrackState::Lost;
}

void STrack::markRemoved() {
    _state = TrackState::Removed;
}

BYTETracker::BYTETracker(const BYTETrackerParams& params) : params(params) {}

std::vector<Object> BYTETracker::update(const std::vector<Object>& objects) {
    // 创建结果容器
    std::vector<Object> results;
    
    // 创建当前检测的ID集合
    std::unordered_set<int> current_ids;
    
    // 清理过期的跟踪目标 - 这里大幅减少保留时间到0.5秒
    auto now = std::chrono::steady_clock::now();
    tracked_stracks.erase(
        std::remove_if(tracked_stracks.begin(), tracked_stracks.end(), 
            [&now](const std::shared_ptr<STrack>& track) {
                auto last_seen = track->getLastSeen();
                if (!last_seen) return true;
                
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_seen).count();
                return duration > 500; // 只保留0.5秒内看到的目标
            }
        ), 
        tracked_stracks.end()
    );
    
    // 处理每个检测结果
    for (const auto& obj : objects) {
        bool matched = false;
        std::shared_ptr<STrack> best_match;
        float best_iou = 0.0f;
        
        // 尝试匹配现有的跟踪目标 - 使用更宽松的匹配条件
        for (auto& track : tracked_stracks) {
            // 只匹配相同类型的物体
            if (track->getClassId() != obj.label) {
                continue;
            }
            
            // 计算IoU
            cv::Rect rect1 = track->getRect();
            cv::Rect rect2 = obj.rect;
            
            // 计算重叠区域
            int x1 = std::max(rect1.x, rect2.x);
            int y1 = std::max(rect1.y, rect2.y);
            int x2 = std::min(rect1.x + rect1.width, rect2.x + rect2.width);
            int y2 = std::min(rect1.y + rect1.height, rect2.y + rect2.height);
            
            int overlap_w = std::max(0, x2 - x1);
            int overlap_h = std::max(0, y2 - y1);
            float overlap_area = overlap_w * overlap_h;
            
            // 计算两个矩形的面积
            float area1 = rect1.width * rect1.height;
            float area2 = rect2.width * rect2.height;
            
            // 计算IoU
            float iou = overlap_area / (area1 + area2 - overlap_area);
            
            // 使用更宽松的IoU阈值 (0.3)
            if (iou > 0.3f && iou > best_iou) {
                best_iou = iou;
                best_match = track;
                matched = true;
            }
        }
        
        // 如果找到匹配，则更新跟踪目标
        if (matched) {
            // 立即更新位置，不使用任何平滑或预测
            best_match->updateTrack(obj.rect, obj.prob, obj.label);
            best_match->markTracked();
            
            Object result;
            result.rect = best_match->getRect();
            result.prob = best_match->getScore();
            result.label = best_match->getClassId();
            result.track_id = best_match->trackId();
            
            results.push_back(result);
            current_ids.insert(best_match->trackId());
        } 
        // 如果没有找到匹配，则创建新的跟踪目标
        else {
            auto new_track = std::make_shared<STrack>(obj.rect, obj.prob, obj.label);
            new_track->markTracked();
            tracked_stracks.push_back(new_track);
            
            Object result;
            result.rect = new_track->getRect();
            result.prob = new_track->getScore();
            result.label = new_track->getClassId();
            result.track_id = new_track->trackId();
            
            results.push_back(result);
            current_ids.insert(new_track->trackId());
        }
    }
    
    return results;
}

std::chrono::steady_clock::time_point* BYTETracker::getTrackLastSeen(int track_id) {
    // 在跟踪中的目标中搜索
    for (const auto& track : tracked_stracks) {
        if (track->trackId() == track_id) {
            return track->getLastSeen();
        }
    }
    
    return nullptr;
}
