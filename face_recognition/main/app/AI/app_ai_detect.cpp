#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdio>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "app_pedestrian_detect.h"
#include "app_humanface_detect.h"
#include "app_coco_detect.h"
#include "app_drawing_utils.h"
#include "app_ai_detect.h"
#include "app_face_recognition.hpp"
#include "esp_painter.h"
#include "ui_extra.h"

static const char *TAG = "app_ai_detect";
constexpr int DETECT_WIDTH = 240, DETECT_HEIGHT = 240;
static esp_painter_handle_t painter;
static TaskHandle_t worker;
static SemaphoreHandle_t result_mutex;
static portMUX_TYPE frame_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t *frame_buffer;
static uint16_t *rotated_buffer;
static bool frame_busy;
static int frame_mode;
static uint32_t sequence;
static int64_t submitted_at;
static std::list<dl::detect::result_t> published_results;
static int published_mode = -1;
static int published_rotation;
static int64_t published_at;

static inline void get_center_square_crop(int src_w, int src_h, int *crop_x, int *crop_y, int *crop_size)
{
    int size = (src_w < src_h) ? src_w : src_h;
    *crop_size = size;
    *crop_x = (src_w - size) / 2;
    *crop_y = (src_h - size) / 2;
}

static void rgb565_center_crop_resize_nn(const uint16_t *src, int src_w, int src_h, uint16_t *dst, int dst_w, int dst_h)
{
    int crop_x = 0;
    int crop_y = 0;
    int crop_size = 0;
    get_center_square_crop(src_w, src_h, &crop_x, &crop_y, &crop_size);

    for (int y = 0; y < dst_h; y++) {
        int sy = crop_y + (y * crop_size) / dst_h;
        const uint16_t *src_row = src + sy * src_w;
        uint16_t *dst_row = dst + y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = crop_x + (x * crop_size) / dst_w;
            dst_row[x] = src_row[sx];
        }
    }
}

static inline int map_coord_detect_to_screen(int v, int crop_origin, int crop_size, int detect_size)
{
    return crop_origin + (v * crop_size) / detect_size;
}

static inline uint16_t maybe_swap_rgb565(uint16_t color, bool swap)
{
    if (swap) {
        return ((color & 0xFF) << 8) | ((color >> 8) & 0xFF);
    }
    return color;
}

static inline uint16_t painter_color_to_rgb565(esp_painter_color_t color)
{
    switch (color) {
    case ESP_PAINTER_COLOR_RED:
        return RGB565(255, 0, 0);
    case ESP_PAINTER_COLOR_GREEN:
        return RGB565(0, 255, 0);
    case ESP_PAINTER_COLOR_BLUE:
        return RGB565(0, 0, 255);
    case ESP_PAINTER_COLOR_WHITE:
        return RGB565(255, 255, 255);
    case ESP_PAINTER_COLOR_BLACK:
        return RGB565(0, 0, 0);
    case ESP_PAINTER_COLOR_YELLOW:
    default:
        return RGB565(255, 255, 0);
    }
}

static void draw_char_rotated_90(uint16_t *rgb_buf, int width, int height, int x, int y, const esp_painter_basic_font_t *font,
                                 esp_painter_color_t color, char c, int box_l, int box_t, int box_r, int box_b)
{
    if (!font || !rgb_buf || c < 32 || c > 127) {
        return;
    }

    const int bytes_per_row = ((int)font->width + 7) / 8;
    const uint8_t *char_bitmap = font->bitmap + (c - 32) * font->height * bytes_per_row;
    const uint16_t rgb565_color = maybe_swap_rgb565(painter_color_to_rgb565(color), true);

    for (int dy = 0; dy < (int)font->height; ++dy) {
        for (int dx = 0; dx < (int)font->width; ++dx) {
            uint8_t byte = char_bitmap[dy * bytes_per_row + (dx / 8)];
            if (byte & (0x80 >> (dx % 8))) {
                int dst_x = x + ((int)font->height - 1 - dy);
                int dst_y = y + dx;
                if (dst_x < box_l || dst_x > box_r || dst_y < box_t || dst_y > box_b) {
                    continue;
                }
                if (dst_x < 0 || dst_x >= width || dst_y < 0 || dst_y >= height) {
                    continue;
                }
                rgb_buf[dst_y * width + dst_x] = rgb565_color;
            }
        }
    }
}

static void draw_label_left_bottom_vertical_rotated(uint16_t *rgb_buf, int width, int height, const std::vector<int> &box, const char *label)
{
    if (!label || box.size() < 4) {
        return;
    }

    const esp_painter_basic_font_t *font = &esp_painter_basic_font_20;
    const int margin = 4;
    const int label_len = (int) strlen(label);
    const int char_w_rot = (int)font->height;
    const int char_h_rot = (int)font->width;
    const int box_l = box[0] + margin;
    const int box_t = box[1] + margin;
    const int box_r = box[2] - margin - 1;
    const int box_b = box[3] - margin - 1;

    if (box_r < box_l || box_b < box_t) {
        return;
    }

    if (char_w_rot > (box_r - box_l + 1) || char_h_rot > (box_b - box_t + 1)) {
        return;
    }

    int max_chars = (box_b - box_t + 1) / char_h_rot;
    if (max_chars <= 0) {
        return;
    }

    int draw_chars = (label_len < max_chars) ? label_len : max_chars;
    int x = box_l;
    int y = box_b - draw_chars * char_h_rot + 1;

    for (int i = 0; i < draw_chars; ++i) {
        draw_char_rotated_90(rgb_buf, width, height, x, y + i * char_h_rot, font, ESP_PAINTER_COLOR_YELLOW, label[i], box_l, box_t, box_r, box_b);
    }
}


