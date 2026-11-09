#include "app_face_recognition.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <vector>
#include "human_face_recognition.hpp"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "ui_extra.h"

namespace {
constexpr int DIM = 512, MAX_PEOPLE = 32, MAX_SAMPLES = 1000;
constexpr uint32_t RECORD_VERSION = 0x4d464e01; // MFN S8 v1, 512 floats
struct Record { uint32_t version; char name[24]; float feature[DIM]; };
struct Person { int id; Record record; };
struct Command { char line[96]; };
struct Sample {
    uint32_t frame;
    int id;
    float similarity;
    int64_t detection, feature, match, algorithm, pipeline;
};
HumanFaceFeat *model;
nvs_handle_t db;
std::vector<Person> people;
QueueHandle_t commands;
SemaphoreHandle_t status_mutex;
char status_text[256] = "Face recognition starting";
char pending_name[24] = {};
int64_t enroll_deadline;
float threshold = 0.50f;
int rotation_degrees = 270; // Mounted camera produces a face rotated clockwise by 90 degrees.
Sample samples[MAX_SAMPLES];
int collected, target, warmup;
int no_face, multi_face, invalid_face, unknown;
int64_t bench_started, first_sample_started, last_sample_finished;
bool dump_pending;
bool snapshot_pending;
lv_obj_t *panel, *label;

void status(const char *fmt, ...)
{
    char text[256];
    va_list args; va_start(args, fmt); vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    snprintf(status_text, sizeof(status_text), "%s", text);
    xSemaphoreGive(status_mutex);
}
bool valid_feature(const float *f)
{
    float norm = 0;
    for (int i = 0; i < DIM; ++i) {
        if (!std::isfinite(f[i])) return false;
        norm += f[i] * f[i];
    }
    return std::isfinite(norm) && norm > 0.90f && norm < 1.10f;
}
void key_for(int id, char *key) { snprintf(key, 12, "p%02d", id); }
void enter_face_page()
{
    if (bsp_display_lock((uint32_t)-1)) {
        if (ui_extra_get_current_page() != UI_PAGE_AI_DETECT) ui_extra_goto_page(UI_PAGE_AI_DETECT);
        ui_extra_set_ai_face_mode();
        bsp_display_unlock();
    }
}
void summary()
{
    if (!collected) { printf("FACE_STATS no_samples\n"); return; }
    std::vector<int64_t> times;
    int64_t det = 0, feat = 0, match = 0, pipe = 0, sum = 0;
    for (int i = 0; i < collected; ++i) {
        const Sample &s = samples[i];
        times.push_back(s.algorithm); sum += s.algorithm;
        det += s.detection; feat += s.feature; match += s.match; pipe += s.pipeline;
    }
    std::sort(times.begin(), times.end());
    double elapsed = (last_sample_finished - first_sample_started) / 1e6;
    printf("FACE_STATS n=%d,db=%u,threshold=%.3f,mean_ms=%.3f,median_ms=%.3f,p95_ms=%.3f,min_ms=%.3f,max_ms=%.3f,detect_ms=%.3f,feature_ms=%.3f,match_ms=%.3f,pipeline_ms=%.3f,fps=%.3f,no_face=%d,multi_face=%d,invalid_face=%d,unknown=%d\n",
           collected, (unsigned)people.size(), threshold, sum / (1000.0 * collected),
           (collected % 2 ? times[collected / 2] : (times[collected / 2 - 1] + times[collected / 2]) / 2.0) / 1000.0,
           times[(95 * collected + 99) / 100 - 1] / 1000.0, times.front() / 1000.0, times.back() / 1000.0,
           det / (1000.0 * collected), feat / (1000.0 * collected), match / (1000.0 * collected),
           pipe / (1000.0 * collected), collected / elapsed, no_face, multi_face, invalid_face, unknown);
    fflush(stdout);
}
void dump()
{
    printf("FACE_CSV_BEGIN\nframe,id,similarity,detect_us,feature_us,match_us,algorithm_us,pipeline_us\n");
    for (int i = 0; i < collected; ++i) {
        const Sample &s = samples[i];
        printf("%lu,%d,%.6f,%lld,%lld,%lld,%lld,%lld\n", (unsigned long)s.frame, s.id, s.similarity,
               (long long)s.detection, (long long)s.feature, (long long)s.match,
               (long long)s.algorithm, (long long)s.pipeline);
        vTaskDelay(1);
    }
    printf("FACE_CSV_END\n"); fflush(stdout);
}
void console_task(void *)
{
    char line[96]; size_t pos = 0;
    while (true) {
        int c = getchar();
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\n' || c == '\r') {
            line[pos] = 0;
            if (pos) face_recognition_command(line);
            pos = 0;
        } else if (c == 8 || c == 127) {
            if (pos) --pos;
        } else if (c >= 32 && c < 127 && pos < sizeof(line) - 1) line[pos++] = (char)c;
    }
}
void button_cb(lv_event_t *event)
{
    face_recognition_command(static_cast<const char *>(lv_event_get_user_data(event)));
}
void refresh_ui(lv_timer_t *)
{
    bool visible = ui_extra_get_current_page() == UI_PAGE_AI_DETECT && ui_extra_get_ai_detect_mode() == AI_DETECT_FACE;
    if (!visible) { lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    char text[256];
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    memcpy(text, status_text, sizeof(text));
    xSemaphoreGive(status_mutex);
    lv_label_set_text(label, text);
}
}

