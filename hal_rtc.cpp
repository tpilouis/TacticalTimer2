/**
 * @file    hal_rtc.cpp
 * @brief   TACTICAL TIMER 2 — BM8563 RTC + NTP 時間同步 HAL 實作
 *
 * 實作 M5Stack Core2 內建 BM8563 RTC 的讀寫操作，
 * 以及透過 NTP 自動同步系統時間並回寫至 RTC 的機制。
 *
 * @version 2.0.0
 */

#include "hal_rtc.h"
#include <M5Core2.h>
#include <Arduino.h>

/// @cond INTERNAL
// SNTP 函式庫版本相容巨集
#if __has_include(<esp_sntp.h>)
  #include <esp_sntp.h>
  #define TT2_SNTP_OK  SNTP_SYNC_STATUS_COMPLETED
  #define TT2_SNTP_STATUS()  sntp_get_sync_status()
#elif __has_include(<sntp.h>)
  #include <sntp.h>
  #define TT2_SNTP_OK  SNTP_SYNC_STATUS_COMPLETED
  #define TT2_SNTP_STATUS()  sntp_get_sync_status()
#else
  // Fallback：只用 getLocalTime()
  #define TT2_SNTP_OK  0
  #define TT2_SNTP_STATUS()  0
#endif
/// @endcond


// ============================================================
// [R2-impl] RTC 讀寫
// ============================================================

/**
 * @brief 從 BM8563 RTC 讀取目前日期時間
 *
 * 透過 M5.Rtc.GetDate() / GetTime() 讀取硬體寄存器，
 * 填入 DateTime 結構。M5Core2 API 本身無回傳錯誤碼，
 * 假設讀取永遠成功。
 *
 * @param[out] dt  讀取結果
 * @return true（M5Core2 API 不報錯，永遠回傳 true）
 */
bool HalRtc::readDateTime(DateTime& dt) {
  RTC_DateTypeDef d;
  RTC_TimeTypeDef t;

  M5.Rtc.GetDate(&d);
  M5.Rtc.GetTime(&t);

  dt.year    = d.Year;
  dt.month   = d.Month;
  dt.day     = d.Date;
  dt.weekday = d.WeekDay;
  dt.hour    = t.Hours;
  dt.minute  = t.Minutes;
  dt.second  = t.Seconds;

  return true;  // M5Core2 API 沒有回傳錯誤碼，假設成功
}

/**
 * @brief 將 DateTime 寫入 BM8563 RTC 硬體
 *
 * 先驗證 dt.isValid()（year > 2020），拒絕無效時間寫入。
 *
 * @param dt  要寫入的日期時間
 * @return true   寫入成功
 * @return false  DateTime 無效（year <= 2020）
 */
bool HalRtc::writeDateTime(const DateTime& dt) {
  if (!dt.isValid()) {
    Serial.println("[HalRtc] writeDateTime: invalid DateTime (year <= 2020)");
    return false;
  }

  RTC_DateTypeDef d;
  d.Year    = dt.year;
  d.Month   = dt.month;
  d.Date    = dt.day;
  d.WeekDay = dt.weekday;
  M5.Rtc.SetDate(&d);

  RTC_TimeTypeDef t;
  t.Hours   = dt.hour;
  t.Minutes = dt.minute;
  t.Seconds = dt.second;
  M5.Rtc.SetTime(&t);

  Serial.printf("[HalRtc] RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
                dt.year, dt.month, dt.day,
                dt.hour, dt.minute, dt.second);
  return true;
}

/**
 * @brief 從系統時間（POSIX time_t）同步回寫 BM8563 RTC
 *
 * 驗證 time(nullptr) > 1700000000（約 2023 年），
 * 避免 NTP 尚未同步時寫入 Unix Epoch 1970。
 * 呼叫 configTzTime() 設定的時區自動轉換 UTC → 本地時間。
 *
 * @return true   同步並寫入 RTC 成功
 * @return false  系統時間尚未有效（NTP 未同步）
 */
