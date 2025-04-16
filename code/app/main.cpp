#include "global.h"
#include "Network.h"
#include "Control.h"
#include "Led.h"
#include "Pantilt.h"
#include "Display.h"
#include "Video.h"
#include "../modules/ApiServer/api_server.h"

#include "onvif_server.h"

#define ONVIF_SERVER_ENABLE 1
#define NETWORK_ENABLE 1
#define CONTROL_ENABLE 1
#define LED_ENABLE 1
#define PANTILT_ENABLE 1
#define DISPLAY_ENABLE 1
#define VIDEO_ENABLE 1
#define API_SERVER_ENABLE 1

int rkipc_log_level = LOG_LEVEL_DEBUG;
char ini_path[] = "ipc-terminal.ini";

bool quit = false;

static void sigterm_handler(int sig) {
    fprintf(stderr, "Caught signal %d, cleaning up...\n", sig);
    quit = true;
}

int main(int argc, char *argv[]) {
    // 注册信号处理函数，捕获 SIGINT 和 SIGTERM
    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    rk_param_init(ini_path);

#if NETWORK_ENABLE
    // Network 模块初始化
    LOG_DEBUG("Network module initialing\n");
    Network *network = new Network();
#endif
#if CONTROL_ENABLE
    // Control 模块初始化
    LOG_DEBUG("Control module initializing\n");
    Control *control = new Control();
#endif
#if NETWORK_ENABLE && CONTROL_ENABLE
    network->signal_network_received.connectWithRef(control, &Control::onNetworkReceived);
#endif
#if LED_ENABLE
    // LED 模块初始化
    LOG_DEBUG("LED module initializing\n");
    Led *led0 = new Led(LED0);
    control->registerControlFunction(ID_LED, OP_LED_ON, std::bind(&Led::on, led0));
    control->registerControlFunction(ID_LED, OP_LED_OFF, std::bind(&Led::off, led0));
    control->registerControlFunction(ID_LED, OP_LED_TOGGLE, std::bind(&Led::toggle, led0));
    control->registerControlFunction(ID_LED, OP_LED_BLINK, std::bind(&Led::blink, led0, std::placeholders::_1));
#endif
#if PANTILT_ENABLE
    // Pantilt 模块初始化
    LOG_DEBUG("Pantilt module initializing\n");
    Pantilt *pantilt = new Pantilt();
    control->registerControlFunction(ID_PANTILT, OP_PANTILT_UP, std::bind(&Pantilt::up, pantilt, std::placeholders::_1));
    control->registerControlFunction(ID_PANTILT, OP_PANTILT_DOWN, std::bind(&Pantilt::down, pantilt, std::placeholders::_1));
    control->registerControlFunction(ID_PANTILT, OP_PANTILT_LEFT, std::bind(&Pantilt::left, pantilt, std::placeholders::_1));
    control->registerControlFunction(ID_PANTILT, OP_PANTILT_RIGHT, std::bind(&Pantilt::right, pantilt, std::placeholders::_1));
    control->registerControlFunction(ID_PANTILT, OP_PANTILT_RESET, std::bind(&Pantilt::reset, pantilt));
#endif
#if DISPLAY_ENABLE
    // 显示类初始化
    LOG_DEBUG("Display module initializing\n");
    Display *display = new Display();
    control->registerControlFunction(ID_DISPLAY, OP_DISPLAY_PAUSE, std::bind(&Display::pause, display));
    control->registerControlFunction(ID_DISPLAY, OP_DISPLAY_RESUME, std::bind(&Display::resume, display));
#endif
#if VIDEO_ENABLE
    // 视频类初始化
    LOG_DEBUG("Video module initializing\n");
    system("RkLunch-stop.sh");
    Video *video = new Video();
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE0_START, std::bind(&Video::video_pipe0_start, video));
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE0_STOP, std::bind(&Video::video_pipe0_stop, video));
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE0_RESTART, std::bind(&Video::video_pipe0_restart, video));
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE1_START, std::bind(&Video::video_pipe1_start, video));
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE1_STOP, std::bind(&Video::video_pipe1_stop, video));
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_PIPE1_RESTART, std::bind(&Video::video_pipe1_restart, video));
    
    // 添加重新加载ROI配置的控制功能
    control->registerControlFunction(ID_VIDEO, OP_VIDEO_RELOAD_CONFIG, std::bind(&Video::reload_roi_config, video));
