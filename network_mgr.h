/**
 * @file    network_mgr.h
 * @brief   TACTICAL TIMER 2 — 網路管理服務宣告
 *
 * 職責：
 *  - WiFi 連線管理（WIFI_AP_STA 模式）
 *  - ESP-NOW 初始化與封包處理（ISR 安全）
 *  - NTP 時間同步（WiFi 連線後觸發，完成後寫入 BM8563 RTC）
 *  - NetworkTask 主函式（從 sseQueue 取資料推送 SSE）
 *
 * ESP-NOW 封包處理設計：
 *  - onDataRecv() 以 IRAM_ATTR 標記（允許從 ISR 呼叫）
 *  - DATA 封包：守衛確認（hitSource + gameState + hitIdx），
 *    再透過 IPC::sendHit() 推入 hitQueue（ISR-safe xQueueSendFromISR）
 *  - PAIRING 封包：直接在 ISR 內回應（回傳 server MAC + channel）
 *
 * 設計模式：Facade（封裝 WiFi + esp_now + NTP 三個底層 API）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"
#include "hal_rtc.h"
#include "timer_core.h"


namespace NetworkMgr {

  // ──────────────────────────────────────────────────────────
  // [N1] WiFi 初始化
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 初始化 WiFi（WIFI_AP_STA 模式）並連線
   *
   * 流程：
   *  1. WiFi.mode(WIFI_AP_STA)
   *  2. WiFi.begin()，最多等待 15 秒
   *  3. 連線成功後：HalRtc::syncNTP()
   *  4. 記錄 channel 供 ESP-NOW 使用
   *
   * @param ssid      WiFi SSID
   * @param password  WiFi Password
   * @return true  = 連線成功
   * @return false = 逾時（繼續以 ESP-NOW only 模式運作）
   */
  bool initWiFi(const char* ssid, const char* password);

  /** @return 目前 WiFi 連線狀態 */
  bool isWiFiConnected();

  /** @return true = 目前以 AP 熱點模式運作（無 Router 外出模式）*/
  bool isAPMode();

  /** @return 本機 IP 字串（AP 模式回傳 "192.168.4.1"，未連線回傳 "0.0.0.0"）*/
  String getIPString();

  /** @return WiFi RSSI（未連線時回傳 0） */
  int32_t getRSSI();

  /** @return WiFi channel（供 ESP-NOW 使用） */
  uint8_t getChannel();


  // ──────────────────────────────────────────────────────────
  // [N2] ESP-NOW 初始化
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 初始化 ESP-NOW，註冊收發 callback
   *
   * 必須在 initWiFi() 之後呼叫（需要 channel 資訊）。
   *
   * @return true = 成功
   */
  bool initEspNow();

  /**
   * @brief 設定目前接受 ESP-NOW 命中的 HitSource 篩選
   *
   * 當 hitSource 為 MIC 時，ESP-NOW DATA 封包會被靜默丟棄。
   * 當 hitSource 為 BOTH 時，兩者都接受。
   *
   * @param source  HitSource::ESP_NOW / MIC / BOTH
   */
  void setHitSource(HitSource source);


  // ──────────────────────────────────────────────────────────
  // [N3] NetworkTask 主函式
  // ──────────────────────────────────────────────────────────

  /**
   * @brief NetworkTask 入口（FreeRTOS task）
   *
   * 從 sseQueue 接收 SseMsg，序列化為 JSON，
   * 透過 ESPAsyncWebServer AsyncEventSource 推送 SSE。
   *
   * @param param  未使用（nullptr）
   */
  void networkTaskFn(void* param);

} // namespace NetworkMgr
