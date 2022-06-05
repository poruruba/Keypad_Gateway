#include <Arduino.h>

#if defined(ARDUINO_M5Stick_C)
#include <M5StickC.h>
#elif defined(ARDUINO_M5Stack_ATOM)
#include <M5Atom.h>
#endif

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#include "hid_l2cap.h"

#define ENABLE_IR
//#define ENABLE_HTTP

#define TARGET_BT_ADDR  { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 } // HIDデバイスのBTのMacアドレス

#define WIFI_SSID "【WiFiアクセスポイントのSSID 】"
#define WIFI_PASSWORD "【WiFIアクセスポイントのパスワード】"

#define UDP_HOST  "【Node-RED稼働ホスト名またはIPアドレス】"
#define UDP_SEND_PORT 1401

#ifdef ENABLE_IR
#include <IRsend.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>

#define UDP_RECV_PORT 1402
#if defined(ARDUINO_M5Stick_C)
#define IR_SEND_PORT 32
#define IR_RECV_PORT 33
#elif defined(ARDUINO_M5Stack_ATOM)
#define IR_SEND_PORT 26
#define IR_RECV_PORT 32
#endif

static IRsend irsend(IR_SEND_PORT);
static IRrecv irrecv(IR_RECV_PORT);
static decode_results results;
#endif

#ifdef ENABLE_HTTP
#include <HTTPClient.h>
#define BASE_URL  "【HTTP Get呼び出し先】"
#endif

#define BT_CONNECT_TIMEOUT  10000
#define JSON_CAPACITY 256

static StaticJsonDocument<JSON_CAPACITY> jsonDoc;
static uint32_t start_tim;
static WiFiUDP udp;

static uint8_t key_message[HID_L2CAP_MESSAGE_SIZE - 2] = { 0 };

static long wifi_connect(const char *ssid, const char *password);

#ifdef ENABLE_HTTP
static String doHttpGet(const char *base_url, uint8_t key, uint8_t mod);
#endif

#ifdef ENABLE_IR
static long process_udp_receive(int packetSize);
static long process_ir_receive(void);
#endif

long udp_send(JsonDocument& json)
{
  int size = measureJson(json);
  char *p_buffer = (char*)malloc(size + 1);
  int len = serializeJson(json, p_buffer, size);
  p_buffer[len] = '\0';

  udp.beginPacket(UDP_HOST, UDP_SEND_PORT);
  udp.write((uint8_t*)p_buffer, len);
  udp.endPacket();

  free(p_buffer);

  return 0;
}

static char toC(uint8_t b)
{
  if( b >= 0 && b <= 9 )
    return '0' + b;
  else if( b >= 0x0a && b <= 0x0f )
    return 'A' + (b - 0x0a);
  else
    return '0';
}

void key_callback(uint8_t *p_msg)
{
  // キーが押されたときだけUDP送信

  for( int i = 0 ; i < HID_L2CAP_MESSAGE_SIZE - 2 ; i++ ){
    uint8_t target = p_msg[2 + i];
    if( target == 0 )
      continue;

    // すでに検知済みのキーか
    int j;
    for( j = 0 ; j < sizeof(key_message) ; j++ ){
      if( target == key_message[j] ){
        break;
      }
    }
    if( j < sizeof(key_message) )
      break; // すでに検知済み

    // 検知済みバッファに加えて、UDP送信
    for( int k = 0 ; k < sizeof(key_message) ; k++ ){
      // 検知済みバッファに空きがあるか
      if( key_message[k] == 0 ){
        key_message[k] = target;

        // UDP送信
        jsonDoc.clear();
        jsonDoc["type"] = "key_press";
        jsonDoc["key"] = target;
        jsonDoc["mod"] = p_msg[0];
        
        udp_send(jsonDoc);

#ifdef ENABLE_HTTP
        // HTTP GET送信
        doHttpGet(BASE_URL, target, p_msg[0]);
#endif
        break;
      }
    }
  }

  // 離されたキーを検知済みバッファから削除
  for( int i = 0 ; i < sizeof(key_message) ; i++ ){
    int j;
    for( j = 0 ; j < HID_L2CAP_MESSAGE_SIZE - 2 ; j++ ){
      if( p_msg[2 + j] == key_message[i] )
        break;
    }
    if( j >= HID_L2CAP_MESSAGE_SIZE - 2 )
      key_message[i] = 0;
  }

/*
  // キーの押下状態が変化したときにUDP送信
  
  char message[HID_L2CAP_MESSAGE_SIZE * 2 + 1];
  for( int i = 0 ; i < HID_L2CAP_MESSAGE_SIZE ; i++ ){
    message[i * 2] = toC((p_msg[i] >> 4) & 0x0f);
    message[i * 2 + 1] = toC(p_msg[i] & 0x0f);
  }
  message[HID_L2CAP_MESSAGE_SIZE * 2] = '\0';

  jsonDoc.clear();
  jsonDoc["type"] = "key_updated";
  jsonDoc["message"] = message;
  
  udp_send(jsonDoc);
*/
}

