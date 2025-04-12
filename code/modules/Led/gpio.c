#include "gpio.h"

#define GPIO_PATH "/sys/class/gpio/"

/**
 * @brief 生成 GPIO 路径。
 * 
 * @param path 保存生成的路径。
 * @param gpio_num GPIO 编号。
 * @param filename 文件名。
 * @return void
 */
void gpio_generate_path(char *path, int gpio_num, const char *filename) {
    sprintf(path, GPIO_PATH "gpio%d/%s", gpio_num, filename);
}

/**
 * @brief 检查 GPIO 是否已经导出。
 * 
 * @param gpio_num GPIO 编号。
 * @return 已导出返回 0，未导出返回 -1。
 */
int gpio_export_check(int gpio_num) {
    char gpio_path[64];

    // Generate the GPIO path, e.g., /sys/class/gpio/gpio54
    sprintf(gpio_path, GPIO_PATH "gpio%d", gpio_num);

    // Check if the GPIO already exists
    if (access(gpio_path, F_OK) == 0) {
        fprintf(stdout, "GPIO%d already exported\n", gpio_num);
        return 0;  // GPIO already exported
    }

    return -1;  // GPIO not exported yet
}

/**
 * @brief 初始化 GPIO。
 * 
 * @param num GPIO 编号。
 * @param direct GPIO 方向。
 * @return 成功返回 0，失败返回 -1。
 */
int gpio_init(enum Gpio_num num, enum Gpio_direction direct) {
    char path[64];
    int fd, ret;
    char buffer[4];

    // If GPIO_num exists, skip export
    if (gpio_export_check(num) == 0) {
        return 0;
    }

    // Export GPIO
    fd = open(GPIO_PATH "export", O_WRONLY);
    if (fd < 0) {
        perror("Failed to open export");
        return -1;
    }
    sprintf(buffer, "%d", num);
    ret = write(fd, buffer, strlen(buffer));
    close(fd);
    if (ret < 0) {
        perror("Failed to export GPIO");
        return -1;
    }

    // Set GPIO direction
    gpio_generate_path(path, num, "direction");
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open direction");
        return -1;
    }
    if (direct == GPIO_INPUT) {
        ret = write(fd, "in", 2);
    } else {
        ret = write(fd, "out", 3);
    }
    close(fd);
    if (ret < 0) {
        perror("Failed to set direction");
        return -1;
    }

    return 0;
}

/**
 * @brief 反初始化 GPIO。
 * 
 * @param num GPIO 编号。
 * @return void
 */
void gpio_deinit(enum Gpio_num num) {
    int fd;
    char buffer[4];

    fd = open(GPIO_PATH "unexport", O_WRONLY);
    if (fd < 0) {
        perror("Failed to open unexport");
        return;
    }
    sprintf(buffer, "%d", num);
    write(fd, buffer, strlen(buffer));
    close(fd);
}

/**
 * @brief 读取 GPIO 值。
 * 
 * @param num GPIO 编号。
 * @return 成功返回 GPIO 值，失败返回 -1。
 */
int gpio_read(enum Gpio_num num) {
    char path[64];
    char value_str[4];
    int fd;

    gpio_generate_path(path, num, "value");
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open value file");
        return -1;
    }

    if (read(fd, value_str, sizeof(value_str)) < 0) {
        perror("Failed to read value");
        close(fd);
        return -1;
    }

    close(fd);
    return atoi(value_str);  // Return the GPIO value
}

/**
 * @brief 写入 GPIO 值。
 * 
 * @param num GPIO 编号。
 * @param value 要写入的值。
 * @return void
 */
void gpio_write(enum Gpio_num num, int value) {
    char path[64];
    char value_str[2];
    int fd;

    gpio_generate_path(path, num, "value");
    fprintf(stdout, "Writing to %s\n", path);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open value file");
        return;
    }

    sprintf(value_str, "%d", value);
    write(fd, value_str, 1);
    close(fd);
}

/**
 * @brief 切换 GPIO 值。
 * 
 * @param num GPIO 编号。
 * @return void
 */
