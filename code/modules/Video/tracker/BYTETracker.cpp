#include "BYTETracker.h"
#include <algorithm>
#include <cmath>
#include <numeric>

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
    // 转换检测结果为STrack对象
    std::vector<std::shared_ptr<STrack>> detections;
    for (const auto& obj : objects) {
        detections.push_back(std::make_shared<STrack>(obj.rect, obj.prob, obj.label));
    }
    
    // 匹配跟踪目标与检测目标
    match(detections);
    
    // 返回当前所有跟踪中的目标
    std::vector<Object> results;
    for (const auto& track : tracked_stracks) {
        if (track->state() == TrackState::Tracked) {
            Object obj;
            obj.rect = track->getRect();
            obj.prob = track->getScore();
            obj.label = track->getClassId();
            obj.track_id = track->trackId();
            results.push_back(obj);
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
    
    // 在丢失目标中搜索
    for (const auto& track : lost_stracks) {
        if (track->trackId() == track_id) {
            return track->getLastSeen();
        }
    }
    
    return nullptr;
}

void BYTETracker::match(const std::vector<std::shared_ptr<STrack>>& detections) {
    std::vector<std::shared_ptr<STrack>> activated_stracks;
    std::vector<std::shared_ptr<STrack>> refind_stracks;
    std::vector<std::shared_ptr<STrack>> removed_stracks;
    std::vector<std::shared_ptr<STrack>> lost_stracks_in_frame;
    
    // 分离出跟踪中和丢失中的目标
    std::vector<std::shared_ptr<STrack>> tracked_stracks_curr;
    for (const auto& track : tracked_stracks) {
        if (track->state() == TrackState::Tracked) {
            tracked_stracks_curr.push_back(track);
        }
    }
    
    // 第一阶段匹配：跟踪中的目标 vs 高质量检测结果
    std::vector<std::shared_ptr<STrack>> high_score_dets;
    std::vector<std::shared_ptr<STrack>> low_score_dets;
    
    for (const auto& det : detections) {
        if (det->getScore() >= params.high_thresh) {
            high_score_dets.push_back(det);
        } else {
            low_score_dets.push_back(det);
        }
    }
    
    // 计算跟踪目标和高质量检测之间的IoU距离矩阵
    std::vector<std::vector<float>> iou_dists(tracked_stracks_curr.size(), 
                                             std::vector<float>(high_score_dets.size()));
    
    for (size_t i = 0; i < tracked_stracks_curr.size(); ++i) {
        for (size_t j = 0; j < high_score_dets.size(); ++j) {
            iou_dists[i][j] = iou_distance(tracked_stracks_curr[i], high_score_dets[j]);
        }
    }
    
    // 匹配高质量检测与跟踪目标
    std::vector<std::pair<int, int>> matches;
    std::vector<int> unmatched_tracks;
    std::vector<int> unmatched_detects;
    
    linear_assignment(iou_dists, params.match_thresh, matches, unmatched_tracks, unmatched_detects);
    
    // 处理匹配结果
    for (auto& [track_idx, det_idx] : matches) {
        auto& track = tracked_stracks_curr[track_idx];
        auto& det = high_score_dets[det_idx];
        track->updateTrack(det->getRect(), det->getScore(), det->getClassId());
        track->markTracked();
        activated_stracks.push_back(track);
    }
    
    // 处理未匹配上的跟踪目标
    for (int idx : unmatched_tracks) {
        auto& track = tracked_stracks_curr[idx];
        track->markLost();
        lost_stracks_in_frame.push_back(track);
    }
    
    // 处理未匹配的检测结果，创建新的跟踪目标
    for (int idx : unmatched_detects) {
        auto& det = high_score_dets[idx];
        if (det->getScore() >= params.track_thresh) {
            activated_stracks.push_back(det);
        }
    }
    
    // 处理所有低分检测，也创建新的跟踪目标
    for (const auto& det : low_score_dets) {
        if (det->getScore() >= params.track_thresh) {
            activated_stracks.push_back(det);
        }
    }
    
    // 处理已经丢失太久的目标
    for (const auto& track : lost_stracks_in_frame) {
        if (track->state() == TrackState::Lost) {
            // 判断是否已经丢失太久
            bool lost_too_long = false;
            auto last_seen = track->getLastSeen();
            if (last_seen) {
                auto now = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - *last_seen);
                lost_too_long = duration.count() > params.track_buffer;
            }
            
            if (lost_too_long) {
                track->markRemoved();
                removed_stracks.push_back(track);
            } else {
                tracked_stracks.push_back(track);
            }
        }
    }
    
    // 合并 tracked_stracks, activated_stracks和refind_stracks
    for (const auto& track : tracked_stracks_curr) {
        if (track->state() == TrackState::Tracked) {
            tracked_stracks.push_back(track);
        }
    }
    for (const auto& track : refind_stracks) {
        tracked_stracks.push_back(track);
    }
    
    // 更新丢失目标列表
    for (const auto& track : lost_stracks_in_frame) {
        if (track->state() == TrackState::Lost) {
            lost_stracks.push_back(track);
        }
    }
    
    // 添加被删除的目标到removed_stracks列表
    for (const auto& track : removed_stracks) {
        removed_stracks.push_back(track);
    }
    
    // 去重处理
    // ...（这里可以添加去重逻辑，但为了简化代码暂时省略）
}

