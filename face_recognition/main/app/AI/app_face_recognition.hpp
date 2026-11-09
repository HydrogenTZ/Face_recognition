#pragma once
#include <list>
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"
#include "esp_err.h"

// Model/database state is exclusively owned by the AI worker.
esp_err_t face_recognition_init();
void face_recognition_commands();
void face_recognition_process(const dl::image::img_t &image,
                             const std::list<dl::detect::result_t> &faces,
                             uint32_t frame_id, int64_t submitted_us,
                             int64_t detect_start_us, int64_t detect_end_us);
void face_recognition_ui_init(); // Call under the LVGL display lock.
bool face_recognition_command(const char *line);
int face_recognition_rotation(); // Clockwise degrees, read by AI worker only.
