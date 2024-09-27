#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <driver/i2s.h>

#include "config.h"
#include "esp32_cam_pins.h"
#include "audio_config.h"

#define CAMERA_MODEL_AI_THINKER

esp_err_t camera_init();
esp_err_t mic_i2s_init();
void wifi_setup();
void start_camera_server(uint16_t, uint16_t, uint16_t);

IPAddress ip;
IPAddress gateway;
IPAddress subnet;

void setup()
{
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  wifi_setup();
  camera_init();
  mic_i2s_init();
  sensor_t *sensors = esp_camera_sensor_get();
  sensors->set_vflip(sensors, 1);
  start_camera_server(80, STREAM_PORT, AUDIO_PORT);
}

void loop()
{
}

// 1. Camera

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PWDN,
    .pin_reset = RESET_PIN,
    .pin_xclk = CAM_XCLK,
    .pin_sccb_sda = CAM_SDA,
    .pin_sccb_scl = CAM_SCL,
    .pin_d7 = CAM_D7,
    .pin_d6 = CAM_D6,
    .pin_d5 = CAM_D5,
    .pin_d4 = CAM_D4,
    .pin_d3 = CAM_D3,
    .pin_d2 = CAM_D2,
    .pin_d1 = CAM_D1,
    .pin_d0 = CAM_D0,
    .pin_vsync = CAM_VSYNC,
    .pin_href = CAM_HREF,
    .pin_pclk = CAM_PCLK,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_UXGA,
    .jpeg_quality = 10,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_LATEST};

esp_err_t camera_init()
{
  return esp_camera_init(&camera_config);
}

// 2. Microphone

esp_err_t mic_i2s_init()
{
  esp_err_t res = ESP_OK;
  i2s_config_t i2sConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), // Use RX mode for audio input
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = i2s_bits_per_sample_t(SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono audio
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = DMA_BUF_COUNT,
      .dma_buf_len = DMA_BUF_LEN,
      .use_apll = false};
  res = i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  if (res == ESP_OK)
  {
    i2s_pin_config_t pinConfig = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD};
    res = i2s_set_pin(I2S_PORT, &pinConfig);
  }

  return res;
}

// 3. WiFi
static void flashLED(int flashtime)
{
  digitalWrite(LED_PIN, LED_ON);
  delay(flashtime);
  digitalWrite(LED_PIN, LED_OFF);
}

void wifi_setup()
{
  Serial.println("Starting wifi");
  // Set IP address
  ip.fromString(IP_ADDRESS);
  gateway.fromString(GATEWAY);
  subnet.fromString(SUBNET);
  WiFi.config(ip, gateway, subnet);

  // Begin connection
  WiFi.begin(SSID, PASSWORD);
  WiFi.setSleep(false);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print("|\r");
    flashLED(300);
    delay(200);
    Serial.print("/\r");
    flashLED(300);
    delay(200);
    Serial.print("-\r");
    flashLED(300);
    delay(200);
    Serial.print("\\\r");
    flashLED(300);
    delay(200);
    Serial.print("|\r");
    flashLED(300);
    delay(200);
    Serial.print("/\r");
    flashLED(300);
    delay(200);
    Serial.print("-\r");
    flashLED(300);
    delay(200);
    Serial.print("\\\r");
    flashLED(300);
    delay(200);
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP: ");
  Serial.println(ip);
}
