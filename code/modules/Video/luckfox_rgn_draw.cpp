#include "luckfox_rgn_draw.h"
#include <opencv2/opencv.hpp>
#include <string.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <map>
#include <chrono>
#include <unordered_map>  // 添加这个头文件以支持std::unordered_map

// label OSD的ID基准值
static const int BASE_LABEL_ID = 1000;

// 记录绘制任务批次号
std::mutex idxMutex;
static int batch_idx = 0;

// 记录已创建的OSD标签ID
static std::unordered_set<int> created_labels;

// 全局队列，用于存储绘制任务
std::queue<RgnDrawTask> drawTaskQueue;
std::mutex queueMutex;
std::condition_variable cv_draw_queue;

// 绘制线程运行标志及线程ID
bool rgn_thread_run_ = true;
pthread_t rgn_draw_thread_id;

// 使用一个全局OSD区域显示所有标签
static RGN_HANDLE g_LabelRgnHandle = 10;  // 使用单一OSD区域显示所有标签
static bool g_LabelRgnCreated = false;
static std::vector<std::pair<int, int>> label_positions; // 存储标签的位置和尺寸

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

// 使用目标类别+位置作为key的哈希函数
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

// 初始化全局标签OSD区域
static RK_S32 init_global_label_rgn() {
    if (g_LabelRgnCreated) {
        return RK_SUCCESS;
    }

    RGN_ATTR_S stRgnAttr;
    RGN_CHN_ATTR_S stRgnChnAttr;
    MPP_CHN_S stChn;

    // 创建一个大的OSD区域用于显示所有标签
    memset(&stRgnAttr, 0, sizeof(stRgnAttr));
    stRgnAttr.enType = OVERLAY_RGN;
    stRgnAttr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
    stRgnAttr.unAttr.stOverlay.u32CanvasNum = 1;
    stRgnAttr.unAttr.stOverlay.stSize.u32Width = 2304;  // 使用与视频相同的宽度
    stRgnAttr.unAttr.stOverlay.stSize.u32Height = 1296; // 使用与视频相同的高度

    // 创建RGN区域
    int ret = RK_MPI_RGN_Create(g_LabelRgnHandle, &stRgnAttr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Create global label RGN failed with %#x\n", ret);
        return ret;
    }

    // 设置OSD区域的位置和属性
    memset(&stRgnChnAttr, 0, sizeof(stRgnChnAttr));
    stRgnChnAttr.bShow = RK_TRUE;
    stRgnChnAttr.enType = OVERLAY_RGN;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;   // 背景完全透明
    stRgnChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 255; // 前景不透明
    stRgnChnAttr.unChnAttr.stOverlayChn.u32Layer = 7;     // 修正为有效范围内的最大值(0-7)

    // 设置通道
    stChn.enModId = RK_ID_VENC;
    stChn.s32DevId = 0;
    stChn.s32ChnId = 0;

    ret = RK_MPI_RGN_AttachToChn(g_LabelRgnHandle, &stChn, &stRgnChnAttr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Attach global label RGN failed with %#x\n", ret);
        RK_MPI_RGN_Destroy(g_LabelRgnHandle);
        return ret;
    }

    g_LabelRgnCreated = true;
    return RK_SUCCESS;
}

