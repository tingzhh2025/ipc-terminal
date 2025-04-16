#include "param.h"
#include <stdlib.h>

float rk_param_get_float(const char *entry, float default_val) {
    // 首先获取字符串形式的参数
    const char *str_val = rk_param_get_string(entry, NULL);
    if (!str_val) {
        return default_val;
    }
    
    // 将字符串转换为浮点数
    char *endptr;
    float val = strtof(str_val, &endptr);
    
    // 检查转换是否成功
    if (endptr == str_val) {
        return default_val;
    }
    
    return val;
}
