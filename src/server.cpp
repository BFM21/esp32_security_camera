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
  char chunkId[4] = {};        // 4 bytes
  uint32_t chunkSize = 0;     // 4 bytes
  char format[4] = {};         // 4 bytes
  char subchunk1Id[4] = {};    // 4 bytes
  uint32_t subchunk1Size = 0; // 4 bytes
  uint16_t audioFormat = 0;   // 2 bytes
  uint16_t numChannels = 0;   // 2 bytes
  uint32_t sampleRate = 0;    // 4 bytes
  uint32_t byteRate = 0;      // 4 bytes
  uint16_t blockAlign = 0;    // 2 bytes
  uint16_t bitsPerSample = 0; // 2 bytes
  char subchunk2Id[4] = {};    // 4 bytes
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

static esp_err_t index_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "identity");
  return httpd_resp_send(req, (const char *)index_simple_html, index_simple_html_len);
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

  config.server_port = http_port;
  config.ctrl_port = http_port;
  Serial.printf("Starting web server on port: '%d'\r\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK)
  {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &motion_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &stop_uri);
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