// 在共享OSD区域上绘制标签
static RK_S32 draw_label_on_global_rgn(const std::vector<std::tuple<int, int, int, int, std::string>>& labels) {
    if (!g_LabelRgnCreated) {
        if (init_global_label_rgn() != RK_SUCCESS) {
            return RK_FAILURE;
        }
    }

    // 获取共享OSD区域的尺寸
    int width = 2304;
    int height = 1296;

    // 创建位图
    BITMAP_S stBitmap;
    memset(&stBitmap, 0, sizeof(BITMAP_S));
    stBitmap.enPixelFormat = RK_FMT_ARGB8888;
    stBitmap.u32Width = width;
    stBitmap.u32Height = height;
    
    // 分配位图内存
    int bitmapSize = width * height * 4; // ARGB8888格式每像素4字节
    stBitmap.pData = malloc(bitmapSize);
    if (stBitmap.pData == NULL) {
        RK_LOGE("Failed to allocate bitmap memory for global label\n");
        return RK_FAILURE;
    }

    // 创建全透明背景
    memset(stBitmap.pData, 0, bitmapSize);
    
    // 创建OpenCV Mat对象用于绘制位图
    cv::Mat bitmapMat(height, width, CV_8UC4, stBitmap.pData);

    // 为每个标签绘制背景和文字
    for (const auto& label_info : labels) {
        int label_x = std::get<0>(label_info);
        int label_y = std::get<1>(label_info);
        int label_width = std::get<2>(label_info);
        int label_height = std::get<3>(label_info);
        const std::string& text = std::get<4>(label_info);

        // 确保标签位置有效
        if (label_x < 0 || label_y < 0 || 
            label_x + label_width > width || 
            label_y + label_height > height) {
            continue;
        }

        // 绘制半透明黑色背景
        cv::Rect bg_rect(label_x, label_y, label_width, label_height);
        cv::rectangle(bitmapMat, bg_rect, cv::Scalar(0, 0, 0, 180), -1);
        
        // 绘制文本
        cv::putText(bitmapMat, text, 
                   cv::Point(label_x + 8, label_y + label_height/2 + 6), 
                   cv::FONT_HERSHEY_DUPLEX, 0.6, 
                   cv::Scalar(255, 255, 255, 255), 1, cv::LINE_AA);
        
        // 绘制边框
        cv::rectangle(bitmapMat, bg_rect, cv::Scalar(100, 100, 100, 200), 1);
    }

    // 设置位图
    int ret = RK_MPI_RGN_SetBitMap(g_LabelRgnHandle, &stBitmap);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Set bitmap failed with %#x\n", ret);
        free(stBitmap.pData);
        return ret;
    }

    // 释放位图内存
    free(stBitmap.pData);
    return RK_SUCCESS;
}

// 在缓冲区中绘制文本区域

RK_S32 rgn_draw_nn_init(RGN_HANDLE RgnHandle)
{
    // 初始化视频编码区域的属性
    int ret = 0;

    RGN_ATTR_S stRgnAttr;
    MPP_CHN_S stMppChn;
    RGN_CHN_ATTR_S stRgnChnAttr;
    BITMAP_S stBitmap;
    int rotation = 0;

    // create overlay regions
    memset(&stRgnAttr, 0, sizeof(stRgnAttr));
    stRgnAttr.enType = OVERLAY_RGN;
    stRgnAttr.unAttr.stOverlay.enPixelFmt = RK_FMT_2BPP;
    stRgnAttr.unAttr.stOverlay.u32CanvasNum = 1;
    stRgnAttr.unAttr.stOverlay.stSize.u32Width = 2304;
    stRgnAttr.unAttr.stOverlay.stSize.u32Height = 1296;
    ret = RK_MPI_RGN_Create(RgnHandle, &stRgnAttr);
    if (RK_SUCCESS != ret) {
        printf("RK_MPI_RGN_Create (%d) failed with %#x\n", RgnHandle, ret);
        RK_MPI_RGN_Destroy(RgnHandle);
        return RK_FAILURE;
    }
    printf("The handle: %d, create success\n", RgnHandle);
    // after malloc max size, it needs to be set to the actual size
    stRgnAttr.unAttr.stOverlay.stSize.u32Width = 2304;
    stRgnAttr.unAttr.stOverlay.stSize.u32Height = 1296;
    ret = RK_MPI_RGN_SetAttr(RgnHandle, &stRgnAttr);
    if (RK_SUCCESS != ret) {
        printf("RK_MPI_RGN_SetAttr (%d) failed with %#x!", RgnHandle, ret);
        return RK_FAILURE;
    }

    // display overlay regions to venc groups
    memset(&stRgnChnAttr, 0, sizeof(stRgnChnAttr));
    stRgnChnAttr.bShow = RK_TRUE;
    stRgnChnAttr.enType = OVERLAY_RGN;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;      // 背景完全透明
    stRgnChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 255;    // 前景不透明
    stRgnChnAttr.unChnAttr.stOverlayChn.u32Layer = RgnHandle;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32ColorLUT[0] = 0x0000FF; // 设置为蓝色
    stRgnChnAttr.unChnAttr.stOverlayChn.u32ColorLUT[1] = 0x0000FF; // 两个颜色都设置为蓝色
    stMppChn.enModId = RK_ID_VENC;
    stMppChn.s32DevId = 0;
    stMppChn.s32ChnId = 0;
    ret = RK_MPI_RGN_AttachToChn(RgnHandle, &stMppChn, &stRgnChnAttr);
    if (RK_SUCCESS != ret) {
        printf("RK_MPI_RGN_AttachToChn (%d) to venc0 failed with %#x\n", RgnHandle, ret);
        return RK_FAILURE;
    }
    printf("RK_MPI_RGN_AttachToChn to venc0 success\n");

    pthread_create(&rgn_draw_thread_id, NULL, rgn_draw_thread, (void *)RgnHandle);
    rgn_thread_run_ = true;

    // 初始化共享标签OSD区域
    init_global_label_rgn();

    return ret;
}

