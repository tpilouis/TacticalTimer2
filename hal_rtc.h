/**
 * @file    hal_rtc.h
 * @brief   TACTICAL TIMER 2 — BM8563 RTC + NTP 時間同步 HAL 宣告
 *
 * 職責：
 *  - 封裝 M5Core2 BM8563 RTC 的讀寫（使用 M5Core2.h 正確 API）
 *  - WiFi 連線後執行 NTP 同步並回寫 RTC
 *  - 提供格式化的日期時間字串（供 UI 顯示）
 *
 * BM8563 說明：
 *  - 由 AXP192 VRTC 腳獨立供電，主電源關閉後仍持續計時
 *  - 使用 M5Core2.h API（非 M5Unified）：
 *      讀取：M5.Rtc.GetDate(&RTC_DateTypeDef) / M5.Rtc.GetTime(&RTC_TimeTypeDef)
 *      寫入：M5.Rtc.SetDate(&RTC_DateTypeDef) / M5.Rtc.SetTime(&RTC_TimeTypeDef)
 *
 * NTP 同步流程：
 *  1. WiFi 連線後呼叫 syncNTP()
 *  2. configTzTime() 設定 POSIX 時區 + NTP server
 *  3. 等待 sntp_get_sync_status() == COMPLETED（最多 NTP_TIMEOUT_S 秒）
 *  4. 取得 localtime()，呼叫 writeToRtc() 回寫 BM8563
 *  5. 之後 readDateTime() 直接從 RTC 讀取（離線也準確）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include <time.h>


namespace HalRtc {

  // ──────────────────────────────────────────────────────────
  // [R1] 時間資料結構（平台無關，供 UI 使用）
  // ──────────────────────────────────────────────────────────

  struct DateTime {
    uint16_t year;    ///< 西元年，e.g. 2025
    uint8_t  month;   ///< 1–12
    uint8_t  day;     ///< 1–31
    uint8_t  weekday; ///< 0=Sunday … 6=Saturday
    uint8_t  hour;    ///< 0–23
    uint8_t  minute;  ///< 0–59
    uint8_t  second;  ///< 0–59

    /// @return true = 已設定有效時間（Year > 2020）
    bool isValid() const { return year > 2020; }
  };


  // ──────────────────────────────────────────────────────────
  // [R2] RTC 讀寫
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 從 BM8563 RTC 讀取目前日期時間
   *
   * @param dt  輸出參數，填入讀取結果
   * @return true  = 成功，dt.isValid() 可能仍為 false（未曾設定過）
   * @return false = RTC 讀取失敗
   */
  bool readDateTime(DateTime& dt);

  /**
   * @brief 將 DateTime 寫入 BM8563 RTC
   *
   * @param dt  要寫入的時間（必須 isValid()）
   * @return true  = 成功
   * @return false = dt 無效或寫入失敗
   */
  bool writeDateTime(const DateTime& dt);

  /**
   * @brief 從 ESP32 系統時間（已做 NTP 同步）讀取並寫入 RTC
   *
   * @return true  = 寫入成功（time_t > 1700000000）
   * @return false = 系統時間無效（NTP 未同步）
   */
  bool syncFromSystemTime();


  // ──────────────────────────────────────────────────────────
  // [R3] NTP 同步
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 啟動 NTP 同步並等待完成，成功後寫入 RTC
   *
   * 使用 NetCfg::NTP_TZ / NTP_SRV1 / NTP_SRV2。
   * 最長等待 NetCfg::NTP_TIMEOUT_S 秒。
   *
   * @note 必須在 WiFi 連線後呼叫
   * @return true  = 同步成功，RTC 已更新
   * @return false = 超時未完成（RTC 保留原有時間）
   */
  bool syncNTP();

  /**
   * @brief 查詢 NTP 是否已完成同步（不阻塞）
   * @return true = 已完成
   */
  bool isNtpSynced();


  // ──────────────────────────────────────────────────────────
  // [R4] 格式化輸出（供 UI 直接顯示）
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 格式化日期字串
   * @param dt    來源時間
   * @param buf   輸出緩衝區
   * @param len   緩衝區長度（建議 20）
   * @param valid 若 dt.isValid() == false，輸出 fallback 字串
   *
   * 輸出格式：
   *  有效："2025 . 03 . 22"
   *  無效："Syncing via NTP..."
   */
  void formatDate(const DateTime& dt, char* buf, size_t len,
                  const char* fallback = "Syncing via NTP...");

  /**
   * @brief 格式化時間字串
   * @param dt   來源時間
   * @param buf  輸出緩衝區
   * @param len  緩衝區長度（建議 12）
   *
   * 輸出格式："14:35:07"
   */
  void formatTime(const DateTime& dt, char* buf, size_t len);

} // namespace HalRtc