esp_err_t face_recognition_init()
{
    status_mutex = xSemaphoreCreateMutex();
    commands = xQueueCreate(8, sizeof(Command));
    if (!status_mutex || !commands) return ESP_ERR_NO_MEM;
    // Dedicated partition; never erase application settings or the SD card.
    esp_err_t ret = nvs_flash_init_partition("face_store");
    if (ret != ESP_OK) return ret;
    ret = nvs_open_from_partition("face_store", "mfn_v1", NVS_READWRITE, &db);
    if (ret != ESP_OK) return ret;
    for (int id = 1; id <= MAX_PEOPLE; ++id) {
        Person p = {}; p.id = id; char key[12]; key_for(id, key);
        size_t length = sizeof(p.record);
        ret = nvs_get_blob(db, key, &p.record, &length);
        if (ret == ESP_ERR_NVS_NOT_FOUND) continue;
        if (ret != ESP_OK || length != sizeof(p.record) || p.record.version != RECORD_VERSION ||
            !memchr(p.record.name, 0, sizeof(p.record.name)) || !valid_feature(p.record.feature)) {
            ESP_LOGE("face", "Invalid saved record %d; not loaded", id); continue;
        }
        people.push_back(p);
    }
    model = new HumanFaceFeat(nullptr, HumanFaceFeat::MFN_S8_V1);
    status("Ready: %u enrolled\nEnroll one clear face", (unsigned)people.size());
    printf("FACE_READY model=MFN_S8_V1 dim=512 db=%u threshold=%.3f\n", (unsigned)people.size(), threshold);
    printf("FACE_CONFIG idf=%s cpu_mhz=%d psram_mhz=%d input=240x240 rotation=%d compiler_debug=%d\n",
           IDF_VER, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_SPIRAM_SPEED, rotation_degrees,
#ifdef CONFIG_COMPILER_OPTIMIZATION_DEBUG
           1
#else
           0
#endif
    );
    if (xTaskCreate(console_task, "face_console", 4096, nullptr, 3, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool face_recognition_command(const char *line)
{
    if (!commands || !line || strlen(line) >= sizeof(Command::line)) return false;
    Command c = {}; snprintf(c.line, sizeof(c.line), "%s", line);
    if (xQueueSend(commands, &c, 0) != pdTRUE) { printf("FACE_ERROR command_queue_full\n"); return false; }
    return true;
}

int face_recognition_rotation() { return rotation_degrees; }

void face_recognition_commands()
{
    Command c;
    while (xQueueReceive(commands, &c, 0) == pdTRUE) {
        char verb[24] = {}, arg[32] = {};
        sscanf(c.line, "%23s %31s", verb, arg);
        if (!strcmp(verb, "face")) { printf("FACE_ERROR omit_prefix_use_help\n"); continue; }
        if (!strcmp(verb, "help")) {
            printf("FACE_HELP enroll [name] | list | delete ID | clear YES | threshold 0.50 | rotate 0/90/180/270 | bench [200] | stats | dump | stop | facepage | status | snapshot\n");
        } else if (!strcmp(verb, "rotate") && !target) {
            if (strcmp(arg, "0") && strcmp(arg, "90") && strcmp(arg, "180") && strcmp(arg, "270")) { printf("FACE_ERROR rotation_0_90_180_270\n"); continue; }
            rotation_degrees = atoi(arg); printf("FACE_ROTATION clockwise=%d\n", rotation_degrees);
        } else if (!strcmp(verb, "status")) {
            xSemaphoreTake(status_mutex, portMAX_DELAY);
            printf("FACE_STATUS page=%d mode=%d rotation=%d %s\n", (int)ui_extra_get_current_page(), (int)ui_extra_get_ai_detect_mode(), rotation_degrees, status_text);
            xSemaphoreGive(status_mutex);
        } else if (!strcmp(verb, "snapshot") && !target) {
            snapshot_pending = true;
        } else if (!strcmp(verb, "facepage")) {
            enter_face_page();
        } else if (!strcmp(verb, "list")) {
            printf("FACE_LIST count=%u\n", (unsigned)people.size());
            for (const auto &p : people) printf("FACE_PERSON id=%d name=%s\n", p.id, p.record.name);
        } else if (!strcmp(verb, "enroll") && !target) {
            if (people.size() >= MAX_PEOPLE) { printf("FACE_ERROR database_full\n"); continue; }
            enter_face_page();
            snprintf(pending_name, sizeof(pending_name), "%.*s", 23, arg[0] ? arg : "person");
            enroll_deadline = esp_timer_get_time() + 15000000;
            status("ENROLL %s\nHold one clear face (15s)", pending_name);
            printf("FACE_ENROLL_WAIT name=%s timeout_s=15\n", pending_name);
        } else if (!strcmp(verb, "clear") && !strcmp(arg, "YES") && !target) {
            esp_err_t ret = nvs_erase_all(db);
            if (ret == ESP_OK) ret = nvs_commit(db);
            if (ret == ESP_OK) people.clear();
            pending_name[0] = 0;
            printf("FACE_CLEAR result=%s\n", esp_err_to_name(ret));
            status("Database: %u enrolled", (unsigned)people.size());
        } else if (!strcmp(verb, "delete") && !target) {
            int id = atoi(arg);
            auto it = std::find_if(people.begin(), people.end(), [id](const Person &p) { return p.id == id; });
            if (it == people.end()) { printf("FACE_ERROR id_not_found\n"); continue; }
            char key[12]; key_for(id, key);
            esp_err_t ret = nvs_erase_key(db, key);
            if (ret == ESP_OK) ret = nvs_commit(db);
            if (ret == ESP_OK) people.erase(it);
            printf("FACE_DELETE id=%d result=%s\n", id, esp_err_to_name(ret));
        } else if (!strcmp(verb, "threshold") && !target) {
            char *end = nullptr; float v = strtof(arg, &end);
            if (!arg[0] || *end || !std::isfinite(v) || v < 0 || v > 1) { printf("FACE_ERROR threshold_range_0_1\n"); continue; }
            threshold = v; printf("FACE_THRESHOLD %.3f\n", threshold);
        } else if (!strcmp(verb, "bench") && !target) {
            if (people.empty()) { printf("FACE_ERROR enroll_before_benchmark\n"); continue; }
            int n = arg[0] ? atoi(arg) : 200;
            if (n < 1 || n > MAX_SAMPLES) { printf("FACE_ERROR bench_range_1_1000\n"); continue; }
            enter_face_page();
            target = n; warmup = 20; collected = 0;
            no_face = multi_face = invalid_face = unknown = 0; pending_name[0] = 0;
            bench_started = esp_timer_get_time(); first_sample_started = 0;
            status("Benchmark warmup: 20\nKeep one face visible");
            printf("FACE_BENCH_BEGIN target=%d warmup=20 db=%u threshold=%.3f rotation=%d timeout_s=180\n", target, (unsigned)people.size(), threshold, rotation_degrees);
        } else if (!strcmp(verb, "stats")) { summary();
        } else if (!strcmp(verb, "dump")) { if (target) dump_pending = true; else dump();
        } else if (!strcmp(verb, "stop")) {
            target = 0; pending_name[0] = 0; summary(); status("Stopped; %d samples", collected);
        } else { printf("FACE_ERROR invalid_or_busy use_help\n"); }
    }
    if (pending_name[0] && esp_timer_get_time() > enroll_deadline) {
        pending_name[0] = 0; status("Enrollment timed out"); printf("FACE_ENROLL_TIMEOUT\n");
    }
    if (target && esp_timer_get_time() - bench_started > 180000000) {
        target = 0; printf("FACE_BENCH_TIMEOUT\n"); summary(); status("Benchmark timeout: %d samples", collected);
        if (dump_pending) { dump_pending = false; dump(); }
    }
}

void face_recognition_process(const dl::image::img_t &image, const std::list<dl::detect::result_t> &faces,
                             uint32_t frame_id, int64_t submitted_us, int64_t t0, int64_t t1)
{
    if (snapshot_pending && !target) {
        snapshot_pending = false;
        printf("FACE_SNAPSHOT_BEGIN width=%d height=%d faces=%u\n", image.width, image.height, (unsigned)faces.size());
        const uint16_t *pixels = (const uint16_t *)image.data;
        char row[4 * 240 + 2];
        const char hex[] = "0123456789abcdef";
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                uint16_t value = pixels[y * image.width + x];
                for (int n = 0; n < 4; ++n) row[4 * x + n] = hex[(value >> (12 - 4 * n)) & 15];
            }
            row[4 * image.width] = '\n'; row[4 * image.width + 1] = 0;
            fputs(row, stdout);
            vTaskDelay(1); // UART VFS polling must not starve the idle watchdog.
        }
        printf("FACE_SNAPSHOT_END\n"); fflush(stdout);
    }
    if (faces.size() != 1) {
        if (target) { if (faces.empty()) ++no_face; else ++multi_face; }
        if (!pending_name[0]) status("%s\nDatabase: %u", faces.empty() ? "No face" : "Only ONE face please", (unsigned)people.size());
        return;
    }
    const auto &face = faces.front();
    if (face.keypoint.size() != 10 || face.box.size() < 4 || face.box[2] - face.box[0] < 45 || face.box[3] - face.box[1] < 45) {
        if (target) ++invalid_face;
        if (!pending_name[0]) status("Move closer: face too small");
        return;
    }
    int64_t tf0 = esp_timer_get_time();
    dl::TensorBase *feat = model->run(image, face.keypoint);
    int64_t tf1 = esp_timer_get_time();
    if (!feat || feat->dtype != dl::DATA_TYPE_FLOAT || feat->size != DIM || !valid_feature((float *)feat->data)) {
        if (target) ++invalid_face;
        printf("FACE_ERROR invalid_feature\n"); return;
    }
    const float *f = (float *)feat->data;
    if (pending_name[0]) {
        int id = 1;
        while (std::any_of(people.begin(), people.end(), [id](const Person &p) { return p.id == id; })) ++id;
        Person p = {}; p.id = id; p.record.version = RECORD_VERSION;
        snprintf(p.record.name, sizeof(p.record.name), "%s", pending_name);
        memcpy(p.record.feature, f, DIM * sizeof(float));
        char key[12]; key_for(id, key);
        esp_err_t ret = nvs_set_blob(db, key, &p.record, sizeof(p.record));
        if (ret == ESP_OK) ret = nvs_commit(db);
        if (ret == ESP_OK) {
            people.push_back(p); status("Enrolled ID %d: %s\nDatabase: %u", id, p.record.name, (unsigned)people.size());
            printf("FACE_ENROLLED id=%d name=%s count=%u\n", id, p.record.name, (unsigned)people.size());
        } else { status("Enrollment failed: %s", esp_err_to_name(ret)); printf("FACE_ERROR save=%s\n", esp_err_to_name(ret)); }
        pending_name[0] = 0; return;
    }
    int64_t tm0 = esp_timer_get_time();
    float best = -1.0f; const Person *winner = nullptr;
    for (const auto &p : people) {
        float similarity = 0;
        for (int i = 0; i < DIM; ++i) similarity += f[i] * p.record.feature[i];
        if (similarity > best) { best = similarity; winner = &p; }
    }
    int id = winner && best >= threshold ? winner->id : 0;
    int64_t tdone = esp_timer_get_time();
    Sample s = {frame_id, id, best, t1 - t0, tf1 - tf0, tdone - tm0, tdone - t0, tdone - submitted_us};
    if (target) {
        if (warmup) { --warmup; status("Benchmark warmup: %d", warmup); return; }
        if (!collected) first_sample_started = submitted_us;
        last_sample_finished = tdone;
        samples[collected++] = s;
        if (!id) ++unknown;
        status("Benchmark %d / %d\nID %d sim %.3f\nAlgorithm %.1f ms", collected, target, id, best, s.algorithm / 1000.0);
        if (collected >= target) {
            target = 0; summary(); printf("FACE_BENCH_DONE samples=%d\n", collected);
            status("Benchmark done: %d\nSend stats / dump", collected);
            if (dump_pending) { dump_pending = false; dump(); }
        }
    } else {
        status("%s ID %d  sim %.3f\nDet %.1f + Feat %.1f ms\nTotal %.1f ms | DB %u",
               id ? winner->record.name : "Unknown", id, best, s.detection / 1000.0,
               s.feature / 1000.0, s.algorithm / 1000.0, (unsigned)people.size());
        static int64_t last_log;
        if (tdone - last_log >= 1000000) {
            printf("FACE_RESULT frame=%lu id=%d name=%s sim=%.4f detect_ms=%.3f feature_ms=%.3f match_ms=%.3f total_ms=%.3f pipeline_ms=%.3f db=%u\n",
                   (unsigned long)frame_id, id, id ? winner->record.name : "Unknown", best,
                   s.detection / 1000.0, s.feature / 1000.0, s.match / 1000.0, s.algorithm / 1000.0,
                   s.pipeline / 1000.0, (unsigned)people.size());
            last_log = tdone;
        }
    }
}

void face_recognition_ui_init()
{
    panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, 310, 128); lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    label = lv_label_create(panel); lv_obj_set_width(label, 292);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    const char *texts[] = {"Enroll", "Bench 200", "Stats"};
    const char *cmds[] = {"enroll", "bench 200", "stats"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *button = lv_btn_create(panel);
        lv_obj_set_size(button, 92, 30); lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, i * 99, 0);
        lv_obj_add_event_cb(button, button_cb, LV_EVENT_CLICKED, (void *)cmds[i]);
        lv_obj_t *text = lv_label_create(button); lv_label_set_text(text, texts[i]); lv_obj_center(text);
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(refresh_ui, 200, nullptr);
}
