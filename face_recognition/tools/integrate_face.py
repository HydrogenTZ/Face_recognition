"""One-time integration against the saved baseline; explicit match checks prevent drift."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]

def replace(path, old, new):
    p = root / path
    text = p.read_text(encoding='utf-8')
    assert old in text, (path, old[:100])
    p.write_text(text.replace(old, new), encoding='utf-8')

# The registry's 0.2.0 package predates ESP-DL 3.1.3's database constructor.
replace('components/human_face_recognition/human_face_recognition.hpp',
        '                        dl::recognition::db_type_t db_type = dl::recognition::DB_FATFS_FLASH,\n', '')
replace('components/human_face_recognition/human_face_recognition.hpp',
        'DataBase(db_path, db_type, 512)', 'DataBase(db_path, 512)')
(root / 'components/human_face_recognition/idf_component.yml').write_text('''version: "0.2.0"
license: "MIT"
description: "Official MFN/MBF models; local ESP-DL 3.1.3 compatibility adaptation"
dependencies:
  espressif/esp-dl:
    version: "==3.1.3"
''', encoding='utf-8')
replace('main/CMakeLists.txt', 'nvs_flash human_face_detect', 'nvs_flash human_face_recognition human_face_detect')
with (root / 'partitions.csv').open('a', encoding='utf-8') as f:
    f.write('face_store, data, nvs, 0xA10000, 0x40000,\n')
replace('main/main.c', '#include "app_ai_detect.h"', '#include "app_ai_detect.h"\n#include "app_face_recognition.hpp"')
# main.c is C; expose the UI initializer through the existing C API instead.
replace('main/main.c', '#include "app_face_recognition.hpp"', '')
replace('main/main.c', '    ui_extra_init();', '    ui_extra_init();\n    app_ai_recognition_ui_init();')
replace('main/main.c', '    ESP_LOGI(TAG, "Application initialization completed");', '''    bsp_display_lock(0);
    ui_extra_goto_page(UI_PAGE_AI_DETECT);
    bsp_display_unlock();
    ESP_LOGI(TAG, "Application initialization completed");''')
replace('main/ui/ui_extra.c', 'current_ai_detect_mode = AI_DETECT_PEDESTRIAN;', 'current_ai_detect_mode = AI_DETECT_FACE;')
replace('main/app/AI/app_ai_detect.h', 'esp_err_t app_ai_detect_init(void);', 'esp_err_t app_ai_detect_init(void);\nvoid app_ai_recognition_ui_init(void);')

# Keep COCO drawing helpers and the album path, replacing only realtime AI ownership.
p = root / 'main/app/AI/app_ai_detect.cpp'
old = p.read_text(encoding='utf-8')
helpers = old[old.index('static inline void get_center_square_crop'):old.index('/**\n * @brief Structure for managing AI detection buffers')]
coco = old[old.index('esp_err_t app_coco_od_detect('):old.index('esp_err_t app_humanface_ai_detect(')]
prefix = '''#include <algorithm>
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
static bool frame_busy;
static int frame_mode;
static uint32_t sequence;
static int64_t submitted_at;
static std::list<dl::detect::result_t> published_results;
static int published_mode = -1;
static int64_t published_at;

'''
body = '''
static void detect_worker(void *)
{
    while (true) {
        // Commands still work with the camera page closed or no camera frames.
        face_recognition_commands();
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) continue;
        int mode = frame_mode;
        uint32_t frame_id = sequence;
        int64_t submitted = submitted_at;
        int64_t t0 = esp_timer_get_time();
        auto results = mode == AI_DETECT_FACE
            ? app_humanface_detect(frame_buffer, DETECT_WIDTH, DETECT_HEIGHT)
            : app_pedestrian_detect(frame_buffer, DETECT_WIDTH, DETECT_HEIGHT);
        int64_t t1 = esp_timer_get_time();
        if (mode == AI_DETECT_FACE) {
            dl::image::img_t image;
            image.data = frame_buffer; image.width = DETECT_WIDTH; image.height = DETECT_HEIGHT;
            image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;
            face_recognition_process(image, results, frame_id, submitted, t0, t1);
        }
        xSemaphoreTake(result_mutex, portMAX_DELAY);
        published_results = std::move(results);
        published_mode = mode; published_at = esp_timer_get_time();
        xSemaphoreGive(result_mutex);
        portENTER_CRITICAL(&frame_mux); frame_busy = false; portEXIT_CRITICAL(&frame_mux);
    }
}

esp_err_t app_ai_detect_init(void)
{
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
    return frame_buffer ? ESP_OK : ESP_ERR_NO_MEM;
}

static void draw_results(uint16_t *buffer, int width, int height, int mode)
{
    std::list<dl::detect::result_t> results;
    xSemaphoreTake(result_mutex, portMAX_DELAY);
    if (published_mode == mode && esp_timer_get_time() - published_at < 1000000) results = published_results;
    xSemaphoreGive(result_mutex);
    int cx, cy, size; get_center_square_crop(width, height, &cx, &cy, &size);
    for (const auto &res : results) {
        if (res.box.size() < 4) continue;
        int x1 = map_coord_detect_to_screen(res.box[0], cx, size, DETECT_WIDTH);
        int y1 = map_coord_detect_to_screen(res.box[1], cy, size, DETECT_HEIGHT);
        int x2 = map_coord_detect_to_screen(res.box[2], cx, size, DETECT_WIDTH);
        int y2 = map_coord_detect_to_screen(res.box[3], cy, size, DETECT_HEIGHT);
        draw_rectangle_rgb(buffer, width, height, x1, y1, x2, y2, 0, 0, 255, 0, 0, 3, false);
        if (mode == AI_DETECT_FACE && res.keypoint.size() == 10) {
            std::vector<int> points;
            for (size_t i = 0; i < res.keypoint.size(); i += 2) {
                points.push_back(map_coord_detect_to_screen(res.keypoint[i], cx, size, DETECT_WIDTH));
                points.push_back(map_coord_detect_to_screen(res.keypoint[i + 1], cy, size, DETECT_HEIGHT));
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
'''
# Legacy album helper uses its own results, never shared with the AI worker.
coco = coco.replace('    detect_results = app_coco_detect(data, width, height);', '    get_coco_detect();\n    auto detect_results = app_coco_detect(data, width, height);')
p.write_text(prefix + helpers + body + '\n' + coco, encoding='utf-8')
# The getter must initialize lazy pedestrian instances before the wrapper's run().
replace('main/app/AI/app_pedestrian_detect.cpp', '    auto &detect_results = detect->run(img);', '    if (!detect) get_pedestrian_detect();\n    auto &detect_results = detect->run(img);')
print('Face recognition integration complete')