static void detect_worker(void *)
{
    while (true) {
        // Commands still work with the camera page closed or no camera frames.
        face_recognition_commands();
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) continue;
        int mode = frame_mode;
        uint32_t frame_id = sequence;
        int64_t submitted = submitted_at;
        int rotation = face_recognition_rotation();
        uint16_t *input = frame_buffer;
        if (rotation) {
            for (int y = 0; y < DETECT_HEIGHT; ++y) {
                for (int x = 0; x < DETECT_WIDTH; ++x) {
                    int dx = x, dy = y;
                    if (rotation == 90) { dx = DETECT_WIDTH - 1 - y; dy = x; }
                    else if (rotation == 180) { dx = DETECT_WIDTH - 1 - x; dy = DETECT_HEIGHT - 1 - y; }
                    else if (rotation == 270) { dx = y; dy = DETECT_HEIGHT - 1 - x; }
                    rotated_buffer[dy * DETECT_WIDTH + dx] = frame_buffer[y * DETECT_WIDTH + x];
                }
            }
            input = rotated_buffer;
        }
        int64_t t0 = esp_timer_get_time();
        auto results = mode == AI_DETECT_FACE
            ? app_humanface_detect(input, DETECT_WIDTH, DETECT_HEIGHT)
            : app_pedestrian_detect(input, DETECT_WIDTH, DETECT_HEIGHT);
        int64_t t1 = esp_timer_get_time();
        if (mode == AI_DETECT_FACE) {
            dl::image::img_t image;
            image.data = input; image.width = DETECT_WIDTH; image.height = DETECT_HEIGHT;
            image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;
            face_recognition_process(image, results, frame_id, submitted, t0, t1);
        }
        xSemaphoreTake(result_mutex, portMAX_DELAY);
        published_results = std::move(results);
        published_mode = mode; published_rotation = rotation; published_at = esp_timer_get_time();
        xSemaphoreGive(result_mutex);
        portENTER_CRITICAL(&frame_mux); frame_busy = false; portEXIT_CRITICAL(&frame_mux);
    }
}