void gpio_toggle(enum Gpio_num num) {
    int value = gpio_read(num);
    if (value != -1) {
        gpio_write(num, !value);  // Write the opposite value
    }
}

/**
 * @brief 获取 GPIO 状态。
 * 
 * @param num GPIO 编号。
 * @param status 保存 GPIO 状态。
 * @return 成功返回 0，失败返回 -1。
 */
int gpio_get_status(enum Gpio_num num, struct Gpio_status *status) {
    char path[64];
    char buffer[8];
    int fd;

    // Get direction
    gpio_generate_path(path, num, "direction");
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open direction file");
        return -1;
    }
    read(fd, buffer, sizeof(buffer));
    status->direct = (strncmp(buffer, "in", 2) == 0) ? GPIO_INPUT : GPIO_OUTPUT;
    close(fd);

    // Get value (GPIO level)
    status->value = gpio_read(num);
    if (status->value == -1) {
        return -1;
    }

    // Get active_low status
    gpio_generate_path(path, num, "active_low");
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open active_low file");
        return -1;
    }
    read(fd, buffer, sizeof(buffer));
    status->active_low = atoi(buffer);  // 0 = normal, 1 = active low
    close(fd);

    return 0;
}

/**
 * @brief 设置 GPIO 方向。
 * 
 * @param num GPIO 编号。
 * @param direct GPIO 方向。
 * @return void
 */
void gpio_set_direction(enum Gpio_num num, enum Gpio_direction direct) {
    char path[64];
    int fd;

    gpio_generate_path(path, num, "direction");
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open direction file");
        return;
    }

    if (direct == GPIO_INPUT) {
        write(fd, "in", 2);
    } else {
        write(fd, "out", 3);
    }
    close(fd);
}

/**
 * @brief 设置 GPIO 边缘触发。
 * 
 * @param num GPIO 编号。
 * @param edge 边缘触发类型。
 * @return 成功返回 0，失败返回 -1。
 */
int gpio_set_edge(enum Gpio_num num, enum Gpio_edge edge) {
    char path[64];
    int fd;
    const char *edge_str;

    switch (edge) {
        case GPIO_EDGE_NONE:
            edge_str = "none";
            break;
        case GPIO_EDGE_RISING:
            edge_str = "rising";
            break;
        case GPIO_EDGE_FALLING:
            edge_str = "falling";
            break;
        case GPIO_EDGE_BOTH:
            edge_str = "both";
            break;
        default:
            return -1;
    }

    gpio_generate_path(path, num, "edge");
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open edge file");
        return -1;
    }

    write(fd, edge_str, strlen(edge_str));
    close(fd);
    return 0;
}

/**
 * @brief 设置 GPIO active_low 属性。
 * 
 * @param num GPIO 编号。
 * @param active_low 0 = normal, 1 = active low。
 * @return 成功返回 0，失败返回 -1。
 */
int gpio_set_active_low(enum Gpio_num num, int active_low) {
    char path[64];
    int fd;
    char buffer[2];

    gpio_generate_path(path, num, "active_low");
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open active_low file");
        return -1;
    }

    sprintf(buffer, "%d", active_low);
    write(fd, buffer, 1);
    close(fd);
    return 0;
}

/**
 * @brief 等待 GPIO 边缘触发事件。
 * 
 * @param num GPIO 编号。
 * @param timeout_ms 超时时间（毫秒）。
 * @return 1 = 事件，0 = 超时，-1 = 错误。
 */
int gpio_wait_for_edge(enum Gpio_num num, int timeout_ms) {
    char path[64];
    int fd;
    struct pollfd pfd;

    gpio_generate_path(path, num, "value");
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open value file");
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLPRI;

    // Initialize poll, read once to clear existing edge events
    char buffer[4];
    read(fd, buffer, sizeof(buffer));

    // Wait for edge event with timeout
    int ret = poll(&pfd, 1, timeout_ms);
    close(fd);
    return ret;  // Return poll result: 1 = event, 0 = timeout, -1 = error
}