float BYTETracker::iou_distance(const std::shared_ptr<STrack>& track, const std::shared_ptr<STrack>& detect) {
    // 计算IoU距离 (1 - IoU)
    cv::Rect rect1 = track->getRect();
    cv::Rect rect2 = detect->getRect();
    
    int x1 = std::max(rect1.x, rect2.x);
    int y1 = std::max(rect1.y, rect2.y);
    int x2 = std::min(rect1.x + rect1.width, rect2.x + rect2.width);
    int y2 = std::min(rect1.y + rect1.height, rect2.y + rect2.height);
    
    int w = std::max(0, x2 - x1);
    int h = std::max(0, y2 - y1);
    
    float inter_area = w * h;
    float union_area = rect1.width * rect1.height + rect2.width * rect2.height - inter_area;
    
    if (union_area == 0) return 1.0f;
    
    float iou = inter_area / union_area;
    return 1.0f - iou;
}

void BYTETracker::linear_assignment(const std::vector<std::vector<float>>& cost_matrix, 
                                  float thresh,
                                  std::vector<std::pair<int, int>>& matches,
                                  std::vector<int>& unmatched_a,
                                  std::vector<int>& unmatched_b) {
    // 简单贪心匹配的实现
    matches.clear();
    unmatched_a.clear();
    unmatched_b.clear();
    
    const size_t rows = cost_matrix.size();
    if (rows == 0) return;
    
    const size_t cols = cost_matrix[0].size();
    if (cols == 0) return;
    
    // 初始化所有行和列为未匹配状态
    std::vector<bool> row_matched(rows, false);
    std::vector<bool> col_matched(cols, false);
    
    // 贪心匹配
    for (size_t i = 0; i < rows; ++i) {
        float min_cost = thresh;
        size_t min_j = cols;
        
        for (size_t j = 0; j < cols; ++j) {
            if (col_matched[j]) continue;
            
            if (cost_matrix[i][j] < min_cost) {
                min_cost = cost_matrix[i][j];
                min_j = j;
            }
        }
        
        if (min_j < cols) {
            matches.push_back({i, min_j});
            row_matched[i] = true;
            col_matched[min_j] = true;
        }
    }
    
    // 收集未匹配的行和列
    for (size_t i = 0; i < rows; ++i) {
        if (!row_matched[i]) {
            unmatched_a.push_back(i);
        }
    }
    
    for (size_t j = 0; j < cols; ++j) {
        if (!col_matched[j]) {
            unmatched_b.push_back(j);
        }
    }
}
