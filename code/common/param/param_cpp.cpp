#include "param.h"
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <mutex>

extern "C" {
    extern dictionary *g_ini_d_;
    extern pthread_mutex_t g_param_mutex;
}

/**
 * 获取特定部分的所有参数
 * @param section 部分名称
 * @return 参数键值对的向量
 */
std::vector<std::pair<std::string, std::string>> rk_param_get_section(const char* section) {
    std::vector<std::pair<std::string, std::string>> result;
    
    pthread_mutex_lock(&g_param_mutex);
    
    if (g_ini_d_ == NULL || section == NULL) {
        pthread_mutex_unlock(&g_param_mutex);
        return result;
    }
    
    // 获取部分中的键数量
    const char* keys[128]; // 假设一个部分最多有128个键
    int n_keys = iniparser_getsecnkeys(g_ini_d_, section);
    
    if (n_keys > 0) {
        // 获取部分中的所有键
        iniparser_getseckeys(g_ini_d_, section, keys);
        
        // 遍历所有键，获取对应的值
        for (int i = 0; i < n_keys && i < 128; i++) {
            const char* key = keys[i];
            const char* value = iniparser_getstring(g_ini_d_, key, "");
            
            result.emplace_back(key, value);
        }
    }
    
    pthread_mutex_unlock(&g_param_mutex);
    return result;
}

/**
 * 获取所有参数
 * @return 参数键值对的向量
 */
std::vector<std::pair<std::string, std::string>> rk_param_get_all() {
    std::vector<std::pair<std::string, std::string>> result;
    
    pthread_mutex_lock(&g_param_mutex);
    
    if (g_ini_d_ == NULL) {
        pthread_mutex_unlock(&g_param_mutex);
        return result;
    }
    
    // 获取部分数量
    int n_sections = iniparser_getnsec(g_ini_d_);
    
    // 遍历所有部分
    for (int i = 0; i < n_sections; i++) {
        // 获取部分名称
        const char* section_name = iniparser_getsecname(g_ini_d_, i);
        if (section_name == NULL) continue;
        
        // 获取该部分中的键数量
        const char* keys[128]; // 假设一个部分最多有128个键
        int n_keys = iniparser_getsecnkeys(g_ini_d_, section_name);
        
        if (n_keys > 0) {
            // 获取部分中的所有键
            iniparser_getseckeys(g_ini_d_, section_name, keys);
            
            // 遍历所有键，获取对应的值
            for (int j = 0; j < n_keys && j < 128; j++) {
                const char* key = keys[j];
                const char* value = iniparser_getstring(g_ini_d_, key, "");
                
                result.emplace_back(key, value);
            }
        }
    }
    
    pthread_mutex_unlock(&g_param_mutex);
    return result;
}