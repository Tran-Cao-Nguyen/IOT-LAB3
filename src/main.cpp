#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <DHT20.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Arduino.h>
#include <Wire.h>
#include <WiFiClientSecure.h>
#include <Arduino_MQTT_Client.h>
#include <Server_Side_RPC.h>
#include <ThingsBoard.h>
#include <Update.h>
#include <ArduinoJson.h>

// Cảm biến DHT20
DHT20 dht20;

// Task handles
TaskHandle_t TaskHandle = NULL;

// Thông tin WiFi
constexpr char WIFI_SSID[] = "Mr.Highland"; 
constexpr char WIFI_PASSWORD[] = "09012003"; 

// Cấu hình ThingsBoard
constexpr char TOKEN[] = "cml9kd3458msb3kelae9";
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;
constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 256U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 256U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

// Cấu hình OTA
constexpr char OTA_VERSION_URL[] = "http://192.168.1.2:8080/version.json"; 
constexpr char OTA_FIRMWARE_URL[] = "http://192.168.1.2:8080/firmware.bin"; 
constexpr char FIRMWARE_VERSION[] = "1.0.0";
constexpr uint32_t OTA_CHECK_INTERVAL = 60000U; // 1 min

// Thời gian gửi dữ liệu telemetry
constexpr int16_t telemetrySendInterval = 10000U;
uint32_t previousDataSend;

// Khởi tạo MQTT client
WiFiClient espClient;
Arduino_MQTT_Client mqttClient(espClient);
Server_Side_RPC<3U, 5U> rpc;
const std::array<IAPI_Implementation *, 1U> apis = {&rpc};
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

// Trạng thái kết nối
bool subscribed = false;

// Khai báo hàm
void InitWiFi();
bool reconnect();
void wifiConnectTask(void *pvParameters);
void coreIotConnectTask(void *pvParameters);
void sendTelemetryTask(void *pvParameters);
void sendAttributesTask(void *pvParameters);
void tbLoopTask(void *pvParameters);
void otaUpdateTask(void *pvParameters);
void performOTAUpdate();

void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  delay(1000);
  InitWiFi();
  Wire.begin(GPIO_NUM_11, GPIO_NUM_12);
  dht20.begin();

  // Tạo các task FreeRTOS
  xTaskCreate(wifiConnectTask, "wifiConnectTask", 4096, NULL, 1, &TaskHandle);
  xTaskCreate(coreIotConnectTask, "coreIotConnectTask", 8192, NULL, 1, &TaskHandle);
  xTaskCreate(sendTelemetryTask, "sendTelemetryTask", 4096, NULL, 1, &TaskHandle);
  xTaskCreate(sendAttributesTask, "sendAttributesTask", 4096, NULL, 1, &TaskHandle);
  xTaskCreate(tbLoopTask, "tbLoopTask", 4096, NULL, 1, &TaskHandle);
  xTaskCreate(otaUpdateTask, "otaUpdateTask", 8192, NULL, 1, &TaskHandle);
}

void loop() {}

void InitWiFi() {
  Serial.println("Đang kết nối tới WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Đã kết nối WiFi thành công!");
}

bool reconnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  InitWiFi();
  return true;
}

void wifiConnectTask(void *pvParameters) {
  while (1) {
    if (!reconnect()) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void coreIotConnectTask(void *pvParameters) {
  while (1) {
    if (!tb.connected()) {
      Serial.printf("Đang kết nối tới máy chủ: (%s) với token: (%s)\n", THINGSBOARD_SERVER, TOKEN);
      if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
        Serial.println("Không thể kết nối tới ThingsBoard.");
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void sendTelemetryTask(void *pvParameters) {
  while (1) {
    previousDataSend = millis();
    dht20.read();

    float temperature = dht20.getTemperature();
    float humidity = dht20.getHumidity();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("Không thể đọc dữ liệu từ cảm biến DHT20!");
    } else {
      Serial.printf("Nhiệt độ: %.2f °C, Độ ẩm: %.2f %%\n", temperature, humidity);
      tb.sendTelemetryData("temperature", temperature);
      tb.sendTelemetryData("humidity", humidity);
      tb.sendAttributeData("firmware_version", FIRMWARE_VERSION);
    }
    vTaskDelay(pdMS_TO_TICKS(telemetrySendInterval));
  }
}

void sendAttributesTask(void *pvParameters) {
  while (1) {
    tb.sendAttributeData("rssi", WiFi.RSSI());
    tb.sendAttributeData("channel", WiFi.channel());
    tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
    tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
    tb.sendAttributeData("ssid", WiFi.SSID().c_str());
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void tbLoopTask(void *pvParameters) {
  while (1) {
    tb.loop();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void otaUpdateTask(void *pvParameters) {
  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Kiểm tra bản cập nhật OTA...");
      Serial.printf("Địa chỉ IP hiện tại: %s\n", WiFi.localIP().toString().c_str());

      HTTPClient http;
      Serial.printf("Kết nối tới: %s\n", OTA_VERSION_URL);
      http.begin(OTA_VERSION_URL);
      int httpCode = http.GET();
      Serial.printf("Mã phản hồi HTTP: %d\n", httpCode);

      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("Nhận dữ liệu phiên bản: " + payload);
        StaticJsonDocument<200> doc;
        deserializeJson(doc, payload);
        String latest_version = doc["version"];
        String firmware_url = doc["url"] | OTA_FIRMWARE_URL;

        if (latest_version != FIRMWARE_VERSION) {
          Serial.printf("Có phiên bản mới: v%s\n", latest_version.c_str());
          performOTAUpdate();
        } else {
          Serial.printf("Phiên bản hiện tại đã là mới nhất: v%s\n", FIRMWARE_VERSION);
        }
      } else {
        Serial.printf("Không kiểm tra được phiên bản OTA, mã lỗi HTTP: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("WiFi chưa kết nối, không thể kiểm tra OTA.");
    }
    vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL));
  }
}

void performOTAUpdate() {
  tb.sendAttributeData("firmware_status", "Bắt đầu cập nhật OTA");
  HTTPClient http;
  http.begin(OTA_FIRMWARE_URL);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (contentLength <= 0) {
      Serial.println("Kích thước firmware không hợp lệ!");
      tb.sendAttributeData("firmware_status", "Kích thước không hợp lệ");
      http.end();
      return;
    }

    if (Update.begin(contentLength)) {
      Serial.println("Đang tải firmware...");
      size_t written = Update.writeStream(http.getStream());

      if (written == contentLength) {
        Serial.println("Nạp firmware thành công.");
        if (Update.end(true)) {
          Serial.println("Cập nhật OTA hoàn tất. Khởi động lại thiết bị!");
          tb.sendAttributeData("firmware_status", "Cập nhật thành công");
          ESP.restart();
        } else {
          Serial.println("Lỗi khi hoàn tất cập nhật.");
          tb.sendAttributeData("firmware_status", "Lỗi hoàn tất");
        }
      } else {
        Serial.println("Lỗi khi ghi firmware.");
        tb.sendAttributeData("firmware_status", "Lỗi ghi dữ liệu");
      }
    } else {
      Serial.println("Không đủ bộ nhớ cho cập nhật OTA.");
      tb.sendAttributeData("firmware_status", "Thiếu bộ nhớ");
    }
  } else {
    Serial.printf("Không tải được firmware, mã lỗi HTTP: %d\n", httpCode);
    tb.sendAttributeData("firmware_status", "Tải firmware thất bại");
  }
  http.end();
}