int rgn_draw_nn_deinit() {
    if (!rgn_thread_run_) {
        printf("rgn thread is not running\n");
        return 0;
    }
    rgn_thread_run_ = false;
    cv_draw_queue.notify_all();
    pthread_join(rgn_draw_thread_id, NULL);

    // 清理所有标签区域
    for (auto label_id : created_labels) {
        RGN_HANDLE labelHandle = BASE_LABEL_ID + label_id;
        // 先解除绑定
        MPP_CHN_S stChn = {RK_ID_VENC, 0, 0};
        RK_MPI_RGN_DetachFromChn(labelHandle, &stChn);
        // 销毁RGN
        RK_MPI_RGN_Destroy(labelHandle);
    }
    created_labels.clear();

    int ret = 0;
    // Detach osd from chn
    MPP_CHN_S stMppChn;
    RGN_HANDLE RgnHandle = 0;
    stMppChn.enModId = RK_ID_VENC;
    stMppChn.s32DevId = 0;
    stMppChn.s32ChnId = 0;
    ret = RK_MPI_RGN_DetachFromChn(RgnHandle, &stMppChn);
    if (RK_SUCCESS != ret)
        printf("RK_MPI_RGN_DetachFrmChn (%d) to venc0 failed with %#x\n", RgnHandle, ret);

    // destory region
    ret = RK_MPI_RGN_Destroy(RgnHandle);
    if (RK_SUCCESS != ret) {
        printf("RK_MPI_RGN_Destroy [%d] failed with %#x\n", RgnHandle, ret);
    }
    printf("Destory handle:%d success\n", RgnHandle);

    // 清理共享标签区域
    if (g_LabelRgnCreated) {
        MPP_CHN_S stChn = {RK_ID_VENC, 0, 0};
        RK_MPI_RGN_DetachFromChn(g_LabelRgnHandle, &stChn);
        RK_MPI_RGN_Destroy(g_LabelRgnHandle);
        g_LabelRgnCreated = false;
    }

    return ret;
}

RK_S32 draw_rect_2bpp(RK_U8 *buffer, RK_U32 width, RK_U32 height, int rgn_x, int rgn_y, int rgn_w,
                      int rgn_h, int line_pixel, COLOR_INDEX_E color_index, const char* label) {
    int i;
    RK_U8 value = (color_index == 0) ? 0xff : 0xaa;

    if (line_pixel > 4) {
        RK_LOGE("line_pixel > 4, not support");
        return RK_FAILURE;
    }

    // 绘制检测框
    RK_U8 *ptr = buffer + (width * rgn_y + rgn_x) / 4;

    // 绘制顶部线条
    for (i = 0; i < line_pixel; i++) {
        memset(ptr, value, (rgn_w + 3) / 4);
        ptr += width / 4;
    }

    // 绘制左右线条
    for (i = 0; i < (rgn_h - line_pixel * 2); i++) {
        if (color_index == RGN_COLOR_LUT_INDEX_1) {
            *ptr = rgn_color_lut_1_left_value[line_pixel - 1];
            *(ptr + ((rgn_w + 3) / 4)) = rgn_color_lut_1_right_value[line_pixel - 1];
        } else {
            *ptr = rgn_color_lut_0_left_value[line_pixel - 1];
            *(ptr + ((rgn_w + 3) / 4)) = rgn_color_lut_0_right_value[line_pixel - 1];
        }
        ptr += width / 4;
    }

    // 绘制底部线条
    for (i = 0; i < line_pixel; i++) {
        memset(ptr, value, (rgn_w + 3) / 4);
        ptr += width / 4;
    }

    // 如果有标签，添加到标签列表中而不是创建新的OSD区域
    if (label && strlen(label) > 0) {
        // 计算标签位置和大小
        int label_height = 28;
        int label_margin = 2;
        int label_y = (rgn_y > label_height + label_margin) ? 
                     (rgn_y - label_height - label_margin) : (rgn_y + label_margin);
        
        int text_len = strlen(label);
        int label_width = text_len * 14 + 10; // 根据文本长度调整宽度
        
        // 确保标签宽度合适
        if (label_width > rgn_w) {
            label_width = rgn_w;
        }
        if (label_width < 60) {
            label_width = 60;
        }
        
        // 计算标签X坐标使其居中
        int label_x = rgn_x + (rgn_w - label_width) / 2;
        
        // 确保标签位置有效
        if (label_x < 0) label_x = 0;
        if (label_y < 0) label_y = 0;
        if (label_x + label_width > width) label_width = width - label_x;
        if (label_y + label_height > height) label_height = height - label_y;
        
        // 添加到全局标签列表
        static std::mutex labels_mtx;
        {
            std::lock_guard<std::mutex> lock(labels_mtx);
            // 如果有添加新标签，需要在后续处理
            // 这里我们记录标签位置，稍后统一绘制
            static std::vector<std::tuple<int, int, int, int, std::string>> labels_to_draw;
            labels_to_draw.emplace_back(label_x, label_y, label_width, label_height, std::string(label));
            
            // 仅从绘制线程中调用，避免多线程访问冲突
            if (pthread_self() == rgn_draw_thread_id) {
                if (!labels_to_draw.empty()) {
                    draw_label_on_global_rgn(labels_to_draw);
                    labels_to_draw.clear();
                }
            }
        }
    }

    return RK_SUCCESS;
}

