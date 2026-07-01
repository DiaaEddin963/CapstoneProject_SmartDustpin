#include <Waste_Classification_inferencing.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "edge-impulse-sdk/dsp/image/image.hpp"   

const char* ssid = "HONO";
const char* password = "qwertyuiop";

WebServer server(80);

#define API_KEY ""
#define DATABASE_URL "https://smart-dustbin-app-67e52-default-rtdb.asia-southeast1.firebasedatabase.app/"

#define LED_PIN 4
#define PIN_RECYCLABLE 12
#define PIN_GENERAL 13
#define ARDUINO_RX_PIN 14

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

static bool is_initialised = false;
uint8_t* last_jpeg_buf;
size_t last_jpeg_len = 0;
bool is_inferencing = false;
String last_result = "Ready for Scan";
bool sensor_triggered = false;

static camera_config_t camera_config = {
  .pin_pwdn = 32,
  .pin_reset = -1,
  .pin_xclk = 0,
  .pin_sscb_sda = 26,
  .pin_sscb_scl = 27,
  .pin_d7 = 35,
  .pin_d6 = 34,
  .pin_d5 = 39,
  .pin_d4 = 36,
  .pin_d3 = 21,
  .pin_d2 = 19,
  .pin_d1 = 18,
  .pin_d0 = 5,
  .pin_vsync = 25,
  .pin_href = 23,
  .pin_pclk = 22,
  .xclk_freq_hz = 20000000,
  .ledc_timer = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,
  .pixel_format = PIXFORMAT_JPEG,
  .frame_size = FRAMESIZE_QVGA,
  .jpeg_quality = 10,
  .fb_count = 2,
  .fb_location = CAMERA_FB_IN_PSRAM,
  .grab_mode = CAMERA_GRAB_LATEST,
};

bool ei_camera_init() {
  if (is_initialised) return true;
  if (esp_camera_init(&camera_config) != ESP_OK) return false;
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  return is_initialised = true;
}

const char* html_page = R"rawliteral(
 <!DOCTYPE html>
 <html>
 <head>
 <meta charset='UTF-8'>
 <title>Smart Sorting</title>
 <style>body{font-family:sans-serif;text-align:center;background:#f4f4f9;} .box{display:inline-block;margin:10px;} img{width:320px;border-radius:8px;} #stat{font-size:18px;font-weight:bold;margin:20px;padding:10px;background:#fff;border-radius:5px;}</style>
 </head>
 <body>
 <h2>Smart Sorting System</h2>
 <div id='stat'>Ready for Scan</div>
 <button onclick="fetch('/trigger')">📸 Trigger Scan</button>
 <div><div class='box'><h3>Live</h3><img id='stream' src='/cam'></div><div class='box'><h3>Result</h3><img id='cap' style='display:none;'></div></div>
 <script>
  setInterval(()=>{if(document.getElementById('stat').innerText!='Analyzing...') document.getElementById('stream').src='/cam?t='+Date.now()},200);
  setInterval(()=>{fetch('/stat_text').then(r=>r.text()).then(t=>{document.getElementById('stat').innerText=t; if(t.includes('Result')) document.getElementById('cap').src='/last_cap?t='+Date.now(), document.getElementById('cap').style.display='inline';})},1000);
 </script>
 </body>
 </html>
)rawliteral";