bool HalRtc::syncFromSystemTime() {
  time_t now = time(nullptr);

  // 有效時間：Unix timestamp > 2023-01-01（防止 1970 預設值）
  if (now <= 1700000000UL) {
    Serial.println("[HalRtc] syncFromSystemTime: system time not valid yet");
    return false;
  }

  // localtime() 根據 configTzTime() 設定的 TZ 自動轉換時區
  struct tm* lt = localtime(&now);

  DateTime dt;
  dt.year    = static_cast<uint16_t>(lt->tm_year + 1900);
  dt.month   = static_cast<uint8_t>(lt->tm_mon  + 1);
  dt.day     = static_cast<uint8_t>(lt->tm_mday);
  dt.weekday = static_cast<uint8_t>(lt->tm_wday);
  dt.hour    = static_cast<uint8_t>(lt->tm_hour);
  dt.minute  = static_cast<uint8_t>(lt->tm_min);
  dt.second  = static_cast<uint8_t>(lt->tm_sec);

  return writeDateTime(dt);
}


// ============================================================
// [R3-impl] NTP 同步
// ============================================================

/**
 * @brief 透過 NTP 同步系統時間並回寫 BM8563 RTC
 *
 * 呼叫 configTzTime() 啟動 SNTP，每 500ms 輪詢同步狀態，
 * 最多等待 NetCfg::NTP_TIMEOUT_S 秒。
 * 同步成功後呼叫 syncFromSystemTime() 更新 RTC 硬體。
 *
 * @return true   NTP 同步成功且 RTC 已更新
 * @return false  同步逾時或 RTC 寫入失敗
 *
 * @note 需要 WiFi 已連線才能使用 NTP
 */
bool HalRtc::syncNTP() {
  // 設定 POSIX 時區 + NTP server（台灣 CST-8）
  configTzTime(NetCfg::NTP_TZ, NetCfg::NTP_SRV1, NetCfg::NTP_SRV2);

  Serial.print("[HalRtc] NTP syncing");

  // 等待同步完成，每 500ms 輪詢一次
  uint8_t tries = NetCfg::NTP_TIMEOUT_S * 2;  // NTP_TIMEOUT_S 秒 × 2次/秒
  bool synced = false;

  for (uint8_t i = 0; i < tries; i++) {
    delay(500);
    Serial.print('.');

#if defined(TT2_SNTP_STATUS)
    if (TT2_SNTP_STATUS() == TT2_SNTP_OK) {
      synced = true;
      break;
    }
#else
    // Fallback：嘗試 getLocalTime
    struct tm info;
    if (getLocalTime(&info, 200)) {
      synced = true;
      break;
    }
#endif
  }

  Serial.println();

  if (!synced) {
    Serial.println("[HalRtc] NTP sync timeout");
    Panic::warn(TT2Error::NTP_SYNC_FAIL, "NTP did not complete in time");
    return false;
  }

  // 同步成功：寫入 BM8563 RTC
  if (syncFromSystemTime()) {
    Serial.println("[HalRtc] NTP sync OK, RTC updated");
    return true;
  }

  return false;
}

/**
 * @brief 查詢 SNTP 是否已完成同步
 * @return true   SNTP 狀態 = COMPLETED
 * @return false  尚未同步或 API 不可用
 */
bool HalRtc::isNtpSynced() {
#if defined(TT2_SNTP_STATUS)
  return TT2_SNTP_STATUS() == TT2_SNTP_OK;
#else
  struct tm info;
  return getLocalTime(&info, 0);
#endif
}


// ============================================================
// [R4-impl] 格式化輸出
// ============================================================

/**
 * @brief 將 DateTime 格式化為日期字串
 *
 * 有效時輸出 `YYYY . MM . DD`，無效時輸出 fallback 字串。
 *
 * @param dt        來源 DateTime
 * @param[out] buf  輸出緩衝區
 * @param len       緩衝區大小
 * @param fallback  dt 無效時的替代字串（nullptr 使用預設 "---- . -- . --"）
 */
void HalRtc::formatDate(const DateTime& dt, char* buf, size_t len,
                        const char* fallback) {
  TT2_NOT_NULL(buf, return);
  if (!dt.isValid()) {
    snprintf(buf, len, "%s", fallback ? fallback : "---- . -- . --");
    return;
  }
  snprintf(buf, len, "%04u . %02u . %02u",
           dt.year, dt.month, dt.day);
}

/**
 * @brief 將 DateTime 格式化為時間字串 `HH:MM:SS`
 *
 * @param dt        來源 DateTime
 * @param[out] buf  輸出緩衝區
 * @param len       緩衝區大小
 */
void HalRtc::formatTime(const DateTime& dt, char* buf, size_t len) {
  TT2_NOT_NULL(buf, return);
  snprintf(buf, len, "%02u:%02u:%02u",
           dt.hour, dt.minute, dt.second);
}
