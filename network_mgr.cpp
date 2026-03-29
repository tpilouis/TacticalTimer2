/**
 * @file    network_mgr.cpp
 * @brief   TACTICAL TIMER 2 — 網路管理服務實作
 *
 * 實作 WiFi 連線、ESP-NOW 初始化、命中事件接收，
 * 以及 NetworkTask 主迴圈（SSE 推播轉發）。
 *
 * @version 2.0.0
 */

#include "network_mgr.h"
#include "web_server.h"     // ← webServerSendHit() 宣告
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>


// ============================================================
// [N0] 模組內部狀態
// ============================================================
static uint8_t   s_channel  = 1;           ///< 目前 WiFi channel（供 ESP-NOW 使用）
static HitSource s_hitSrc   = HitSource::MIC; ///< 允許的命中來源
static bool      s_apMode   = false;       ///< true = 以 AP 熱點模式運作

/// ESP-NOW peer 快取（最多 STATION_MAX 個）
static esp_now_peer_info_t s_peers[Limits::STATION_MAX];
static uint8_t             s_peerCount = 0;


// ============================================================
// [N-internal] ESP-NOW 輔助函式
// ============================================================

/**
 * @brief 新增 ESP-NOW peer（內部輔助函式）
 *
 * 若 peer 已存在則直接回傳 true，
 * 否則建立新 peer 並加入 s_peers 快取。
 *
 * @param mac  6-byte MAC 位址
 * @return true   peer 已加入或已存在
 * @return false  peer 數量已達 STATION_MAX 上限或 API 失敗
 */
static bool addPeer(const uint8_t* mac) {
  if (s_peerCount >= Limits::STATION_MAX) return false;
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = s_channel;
  info.encrypt = false;

  if (esp_now_add_peer(&info) != ESP_OK) return false;

  memcpy(&s_peers[s_peerCount], &info, sizeof(info));
  s_peerCount++;
  return true;
}

/**
 * @brief ESP-NOW 資料接收回呼（IRAM_ATTR，可在 ISR 中執行）
 *
 * 處理兩種封包類型：
 *  - PAIRING：回應配對請求並將 station 加入 peer 列表
 *  - DATA：解析命中事件，經 HitSource 守衛後送入 hitQueue
 *
 * @note 此函式在中斷上下文執行，不可使用 xQueueSend（需用 FromISR 版本）
 * @param mac   發送方 MAC 位址
 * @param data  接收到的原始資料
 * @param len   資料長度（bytes）
 */
static void IRAM_ATTR onDataRecv(const uint8_t* mac,
                                  const uint8_t* data, int len) {
  if (len < 1 || !data) return;

  uint8_t msgType = data[0];

  // ── PAIRING 請求：直接回應 ────────────────────────────────
  if (msgType == static_cast<uint8_t>(MsgType::PAIRING)) {
    if (len < (int)sizeof(MsgPairing)) return;
    MsgPairing req;
    memcpy(&req, data, sizeof(req));
    if (req.id == 0) return;  // 只處理 station (id>0) 的請求

    MsgPairing resp = {};
    resp.msgType = static_cast<uint8_t>(MsgType::PAIRING);
    resp.id      = 0;  // 0 = server
    resp.channel = s_channel;
    WiFi.softAPmacAddress(resp.macAddr);
    esp_now_send(mac, reinterpret_cast<uint8_t*>(&resp), sizeof(resp));
    addPeer(mac);
    return;
  }

  // ── DATA 封包（命中事件）────────────────────────────────
  if (msgType != static_cast<uint8_t>(MsgType::DATA)) return;
  if (len < (int)sizeof(MsgData)) return;

  // HitSource 守衛：只接受 ESP_NOW 或 BOTH
  if (s_hitSrc != HitSource::ESP_NOW && s_hitSrc != HitSource::BOTH) return;

  // 遊戲狀態守衛（無鎖讀取，ISR 中不可取 mutex）
  // TimerCore::getState() 用 mutex，ISR 不可呼叫
  // 解法：直接讀取可見的 volatile GameState
  // 為安全起見，丟入 hitQueue 後由 processHits 做二次守衛
  MsgData msg;
  memcpy(&msg, data, sizeof(msg));

  HitMsg hm;
  hm.station_id  = msg.id;
  hm.hit_time_ms = millis();

  // ISR-safe queue send
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(IPC::hitQueue, &hm, &woken);
  portYIELD_FROM_ISR(woken);
}

/**
 * @brief ESP-NOW 傳送完成回呼（診斷用）
 * @param mac     目標 MAC 位址
 * @param status  傳送結果
 */
static void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  // 僅診斷用
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[NetworkMgr] ESP-NOW send failed");
  }
}


// ============================================================
// [N1-impl] WiFi 初始化
// ============================================================