int rgn_draw_queue_push(RgnDrawTask tasks) {
    std::unique_lock<std::mutex> lck(queueMutex);
    drawTaskQueue.push(tasks);
    return 0;
}

int rgn_draw_queue_pop(RgnDrawTask *tasks) {
    std::unique_lock<std::mutex> lck(queueMutex);
    if (drawTaskQueue.empty()) {
        return -1;
    }
    *tasks = drawTaskQueue.front();
    drawTaskQueue.pop();
    return 0;
}

// 添加绘制任务
void rgn_add_draw_task(const RgnDrawParams& params) {
    RgnDrawTask task;
    task.params = params;
    idxMutex.lock();
    task.batch_idx = batch_idx;
    batch_idx = (batch_idx + 1) % INT32_MAX;
    idxMutex.unlock();
    task.batch_num = 1;
    rgn_draw_queue_push(task);
    cv_draw_queue.notify_all();  // 通知绘制线程新任务到来
}

// 批量添加绘制任务，填充绘制批次号，绘制批次数量
void rgn_add_draw_tasks_batch(const std::vector<RgnDrawParams>& params) {
    for (const auto& p : params) {
        RgnDrawTask task;
        task.params = p;
        idxMutex.lock();
        task.batch_idx = batch_idx;
        idxMutex.unlock();
        task.batch_num = params.size();
        // printf("add draw task %d\n", batch_idx);
        // printf("add draw task count %d\n", params.size());
        rgn_draw_queue_push(task);
    }
    idxMutex.lock();
    batch_idx = (batch_idx + 1) % INT32_MAX;
    idxMutex.unlock();
    cv_draw_queue.notify_all();  // 通知绘制线程新任务到来
}