esp_err_t app_ai_detect_init(void)
{
    // MSR/MNP prints three per-face timing lines internally; keep UART out of benchmarks.
    esp_log_level_set("detect", ESP_LOG_WARN);
    // Keep unused COCO/pedestrian models lazy to leave RAM for face embeddings.
    if (!get_humanface_detect()) return ESP_ERR_NO_MEM;
    esp_err_t ret = face_recognition_init();
    if (ret != ESP_OK) return ret;
    result_mutex = xSemaphoreCreateMutex();
    if (!result_mutex) return ESP_ERR_NO_MEM;
    esp_painter_config_t cfg = {};
    cfg.canvas.width = BSP_LCD_H_RES; cfg.canvas.height = BSP_LCD_V_RES;
    cfg.color_format = ESP_PAINTER_COLOR_FORMAT_RGB565;
    cfg.default_font = &esp_painter_basic_font_20; cfg.swap_rgb565 = true;
    ESP_ERROR_CHECK(esp_painter_init(&cfg, &painter));
    set_screen_dimensions(BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (xTaskCreatePinnedToCore(detect_worker, "face_ai_worker", 16 * 1024, nullptr, 5, &worker, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void app_ai_recognition_ui_init(void) { face_recognition_ui_init(); }

esp_err_t app_ai_detection_init_buffers(size_t cache_line_size)
{
    frame_buffer = (uint16_t *)heap_caps_aligned_calloc(cache_line_size, 1,
        DETECT_WIDTH * DETECT_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    rotated_buffer = (uint16_t *)heap_caps_aligned_calloc(cache_line_size, 1,
        DETECT_WIDTH * DETECT_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    return frame_buffer && rotated_buffer ? ESP_OK : ESP_ERR_NO_MEM;
}

static void unrotate(int &x, int &y, int rotation)
{
    int u = x, v = y;
    if (rotation == 90) { x = v; y = DETECT_HEIGHT - 1 - u; }
    else if (rotation == 180) { x = DETECT_WIDTH - 1 - u; y = DETECT_HEIGHT - 1 - v; }
    else if (rotation == 270) { x = DETECT_WIDTH - 1 - v; y = u; }
}

static void draw_results(uint16_t *buffer, int width, int height, int mode)
{
    std::list<dl::detect::result_t> results;
    int rotation;
    xSemaphoreTake(result_mutex, portMAX_DELAY);
    if (published_mode == mode && esp_timer_get_time() - published_at < 1000000) results = published_results;
    rotation = published_rotation;
    xSemaphoreGive(result_mutex);
    int cx, cy, size; get_center_square_crop(width, height, &cx, &cy, &size);
    for (const auto &res : results) {
        if (res.box.size() < 4) continue;
        int ax = res.box[0], ay = res.box[1], bx = res.box[2], by = res.box[3];
        unrotate(ax, ay, rotation); unrotate(bx, by, rotation);
        int x1 = map_coord_detect_to_screen(std::min(ax, bx), cx, size, DETECT_WIDTH);
        int y1 = map_coord_detect_to_screen(std::min(ay, by), cy, size, DETECT_HEIGHT);
        int x2 = map_coord_detect_to_screen(std::max(ax, bx), cx, size, DETECT_WIDTH);
        int y2 = map_coord_detect_to_screen(std::max(ay, by), cy, size, DETECT_HEIGHT);
        draw_rectangle_rgb(buffer, width, height, x1, y1, x2, y2, 0, 0, 255, 0, 0, 3, false);
        if (mode == AI_DETECT_FACE && res.keypoint.size() == 10) {
            std::vector<int> points;
            for (size_t i = 0; i < res.keypoint.size(); i += 2) {
                int x = res.keypoint[i], y = res.keypoint[i + 1]; unrotate(x, y, rotation);
                points.push_back(map_coord_detect_to_screen(x, cx, size, DETECT_WIDTH));
                points.push_back(map_coord_detect_to_screen(y, cy, size, DETECT_HEIGHT));
            }
            draw_green_points(buffer, points, false);
        }
    }
}

static esp_err_t submit_frame(const uint16_t *input, int width, int height, int mode, bool crop)
{
    if (!frame_buffer) return ESP_ERR_INVALID_STATE;
    bool accepted = false;
    portENTER_CRITICAL(&frame_mux);
    if (!frame_busy) { frame_busy = true; accepted = true; }
    portEXIT_CRITICAL(&frame_mux);
    // Backpressure: drop arrivals while busy. Never overwrite an in-flight frame.
    if (accepted) {
        submitted_at = esp_timer_get_time(); frame_mode = mode; ++sequence;
        if (crop) rgb565_center_crop_resize_nn(input, width, height, frame_buffer, DETECT_WIDTH, DETECT_HEIGHT);
        else memcpy(frame_buffer, input, DETECT_WIDTH * DETECT_HEIGHT * sizeof(uint16_t));
        xTaskNotifyGive(worker);
    }
    return ESP_OK;
}

esp_err_t app_ai_detection_process_frame(uint8_t *buffer, uint32_t width, uint32_t height, int mode)
{
    if (mode != AI_DETECT_FACE && mode != AI_DETECT_PEDESTRIAN) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = submit_frame((uint16_t *)buffer, width, height, mode, true);
    draw_results((uint16_t *)buffer, width, height, mode);
    return ret;
}

esp_err_t app_ai_detection_deinit(void)
{
    // Worker lifetime is the application lifetime; reject unsafe free while running.
    return worker ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t app_humanface_ai_detect(uint16_t *input, uint16_t *output, int width, int height)
{
    esp_err_t ret = submit_frame(input, DETECT_WIDTH, DETECT_HEIGHT, AI_DETECT_FACE, false);
    draw_results(output, width, height, AI_DETECT_FACE); return ret;
}

esp_err_t app_pedestrian_ai_detect(uint16_t *input, uint16_t *output, int width, int height)
{
    esp_err_t ret = submit_frame(input, DETECT_WIDTH, DETECT_HEIGHT, AI_DETECT_PEDESTRIAN, false);
    draw_results(output, width, height, AI_DETECT_PEDESTRIAN); return ret;
}

esp_err_t app_coco_od_detect(uint16_t *data, int width, int height)
{
    ESP_LOGI(TAG, "Detecting COCO objects");
    get_coco_detect();
    auto detect_results = app_coco_detect(data, width, height);
    if (detect_results.size() > 0) {
        uint16_t *rgb_buf = data;
        for (const auto& res : detect_results) {
            const auto& box = res.box;
            
            // Get confidence score
            float score = res.score;
            
            // Only process detections with confidence higher than 30%
            if (score <= 0.30f) {
                continue;  // Skip this detection if confidence is too low
            }
            
            // Check if bounding box is valid
            if (box.size() >= 4 && std::any_of(box.begin(), box.end(), [](int v) { return v != 0; })) {
                draw_rectangle_rgb(rgb_buf, width, height,
                                box[0], box[1], box[2], box[3],
                                0, 0, 255, 0, 0, 5, true);

                // Display COCO detection class name with confidence score
                int category = res.category;
                
                const char* class_name = get_coco_class_name(category);
                char label[64];
                snprintf(label, sizeof(label), "%s", class_name);
                
                draw_label_left_bottom_vertical_rotated(rgb_buf, width, height, box, label);
            }
        }
    }
    return ESP_OK;
}

