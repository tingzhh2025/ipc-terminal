#include "modules/Display/DisplayCheck.h"

int main() {
    // ...原始代码...
    
    // 初始化其他必要模块
    Network network;
    Control control;
    Video video;
    Pantilt pantilt;
    Led led;
    
    // 根据配置和设备情况决定是否初始化Display
    std::unique_ptr<Display> display;
    if (DisplayCheck::isDisplayEnabled() && DisplayCheck::isDisplayAvailable()) {
        display = std::make_unique<Display>();
        // 连接视频帧信号到Display
        video.signal_video_frame.connect(std::bind(&Display::push_frame, display.get(), std::placeholders::_1));
    } else {
        LOG_INFO("Display module is disabled or not available\n");
    }
    
    // ...原始代码...
    
    return 0;
}
