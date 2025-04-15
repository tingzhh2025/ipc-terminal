#ifndef DISPLAY_CHECK_H
#define DISPLAY_CHECK_H

#include <string>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include "log/log.h"

class DisplayCheck {
public:
    /**
     * 检查显示设备是否可用
     * 
     * @return 如果显示设备可用返回true，否则返回false
     */
    static bool isDisplayAvailable() {
        // 检查常见的framebuffer设备
        const char* fb_devices[] = {
            "/dev/fb0",
            "/dev/fb1",
            "/dev/graphics/fb0"
        };
        
        for (const char* fb_device : fb_devices) {
            if (access(fb_device, F_OK | R_OK | W_OK) == 0) {
                struct stat st;
                if (stat(fb_device, &st) == 0 && S_ISCHR(st.st_mode)) {
                    LOG_DEBUG("Found available display device: %s\n", fb_device);
                    return true;
                }
            }
        }
        
        LOG_WARN("No display device available\n");
        return false;
    }
    
    /**
     * 检查配置文件中是否启用了显示功能
     * 
     * @return 如果启用了显示功能返回true，否则返回false
     */
    static bool isDisplayEnabled() {
        int display_enable = rk_param_get_int("display:enable", 1);
        return display_enable != 0;
    }
};

#endif // DISPLAY_CHECK_H
