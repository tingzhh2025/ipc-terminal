// Copyright 2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "iniparser.h"

#ifdef __cplusplus
#include <vector>
#include <string>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern dictionary *g_ini_d_;

int rk_param_get_int(const char *entry, int default_val);
int rk_param_set_int(const char *entry, int val);
const char *rk_param_get_string(const char *entry, const char *default_val);
int rk_param_set_string(const char *entry, const char *val);
float rk_param_get_float(const char *entry, float default_val);
int rk_param_save();
int rk_param_init(char *ini_path);
int rk_param_deinit();
int rk_param_reload();

#ifdef __cplusplus
}

// 添加C++接口，返回特定部分的所有参数
std::vector<std::pair<std::string, std::string>> rk_param_get_section(const char* section);

// 添加C++接口，返回所有参数
std::vector<std::pair<std::string, std::string>> rk_param_get_all();
#endif