bool detect_motion() {
  camera_fb_t* fb1 = esp_camera_fb_get();
  if (!fb1) return false;
  int size1 = fb1->len;
  esp_camera_fb_return(fb1);

  delay(150);

  camera_fb_t* fb2 = esp_camera_fb_get();
  if (!fb2) return false;
  int size2 = fb2->len;
  esp_camera_fb_return(fb2);

  int diff = abs(size1 - size2);

  if (diff > 1500) {
    Serial.println("Motion Detected! Diff: " + String(diff));
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, ARDUINO_RX_PIN, -1);
  last_jpeg_buf = (uint8_t*)ps_malloc(256000);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(PIN_RECYCLABLE, OUTPUT);
  pinMode(PIN_GENERAL, OUTPUT);

  digitalWrite(PIN_RECYCLABLE, LOW);
  digitalWrite(PIN_GENERAL, LOW);

  ei_camera_init();

  int wifi_timeout = 0;
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_timeout++;

    if (wifi_timeout >= 20) {
      Serial.println("\nWi-Fi Connection Failed!");
      Serial.println("Restarting system in 3 seconds...");
      delay(3000);
      ESP.restart();
    }
  }

  Serial.println("\n\n========================");
  Serial.println("System Works Successfully!");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase Auth Successful");
    String camUrl = "http://" + WiFi.localIP().toString() + "/last_cap";

    if (Firebase.RTDB.setString(&fbdo, "/smartDustbin/cameraUrl", camUrl)) {
      Serial.println("Camera URL updated in Firebase!");
    } else {
      Serial.println("Failed to update Camera URL: " + fbdo.errorReason());
    }
  } else {
    Serial.printf("Firebase Auth Failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  fbdo.setBSSLBufferSize(2048, 512); 

  Serial.println("Waiting for Firebase to be ready...");
  delay(3000); 

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("========================\n");

  if (Firebase.ready()) {
    String camUrl = "http://" + WiFi.localIP().toString() + "/last_cap";
    if (Firebase.RTDB.setString(&fbdo, "/smartDustbin/cameraUrl", camUrl)) {
      Serial.println("Camera URL updated in Firebase successfully!");
    } else {
      Serial.println("Failed to update Camera URL: " + fbdo.errorReason());
    }
  } else {
    Serial.println("Firebase is not ready yet! Token generation might still be in progress.");
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("========================\n");

  server.on("/", [] {
    server.send(200, "text/html", html_page);
  });
  server.on("/trigger", [] {
    sensor_triggered = true;
    server.send(200, "text/plain", "OK");
  });
  server.on("/stat_text", [] {
    server.send(200, "text/plain", last_result);
  });
  server.on("/cam", [] {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      server.setContentLength(fb->len);
      server.send(200, "image/jpeg", "");
      server.client().write(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
  });
  server.on("/last_cap", [] {
    if (last_jpeg_len > 0) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.setContentLength(last_jpeg_len);
      server.send(200, "image/jpeg", "");
      server.client().write(last_jpeg_buf, last_jpeg_len);
    }
  });
  server.on("/api/result", [] {
    String json = "{\"result\":\"" + last_result + "\"}";
    server.send(200, "application/json", json);
  });

  server.begin();
}

void loop() {

  if (Serial1.available()) {
  String data = Serial1.readStringUntil('\n');
  data.trim();
  
  int firstComma = data.indexOf(',');
  int secondComma = data.indexOf(',', firstComma + 1);
  
  if (firstComma > 0 && secondComma > 0) {
    int recycle_pct = data.substring(0, firstComma).toInt();
    int general_pct = data.substring(firstComma + 1, secondComma).toInt();
    int gas_val = data.substring(secondComma + 1).toInt();
    
    String gasLevel = (gas_val > 400) ? "Danger" : "Safe";
    String smellLevel = (gas_val > 400) ? "Bad" : "Good";

    Firebase.RTDB.setInt(&fbdo, "/smartDustbin/recyclableWaste", recycle_pct);
    Firebase.RTDB.setInt(&fbdo, "/smartDustbin/generalWaste", general_pct);
    Firebase.RTDB.setString(&fbdo, "/smartDustbin/gasLevel", gasLevel);
    Firebase.RTDB.setString(&fbdo, "/smartDustbin/smellLevel", smellLevel);
  }
}
  server.handleClient();

  if (!is_inferencing && detect_motion()) {
    sensor_triggered = true;
  }

  if (sensor_triggered && !is_inferencing) {
    is_inferencing = true;
    sensor_triggered = false;
    last_result = "Analyzing...";

    digitalWrite(LED_PIN, HIGH);
    delay(1000);

    camera_fb_t* dummy = esp_camera_fb_get();
    if (dummy) esp_camera_fb_return(dummy);
    dummy = esp_camera_fb_get();
    if (dummy) esp_camera_fb_return(dummy);

    camera_fb_t* fb = esp_camera_fb_get();
    digitalWrite(LED_PIN, LOW);

    if (fb) {
      memcpy(last_jpeg_buf, fb->buf, fb->len);
      last_jpeg_len = fb->len;

      String res = "Error";
      
      signal_t signal;
      
      ei_impulse_result_t result = { 0 };
      EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
      
      if (err == EI_IMPULSE_OK) {
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
          if (result.classification[ix].value > highest_confidence) {
            highest_confidence = result.classification[ix].value;
            res = String(result.classification[ix].label);
          }
        }
      }
      
      res.toUpperCase();
      float display_confidence = highest_confidence * 100.0;

      digitalWrite(PIN_RECYCLABLE, LOW);
      digitalWrite(PIN_GENERAL, LOW);

      if (res.indexOf("ALUMINIUM") != -1) {
        last_result = "Result: Recyclable Aluminium can (" + String(display_confidence, 1) + "%)";
        digitalWrite(PIN_RECYCLABLE, HIGH);
        delay(500);
        digitalWrite(PIN_RECYCLABLE, LOW);
        delay(4000);
      } else if (res.indexOf("GLASS") != -1) {
        last_result = "Result: Recyclable Glass Bottle (" + String(display_confidence, 1) + "%)";
        digitalWrite(PIN_RECYCLABLE, HIGH);
        delay(500);
        digitalWrite(PIN_RECYCLABLE, LOW);
        delay(4000);
      } else if (res.indexOf("PLASTIC") != -1) {
        last_result = "Result: Recyclable Plastic Bottle (" + String(display_confidence, 1) + "%)";
        digitalWrite(PIN_RECYCLABLE, HIGH);
        delay(500);
        digitalWrite(PIN_RECYCLABLE, LOW);
        delay(4000);
      } else if (res.indexOf("MILK") != -1) {
        last_result = "Result: Recyclable Milk container (" + String(display_confidence, 1) + "%)";
        digitalWrite(PIN_RECYCLABLE, HIGH);
        delay(500);
        digitalWrite(PIN_RECYCLABLE, LOW);
        delay(4000);
      } else if (res.indexOf("EMPTY") != -1) {
        last_result = "Result: Empty background";
      } else {
        last_result = "Result: General";
        digitalWrite(PIN_GENERAL, HIGH);
        delay(500);
        digitalWrite(PIN_GENERAL, LOW);
        delay(4000);
      }

      Serial.println("\n=============================");
      Serial.println(last_result);
      Serial.println("=============================\n");

      if (Firebase.RTDB.setString(&fbdo, "/smartDustbin/CurrentResult", last_result)) {
        Serial.println("Successfully pushed to Firebase");
      } else {
        Serial.println("Firebase Push Failed: " + fbdo.errorReason());
      }
      esp_camera_fb_return(fb);
    }
    is_inferencing = false;
  }
}
