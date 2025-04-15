#include "Display.h"
#include "framebuffer.h"
#include "DisplayCheck.h"

/**
 * @brief 显示类构造函数。
 */
Display::Display() {
flag_quit = false;
flag_pause = false;

// 检查是否启用显示功能以及显示设备是否可用
bool display_enabled = DisplayCheck::isDisplayEnabled();
bool display_available = DisplayCheck::isDisplayAvailable();

if (!display_enabled || !display_available) {
    LOG_WARN("Display is disabled or not available, display thread will not start\n");
    flag_quit = true;
    return;
}

// 初始化Framebuffer
int ret = framebuffer_init(FB_DEVICE);
if (ret != 0) {
    LOG_ERROR("Framebuffer init failed\n");
    flag_quit = true;
    return;
}

// 获取显示分辨率和色深
ret = framebuffer_get_resolution(&width, &height, &bit_depth);
if (ret != 0) {
    LOG_ERROR("Get framebuffer resolution failed\n");
    flag_quit = true;
    framebuffer_deinit();
    return;
}

LOG_INFO("Framebuffer: %dx%d, %d bpp\n", width, height, bit_depth);

// 创建显示线程
display_thread = std::thread(&Display::display_on_fb, this);
}

/**
 * @brief 显示类析构函数。
 */
Display::~Display() {
    // 设置退出标志，并唤醒所有等待的线程
    {
        // 作用域自动加锁，超出作用域自动解锁
        std::lock_guard<std::mutex> lock(mtx_display);
        flag_quit = true;
    }
    cond_var_display.notify_all();

    // 等待线程结束
    if (display_thread.joinable()) {
        display_thread.join();
    }

    // 释放 framebuffer 资源
    framebuffer_deinit();

    LOG_DEBUG("Display deinitialized\n");
}

/**
 * @brief 推送帧到显示队列。
 * 
 * @param frame 要显示的帧。
 */
void Display::push_frame(const cv::Mat& frame) {
    if (flag_quit) {
        return;
    }

    if (flag_pause || flag_quit) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_display);
        if (frame_queue.size() < 10) { // 限制队列长度，避免内存占用过多
            frame_queue.push(frame);
        }
    }
    cond_var_display.notify_one(); // 唤醒显示线程
}

/**
 * @brief 暂停显示。
 */
void Display::pause() {
    std::lock_guard<std::mutex> lock(mtx_display);
    flag_pause = true;
}

/**
 * @brief 继续显示。
 */
void Display::resume() {
    {
        std::lock_guard<std::mutex> lock(mtx_display);
        flag_pause = false;
        while (!frame_queue.empty()) {
            frame_queue.pop(); // 清空队列
        }
    }
    cond_var_display.notify_one(); // 唤醒显示线程继续工作
}

/**
 * @brief 显示线程的运行函数。
 */
void Display::display_on_fb() {
    while (!flag_quit) {
        std::unique_lock<std::mutex> ulock(mtx_display);

        // 等待队列有数据或退出信号
        cond_var_display.wait(ulock, [this] {
            return !frame_queue.empty() || flag_quit;
        });

        // 如果设置了退出标志，退出线程
        if (flag_quit) {
            break;
        }

        // 暂停时等待唤醒
        if (flag_pause) {
            cond_var_display.wait(ulock, [this] { 
                return !flag_pause || flag_quit; 
            });
        }

        // 取出一帧
        cv::Mat frame = frame_queue.front();
        frame_queue.pop();

        ulock.unlock(); // 解锁以允许其他线程推送帧

        if (frame.empty()) {
            LOG_WARN("Empty frame received\n");
            return;
        }

        if (frame.type() != CV_8UC3) {
            LOG_ERROR("Frame type mismatch, expect CV_8UC3 but got %d\n", frame.type());
            return;
        }
        
        cv::Mat resized_frame;
        if (frame.rows != height || frame.cols != width) {
            cv::resize(frame, resized_frame, cv::Size(width, height));
        } else {
            resized_frame = frame.clone();
        }

        cv::Mat rgb565_frame;
        cv::cvtColor(resized_frame, rgb565_frame, cv::COLOR_BGR2BGR565);

        if (framebuffer_set_frame_rgb565((uint16_t*)rgb565_frame.data, width, height) != 0) {
            LOG_ERROR("Failed to set frame to framebuffer\n");
        }
    }
}