void setup() {
  // put your setup code here, to run once:

#if defined(ARDUINO_M5Stick_C)
  M5.begin(true, true, true);
#elif defined(ARDUINO_M5Stack_ATOM)
  M5.begin(true, true, false);
#endif
  long ret;

//  delay(5000);
  Serial.println("setup start");

  wifi_connect(WIFI_SSID, WIFI_PASSWORD);

#ifdef ENABLE_IR
  irsend.begin();
  irrecv.enableIRIn();
  udp.begin(UDP_RECV_PORT);
#endif

  ret = hid_l2cap_initialize(key_callback);
  if( ret != 0 ){
    Serial.println("hid_l2cap_initialize error");
    return;
  }

  BD_ADDR addr = TARGET_BT_ADDR;

  start_tim = millis();
  ret = hid_l2cap_connect(addr);
  if( ret != 0 ){
    Serial.println("hid_l2cap_connect error");
    return;
  }

  Serial.println("setup finished");
}

void loop() {
  // put your main code here, to run repeatedly:

  M5.update();

  if( WiFi.status() != WL_CONNECTED )
    return;

#ifdef ENABLE_IR
  if (irrecv.decode(&results)) {
    process_ir_receive();
    irrecv.resume(); 
  }

  int packetSize = udp.parsePacket();
  if( packetSize > 0 ){
    process_udp_receive(packetSize);
  }
#endif

  BT_STATUS status = hid_l2cap_is_connected();
  if( status == BT_CONNECTING ){
    uint32_t tim = millis();
    if( (tim - start_tim) >= BT_CONNECT_TIMEOUT ){
      start_tim = tim;
      hid_l2cap_reconnect();
    }
  }else
  if( status == BT_DISCONNECTED ){
    start_tim = millis();
    hid_l2cap_reconnect();
  }

  delay(1);
}

static long wifi_connect(const char *ssid, const char *password)
{
  Serial.println("");
  Serial.print("WiFi Connenting");

  if( ssid == NULL && password == NULL )
    WiFi.begin();
  else
    WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    delay(500);
  }
  Serial.print("\nConnected : IP=");
  Serial.print(WiFi.localIP());
  Serial.print(" Mac=");
  Serial.println(WiFi.macAddress());

  return 0;
}

#ifdef ENABLE_HTTP
static String doHttpGet(const char *base_url, uint8_t key, uint8_t mod)
{
  char temp[4];

  String url(base_url);
  url += "?key=";
  itoa(key, temp, 10);
  url += String(temp);
  url += "&mod=";
  itoa(mod, temp, 10);
  url += String(temp);

  HTTPClient http;
  http.begin(url);
  int status_code = http.GET();
  if (status_code == 200){
    String result = http.getString();
    http.end();
    return result;
  }else{
    http.end();
    return String("");
  }
}
#endif

#ifdef ENABLE_IR
static long process_ir_receive(void)
{
  Serial.println("process_ir_receive");

  if(results.overflow){
    Serial.println("Overflow");
    return -1;
  }
  if( results.decode_type != decode_type_t::NEC || results.repeat ){
    Serial.println("not supported");
    return -1;
  }

  Serial.print(resultToHumanReadableBasic(&results));
  Serial.printf("address=%d, command=%d\n", results.address, results.command);

  jsonDoc.clear();
  jsonDoc["type"] = "ir_received";
  jsonDoc["address"] = results.address;
  jsonDoc["command"] = results.command;
  jsonDoc["value"] = results.value;

  udp_send(jsonDoc);

  return 0;
}

static long process_udp_receive(int packetSize)
{
  Serial.println("process_udp_receive");

  char *p_buffer = (char*)malloc(packetSize + 1);
  if( p_buffer == NULL )
    return -1;
  
  int len = udp.read(p_buffer, packetSize);
  if( len <= 0 ){
    free(p_buffer);
    return -1;
  }
  p_buffer[len] = '\0';

  DeserializationError err = deserializeJson(jsonDoc, p_buffer);
  if (err) {
    Serial.print(F("deserializeJson() failed with code "));
    Serial.println(err.f_str());

    free(p_buffer);
    return -1;
  }

  const char *p_type = jsonDoc["type"];
  Serial.printf("type=%s\n", p_type);
  if( strcmp(p_type, "ir_send") == 0 ){
    if( jsonDoc.containsKey("value") ){
      uint32_t value = jsonDoc["value"];
      irsend.sendNEC(value);
    }else{
      uint16_t address = jsonDoc["address"];
      uint16_t command = jsonDoc["command"];
      uint32_t value = irsend.encodeNEC(address, command);
      irsend.sendNEC(value);
    }
  }else{
    Serial.println("Not supported");
    free(p_buffer);
    return -1;
  }

  free(p_buffer);

  return 0;
}
#endif
