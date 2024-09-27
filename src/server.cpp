#include <esp_http_server.h>
#include <esp_timer.h>
#include <esp_camera.h>
#include <esp_int_wdt.h>
#include <esp_task_wdt.h>
#include <Arduino.h>
#include <driver/i2s.h>
#include <limits.h>
#include <string>

#include "audio_config.h"
#include "esp32_cam_pins.h"
#include "index_page.h"

#define MIN_FRAME_TIME 0

#define PART_BOUNDARY "123456789000000000000987654321"

static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

typedef struct
{
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

bool streamKill = false;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;
httpd_handle_t audio_httpd = NULL;

const int sampleRate = SAMPLE_RATE;    // Sample rate of the audio
const int bitsPerSample = SAMPLE_BITS; // Bits per sample of the audio
const int numChannels = 1;             // Number of audio channels (1 for mono, 2 for stereo)
const int bufferSize = DMA_BUF_LEN;    // Buffer size for I2S data transfer
uint32_t elapsedSeconds = 1;
uint32_t lastDoTime = 0;

struct WAVHeader
{
    char chunkId[4] = {};       // 4 bytes
    uint32_t chunkSize = 0;     // 4 bytes
    char format[4] = {};        // 4 bytes
    char subchunk1Id[4] = {};   // 4 bytes
    uint32_t subchunk1Size = 0; // 4 bytes
    uint16_t audioFormat = 0;   // 2 bytes
    uint16_t numChannels = 0;   // 2 bytes
    uint32_t sampleRate = 0;    // 4 bytes
    uint32_t byteRate = 0;      // 4 bytes
    uint16_t blockAlign = 0;    // 2 bytes
    uint16_t bitsPerSample = 0; // 2 bytes
    char subchunk2Id[4] = {};   // 4 bytes
    uint32_t subchunk2Size = 0; // 4 bytes
};

void initialize_wav_header(WAVHeader &header, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels)
{

    strncpy(header.chunkId, "RIFF", 4);
    strncpy(header.format, "WAVE", 4);
    strncpy(header.subchunk1Id, "fmt ", 4);
    strncpy(header.subchunk2Id, "data", 4);

    header.chunkSize = ((sampleRate * bitsPerSample * numChannels) / 8 * elapsedSeconds) + 44 - 8; // Placeholder for Chunk Size (to be updated later)
    header.subchunk1Size = 16;                                                                     // PCM format size (constant for uncompressed audio)
    header.audioFormat = 1;                                                                        // PCM audio format (constant for uncompressed audio)
    header.numChannels = numChannels;
    header.sampleRate = sampleRate;
    header.bitsPerSample = bitsPerSample;
    header.byteRate = (sampleRate * bitsPerSample * numChannels) / 8;
    header.blockAlign = (bitsPerSample * numChannels) / 8;
    header.subchunk2Size = ((sampleRate * bitsPerSample * numChannels) / 8 * UINT32_MAX); // Placeholder for data size (to be updated later)
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static int parse_get_var(char *buf, const char * key, int def)
{
    char _int[16];
    if(httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK){
        return def;
    }
    return atoi(_int);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    return httpd_resp_send(req, (const char *)index_simple_html, index_simple_html_len);
}

static esp_err_t audio_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_OK;

    WAVHeader wavHeader;
    initialize_wav_header(wavHeader, sampleRate, bitsPerSample, numChannels);

    res = httpd_resp_set_type(req, "audio/wav");

    if (res != ESP_OK)
    {
        Serial.println("Audio stream: failed to set HTTP response type");
        return res;
    }

    res = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (res != ESP_OK)
    {
        Serial.println("Audio stream: failed to set HTTP headers");
        return res;
    }

    // Send the initial part of the WAV header
    res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(&wavHeader), sizeof(wavHeader));

    if (res != ESP_OK)
    {
        Serial.println("Audio stream: Sending initial part of WAV header failed");
        return res;
    }
    char i2s_read_buffer[bufferSize] = {};
    size_t bytesRead = 0;

    while (true)
    {

        // Read audio data from I2S DMA
        esp_err_t result = i2s_read(I2S_PORT, &i2s_read_buffer, bufferSize, &bytesRead, portMAX_DELAY);

        // Send data to client
        if (bytesRead > 0)
        {
            res = httpd_resp_send_chunk(req, i2s_read_buffer, bytesRead);
        }
        if (res != ESP_OK)
        {
            // This is the error exit point from the stream loop.
            // We end the stream here only if a Hard failure has been encountered or the connection has been interrupted.
            Serial.printf("Audio stream failed, code = %i : %s\r\n", res, esp_err_to_name(res));
            break;
        }
        if ((res != ESP_OK) || streamKill)
        {
            // We end the stream here when a kill is signalled.
            Serial.printf("Audio stream killed\r\n");
            break;
        }
    }
    Serial.println("Audio stream ended");
    //   i2s_driver_uninstall(I2S_PORT);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t capture_handler(httpd_req_t *req)
{

    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;

    fb = esp_camera_fb_get();
    if (!fb)
    {
        Serial.println("CAPTURE: failed to acquire frame");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    fb = NULL;
    return res;
}

static esp_err_t motion_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_OK;
    uint8_t motion = digitalRead(GPIO_2);
    std::string buf = "null";
    if (motion)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        buf = "{\"motion\":true}";
        res = httpd_resp_send(req, buf.c_str(), buf.length());
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        buf = "{\"motion\":false}";
        res = httpd_resp_send(req, buf.c_str(), buf.length());
    }

    return res;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[64];

    streamKill = false;

    Serial.println("Camera stream requested");

    static int64_t last_frame = 0;
    if (!last_frame)
    {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        Serial.println("Camera stream: failed to set HTTP response type");
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (res == ESP_OK)
    {
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("Camera stream: failed to acquire frame");
            res = ESP_FAIL;
        }
        else
        {
            if (fb->format != PIXFORMAT_JPEG)
            {
                Serial.println("Camera stream: Non-JPEG frame returned by camera module");
                res = ESP_FAIL;
            }
            else
            {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if (res == ESP_OK)
        {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf)
        {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK)
        {
            // This is the error exit point from the stream loop.
            // We end the stream here only if a Hard failure has been encountered or the connection has been interrupted.
            Serial.printf("Camera stream failed, code = %i : %s\r\n", res, esp_err_to_name(res));
            break;
        }
        if ((res != ESP_OK) || streamKill)
        {
            // We end the stream here when a kill is signalled.
            Serial.printf("Camera stream killed\r\n");
            break;
        }
        int64_t frame_time = esp_timer_get_time() - last_frame;
        frame_time /= 1000;
        int32_t frame_delay = (MIN_FRAME_TIME > frame_time) ? MIN_FRAME_TIME - frame_time : 0;
        delay(frame_delay);

        last_frame = esp_timer_get_time();
    }

    Serial.println("Camera stream ended");
    last_frame = 0;
    return res;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    Serial.println("\r\nStream stop requested via Web");
    streamKill = true;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    log_i("%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize")) {
        if (s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
        res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))
        res = s->set_vflip(s, val);
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);

    else {
        log_i("Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t xclk_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _xclk[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int xclk = atoi(_xclk);
    log_i("Set XCLK: %d MHz", xclk);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];
    char _val[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK ||
        httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    int val = atoi(_val);
    log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_reg(s, reg, mask, val);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->get_reg(s, reg, mask);
    if (res < 0) {
        return httpd_resp_send_500(req);
    }
    log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

    char buffer[20];
    const char * val = itoa(res, buffer, 10);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, val, strlen(val));
}

static esp_err_t pll_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int bypass = parse_get_var(buf, "bypass", 0);
    int mul = parse_get_var(buf, "mul", 0);
    int sys = parse_get_var(buf, "sys", 0);
    int root = parse_get_var(buf, "root", 0);
    int pre = parse_get_var(buf, "pre", 0);
    int seld5 = parse_get_var(buf, "seld5", 0);
    int pclken = parse_get_var(buf, "pclken", 0);
    int pclk = parse_get_var(buf, "pclk", 0);
    free(buf);

    log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d", bypass, mul, sys, root, pre, seld5, pclken, pclk);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int startX = parse_get_var(buf, "sx", 0);
    int startY = parse_get_var(buf, "sy", 0);
    int endX = parse_get_var(buf, "ex", 0);
    int endY = parse_get_var(buf, "ey", 0);
    int offsetX = parse_get_var(buf, "offx", 0);
    int offsetY = parse_get_var(buf, "offy", 0);
    int totalX = parse_get_var(buf, "tx", 0);
    int totalY = parse_get_var(buf, "ty", 0);
    int outputX = parse_get_var(buf, "ox", 0);
    int outputY = parse_get_var(buf, "oy", 0);
    bool scale = parse_get_var(buf, "scale", 0) == 1;
    bool binning = parse_get_var(buf, "binning", 0) == 1;
    free(buf);

    log_i("Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u", startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

void start_camera_server(uint16_t http_port, uint16_t stream_port, uint16_t audio_port)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16; // we use more than the default 8 (on port 80)

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL};

    httpd_uri_t motion_uri = {
        .uri = "/motion",
        .method = HTTP_GET,
        .handler = motion_handler,
        .user_ctx = NULL};

    httpd_uri_t audio_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = audio_handler,
        .user_ctx = NULL};

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL};

    httpd_uri_t stop_uri = {
        .uri = "/stop",
        .method = HTTP_GET,
        .handler = stop_handler,
        .user_ctx = NULL};

    httpd_uri_t stream_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL};

        httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL};

    httpd_uri_t xclk_uri = {
        .uri = "/xclk",
        .method = HTTP_GET,
        .handler = xclk_handler,
        .user_ctx = NULL};

    httpd_uri_t reg_uri = {
        .uri = "/reg",
        .method = HTTP_GET,
        .handler = reg_handler,
        .user_ctx = NULL};

    httpd_uri_t greg_uri = {
        .uri = "/greg",
        .method = HTTP_GET,
        .handler = greg_handler,
        .user_ctx = NULL};

    httpd_uri_t pll_uri = {
        .uri = "/pll",
        .method = HTTP_GET,
        .handler = pll_handler,
        .user_ctx = NULL};

    httpd_uri_t win_uri = {
        .uri = "/resolution",
        .method = HTTP_GET,
        .handler = win_handler,
        .user_ctx = NULL};

    config.server_port = http_port;
    config.ctrl_port = http_port;
    Serial.printf("Starting web server on port: '%d'\r\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &motion_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &stop_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);

        httpd_register_uri_handler(camera_httpd, &xclk_uri);
        httpd_register_uri_handler(camera_httpd, &reg_uri);
        httpd_register_uri_handler(camera_httpd, &greg_uri);
        httpd_register_uri_handler(camera_httpd, &pll_uri);
        httpd_register_uri_handler(camera_httpd, &win_uri);
    }

    config.server_port = stream_port;
    config.ctrl_port = stream_port;
    Serial.printf("Starting stream server on port: '%d'\r\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }

    config.server_port = audio_port;
    config.ctrl_port = audio_port;
    Serial.printf("Starting audio server on port: '%d'\r\n", config.server_port);
    if (httpd_start(&audio_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(audio_httpd, &audio_uri);
    }
}