#endif
#if VIDEO_ENABLE && DISPLAY_ENABLE
    video->signal_video_frame.connectWithRef(display, &Display::push_frame);
#endif
#if VIDEO_ENABLE && PANTILT_ENABLE
    video->signal_adjust_pantilt.connect(pantilt, &Pantilt::onAjustPantilt);
#endif

#if API_SERVER_ENABLE
    // 初始化 API 服务器
    LOG_DEBUG("API server initializing\n");
    // 读取 API 服务器的端口号（默认为 8080）
    int api_port = rk_param_get_int("api:port", 8080);
    ApiServer api_server(api_port);
    
#if API_SERVER_ENABLE && VIDEO_ENABLE
    // 设置 ROI 检测器
    api_server.setRoiDetector(video->get_roi_detector());
    
    // 注册视频控制功能
    api_server.setControl(control);
#endif

#if API_SERVER_ENABLE && LED_ENABLE
    // 注册 LED 控制器
    api_server.setLed(led0);
#endif

#if API_SERVER_ENABLE && PANTILT_ENABLE
    // 注册云台控制器
    api_server.setPantilt(pantilt);
#endif
    
    // 读取并设置 API 密钥（如果有的话）
    std::string api_key = rk_param_get_string("api:key", "");
    if (!api_key.empty()) {
        LOG_INFO("API authentication enabled\n");
        api_server.setApiKey(api_key);
    } else {
        LOG_WARN("API authentication disabled. Consider setting an API key for better security\n");
    }
    
    // 启动 API 服务器
    if (api_server.start()) {
        LOG_INFO("API server started on port %d\n", api_port);
        
        // 注册告警回调，以便记录告警历史
        api_server.registerAlarmCallback();
    } else {
        LOG_ERROR("Failed to start API server\n");
    }
#endif

#if ONVIF_SERVER_ENABLE
    // ONVIF 服务初始化
    LOG_DEBUG("ONVIF server initializing\n");
    onvif_server_init();
#endif

    while (!quit) {
        // LOG_ERROR("neteork:server_ip%s\n", rk_param_get_string("network:server_ip", ""));
        // LOG_WARN("neteork:server_ip%s\n", rk_param_get_string("network:server_ip", ""));
        // LOG_INFO("neteork:server_ip%s\n", rk_param_get_string("network:server_ip", ""));
        // LOG_DEBUG("neteork:server_ip%s\n", rk_param_get_string("network:server_ip", ""));

        // network->send_data("Hello, server");
        sleep(1);
    }

    try {
#if ONVIF_SERVER_ENABLE
        onvif_server_deinit();
        LOG_DEBUG("ONVIF server deinitialized\n");
#endif
#if API_SERVER_ENABLE
        api_server.stop();
        LOG_DEBUG("API server stopped\n");
#endif
#if VIDEO_ENABLE
        delete video;
        LOG_DEBUG("Video module deinitialized\n");
#endif
#if DISPLAY_ENABLE
        delete display;
        LOG_DEBUG("Display module deinitialized\n");
#endif
#if PANTILT_ENABLE
        delete pantilt;
        LOG_DEBUG("Pantilt module deinitialized\n");
#endif
#if LED_ENABLE
        delete led0;
        LOG_DEBUG("LED module deinitialized\n");
#endif
#if CONTROL_ENABLE
        delete control;
        LOG_DEBUG("Control module deinitialized\n");
#endif
#if NETWORK_ENABLE
        delete network;
        LOG_DEBUG("Network module deinitialized\n");
#endif
    } catch (std::exception &e) {
        LOG_ERROR("Exception: %s\n", e.what());
    }

    rk_param_deinit();

    LOG_INFO("Program exited\n");

    return 0;
}