// 绘制线程，支持异步绘制和批量绘制
void *rgn_draw_thread(void *arg) {
    RGN_HANDLE RgnHandle = (RGN_HANDLE)arg;
    int ret;
    int line_pixel = 2;
    RGN_CANVAS_INFO_S stCanvasInfo;

    // 用于收集所有标签信息
    std::vector<std::tuple<int, int, int, int, std::string>> frame_labels;

    while (rgn_thread_run_) {
        ret = RK_MPI_RGN_GetCanvasInfo(RgnHandle, &stCanvasInfo);
        if (ret != RK_SUCCESS) {
            RK_LOGE("Failed to get canvas info: %#x", ret);
            usleep(10000);  // 等待10ms
            continue;
        }

        // 检查画布尺寸
        if ((stCanvasInfo.stSize.u32Width % 16 != 0) ||
            (stCanvasInfo.stSize.u32Height % 16 != 0)) {
            RK_LOGE("Canvas dimensions must be multiples of 16");
            continue;
        }

        // 清理上一帧的OSD标签
        for (auto label_id : created_labels) {
            RGN_HANDLE labelHandle = BASE_LABEL_ID + label_id;
            // 先解除绑定，然后销毁
            MPP_CHN_S stChn = {RK_ID_VENC, 0, 0};
            RK_MPI_RGN_DetachFromChn(labelHandle, &stChn);
            RK_MPI_RGN_Destroy(labelHandle);
        }
        created_labels.clear();

        // 清空画布
        memset((void *)stCanvasInfo.u64VirAddr, 0,
               stCanvasInfo.u32VirWidth * stCanvasInfo.u32VirHeight >> 2);

        // 用于收集所有标签信息，每帧重置
        frame_labels.clear();

        // 绘制当前批次的所有任务
        int currentBatchIdx = -1;
        int remainingInBatch = 0;

        while (rgn_thread_run_) {
            // 获取任务
            RgnDrawTask task;
            {
                std::unique_lock<std::mutex> lck(queueMutex);
                
                // 等待新任务或退出信号
                cv_draw_queue.wait(lck, [&] {
                    return !drawTaskQueue.empty() || !rgn_thread_run_;
                });
                
                if (drawTaskQueue.empty()) {
                    if (!rgn_thread_run_) {
                        break;  // 线程退出
                    }
                    continue;  // 继续等待
                }

                // 处理批次
                if (currentBatchIdx == -1) {
                    currentBatchIdx = drawTaskQueue.front().batch_idx;
                    remainingInBatch = drawTaskQueue.front().batch_num;
                }

                // 检查是否是当前批次
                if (drawTaskQueue.front().batch_idx != currentBatchIdx) {
                    break;  // 处理下一批次
                }

                task = drawTaskQueue.front();
                drawTaskQueue.pop();
            }

            // 处理单个任务
            int x = task.params.x & ~1;  // 确保是2的倍数
            int y = task.params.y & ~1;
            int w = task.params.w & ~1;
            int h = task.params.h & ~1;

            // 边界检查
            if (w <= 0 || h <= 0) continue;
            if (x + w > stCanvasInfo.stSize.u32Width) {
                w = stCanvasInfo.stSize.u32Width - x - line_pixel;
            }
            if (y + h > stCanvasInfo.stSize.u32Height) {
                h = stCanvasInfo.stSize.u32Height - y - line_pixel;
            }
            if (w <= 0 || h <= 0) continue;

            // 绘制检测框和标签
            draw_rect_2bpp((RK_U8 *)stCanvasInfo.u64VirAddr,
                          stCanvasInfo.u32VirWidth,
                          stCanvasInfo.u32VirHeight,
                          x, y, w, h,
                          line_pixel,
                          RGN_COLOR_LUT_INDEX_0,
                          task.params.label);

            // 如果有标签，添加到标签列表中
            if (task.params.label && strlen(task.params.label) > 0) {
                // 计算标签位置和大小
                int label_height = 28;
                int label_margin = 2;
                int label_y = (y > label_height + label_margin) ? 
                             (y - label_height - label_margin) : (y + label_margin);
                
                int text_len = strlen(task.params.label);
                int label_width = text_len * 14 + 10; // 根据文本长度调整宽度
                
                // 确保标签宽度合适
                if (label_width > w) {
                    label_width = w;
                }
                if (label_width < 60) {
                    label_width = 60;
                }
                
                // 计算标签X坐标使其居中
                int label_x = x + (w - label_width) / 2;
                
                // 确保标签位置有效
                if (label_x < 0) label_x = 0;
                if (label_y < 0) label_y = 0;
                if (label_x + label_width > stCanvasInfo.stSize.u32Width) label_width = stCanvasInfo.stSize.u32Width - label_x;
                if (label_y + label_height > stCanvasInfo.stSize.u32Height) label_height = stCanvasInfo.stSize.u32Height - label_y;
                
                // 添加到全局标签列表
                frame_labels.emplace_back(label_x, label_y, label_width, label_height, std::string(task.params.label));
            }

            // 更新批次计数
            remainingInBatch--;
            if (remainingInBatch <= 0) {
                break;  // 当前批次完成
            }
        }

        // 在处理完所有任务后，统一绘制标签
        if (!frame_labels.empty()) {
            draw_label_on_global_rgn(frame_labels);
        }

        // 更新画布显示
        ret = RK_MPI_RGN_UpdateCanvas(RgnHandle);
        if (ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_RGN_UpdateCanvas failed with %#x!", ret);
            continue;
        }
    }
    return NULL;
}