/**
 * @brief 以 STA 模式連線 WiFi，失敗時自動啟用 AP 熱點
 *
 * 流程：
 *  1. WIFI_AP_STA 模式嘗試連線家用 Router（最多 15 秒）
 *  2. 連線成功 → STA 模式，取得 IP，同步 NTP
 *  3. 連線失敗 → 啟動 AP 熱點（SSID: TacticalTimer2，IP: 192.168.4.1）
 *     手機直接連 Core2 熱點即可使用 Web Dashboard
 *
 * @param ssid      WiFi SSID
 * @param password  WiFi 密碼
 * @return true   STA 連線成功
 * @return false  STA 失敗，已切換 AP 模式
 */
bool NetworkMgr::initWiFi(const char* ssid, const char* password) {
  s_apMode = false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);

  Serial.print("[NetworkMgr] WiFi connecting");
  for (uint8_t i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // ── STA 模式成功 ─────────────────────────────────────────
    s_channel = static_cast<uint8_t>(WiFi.channel());
    Serial.printf("[NetworkMgr] WiFi OK: IP=%s RSSI=%d ch=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), s_channel);
    HalRtc::syncNTP();
    return true;
  }

  // ── STA 連線失敗 → 啟動 AP 熱點 ─────────────────────────
  Panic::warn(TT2Error::WIFI_TIMEOUT, "WiFi connect timeout, starting AP");
  s_apMode  = true;
  s_channel = 1;  // AP 使用 channel 1

  WiFi.softAP(NetCfg::AP_SSID, NetCfg::AP_PASSWORD);
  delay(100);  // softAP 需要短暫時間初始化

  Serial.printf("[NetworkMgr] AP Mode: SSID=%s IP=%s\n",
                NetCfg::AP_SSID, WiFi.softAPIP().toString().c_str());
  return false;
}

/// @brief 查詢 WiFi 連線狀態
/// @return true 已連線
bool    NetworkMgr::isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }

/// @brief 查詢是否以 AP 熱點模式運作
/// @return true = AP 模式（外出使用），false = STA 模式（連家用 Router）
bool    NetworkMgr::isAPMode()        { return s_apMode; }

/// @brief 取得 IP 位址字串
/// @return STA 模式回傳 Router 分配的 IP；AP 模式回傳 "192.168.4.1"；未連線回傳 "0.0.0.0"
String  NetworkMgr::getIPString()     {
  if (isWiFiConnected()) return WiFi.localIP().toString();
  if (s_apMode)          return WiFi.softAPIP().toString();
  return "0.0.0.0";
}

/// @brief 取得目前 WiFi RSSI（dBm），未連線回傳 0
int32_t NetworkMgr::getRSSI()         { return isWiFiConnected() ? WiFi.RSSI() : 0; }

/// @brief 取得目前 WiFi channel（供 ESP-NOW 使用）
uint8_t NetworkMgr::getChannel()      { return s_channel; }


// ============================================================
// [N2-impl] ESP-NOW 初始化
// ============================================================

/**
 * @brief 初始化 ESP-NOW 並註冊收發回呼
 *
 * 需在 initWiFi() 之後呼叫，確保 channel 已確定。
 *
 * @return true   ESP-NOW 初始化成功
 * @return false  esp_now_init() 失敗
 */
bool NetworkMgr::initEspNow() {
  if (esp_now_init() != ESP_OK) {
    Panic::warn(TT2Error::ESPNOW_INIT_FAIL, "esp_now_init failed");
    return false;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  Serial.printf("[NetworkMgr] ESP-NOW OK, channel=%u MAC=%s\n",
                s_channel, WiFi.macAddress().c_str());
  return true;
}

/**
 * @brief 設定允許的命中來源（ESP_NOW / MIC / BOTH）
 *
 * 影響 onDataRecv 是否處理 ESP-NOW 命中封包。
 * 由 PresetMgr::applySettings() 呼叫。
 *
 * @param source  HitSource 枚舉值
 */
void NetworkMgr::setHitSource(HitSource source) {
  s_hitSrc = source;
}


// ============================================================
// [N3-impl] NetworkTask 主函式
// ============================================================

/**
 * @brief NetworkTask 主迴圈（RTOS Task，執行於 Core 0）
 *
 * 持續從 sseQueue 取出 SSE 推播訊息，
 * 呼叫 webServerSendHit() 透過 AsyncWebServer 推送給 Web client。
 *
 * 若 Queue 空則 vTaskDelay(5ms) 讓出 CPU，避免空轉。
 *
 * @param param  保留（未使用），FreeRTOS Task 參數
 */
void NetworkMgr::networkTaskFn(void* /*param*/) {
  Serial.printf("[NetworkMgr] Task started on Core %d\n",
                (int)xPortGetCoreID());
  WDT::subscribe();

  for (;;) {
    WDT::feed();

    SseMsg sm;
    if (IPC::receiveSse(sm)) {
      webServerSendHit(sm);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  WDT::unsubscribe();
  vTaskDelete(nullptr);
}
