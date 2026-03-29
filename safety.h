/**
 * @file    safety.h
 * @brief   TACTICAL TIMER 2 — 安全防護層宣告
 *
 * 職責（僅此模組）：
 *  1. 錯誤碼定義（TT2Error enum）
 *  2. 硬體看門狗 wrapper（ESP32 task watchdog）
 *  3. 執行期記憶體健康檢查
 *  4. 恐慌處理（panic handler）— 記錄原因後安全重啟
 *  5. Stack 水位監控（debug build）
 *
 * 設計原則：
 *  - 不依賴任何其他 TT2 模組（除 config.h）
 *  - 所有函式可在任意 RTOS Task 內安全呼叫
 *  - 非阻塞：不做 vTaskDelay / delay
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include <Arduino.h>
#include "esp_task_wdt.h"


// ============================================================
// [S1] 錯誤碼
// ============================================================
enum class TT2Error : uint16_t {
  OK              = 0x0000,

  // IPC errors (0x01xx)
  IPC_QUEUE_FULL  = 0x0101,
  IPC_QUEUE_NULL  = 0x0102,
  IPC_MUTEX_NULL  = 0x0103,
  IPC_CREATE_FAIL = 0x0104,

  // Audio errors (0x02xx)
  AUDIO_I2S_FAIL  = 0x0201,
  AUDIO_FILE_404  = 0x0202,
  AUDIO_PLAY_FAIL = 0x0203,

  // Mic errors (0x03xx)
  MIC_INSTALL_FAIL= 0x0301,
  MIC_READ_FAIL   = 0x0302,
  MIC_TIMEOUT     = 0x0303,

  // Storage errors (0x04xx)
  SD_MOUNT_FAIL   = 0x0401,
  SD_WRITE_FAIL   = 0x0402,
  SD_READ_FAIL    = 0x0403,
  NVS_OPEN_FAIL   = 0x0404,
  NVS_WRITE_FAIL  = 0x0405,

  // Network errors (0x05xx)
  WIFI_TIMEOUT    = 0x0501,
  ESPNOW_INIT_FAIL= 0x0502,
  NTP_SYNC_FAIL   = 0x0503,

  // FSM errors (0x06xx)
  FSM_BAD_STATE   = 0x0601,
  FSM_BAD_MODE    = 0x0602,

  // Bounds errors (0x07xx)
  BOUNDS_HIT      = 0x0701,
  BOUNDS_GAME     = 0x0702,
  BOUNDS_PRESET   = 0x0703,

  // Memory errors (0x08xx)
  MEM_LOW         = 0x0801,
  STACK_OVERFLOW  = 0x0802,

  // Generic (0xFFxx)
  UNKNOWN         = 0xFF00,
};

/// 將 TT2Error 轉為短字串（用於 Serial 輸出）
const char* tt2ErrorName(TT2Error e);


// ============================================================
// [S2] 看門狗設定
// ============================================================
namespace WDT {
  constexpr uint32_t TIMEOUT_S = 10;  ///< 10 秒無餵狗 → 重啟

  /**
   * @brief 初始化 ESP32 Task Watchdog（在 setup() 呼叫）
   * @note  所有需要看門狗保護的 Task 必須用 WDT::subscribe() 註冊
   */
  void init();

  /** 訂閱目前 Task 到看門狗（在 Task 起始處呼叫） */
  void subscribe();

  /** 餵狗（在 Task 主迴圈定期呼叫，建議每 1–2 秒一次） */
  void feed();

  /** 取消訂閱（Task 正常結束前呼叫） */
  void unsubscribe();
}


// ============================================================
// [S3] 恐慌處理
// ============================================================
namespace Panic {
  /**
   * @brief 記錄錯誤原因並執行安全重啟
   *
   * 流程：
   *  1. 停止看門狗（避免 WDT 重啟干擾）
   *  2. Serial 輸出錯誤訊息
   *  3. 嘗試將錯誤碼寫入 NVS（重啟後可讀取診斷）
   *  4. delay(500) 確保 Serial flush
   *  5. ESP.restart()
   *
   * @param err   錯誤碼
   * @param msg   附加說明字串（可為 nullptr）
   * @param fatal true = 呼叫後不返回；false = 僅記錄，繼續執行
   */
  [[noreturn]] void halt(TT2Error err, const char* msg = nullptr);

  /**
   * @brief 非致命錯誤記錄（僅 Serial 輸出，不重啟）
   */
  void warn(TT2Error err, const char* msg = nullptr);
}


// ============================================================
// [S4] 記憶體健康監控
// ============================================================
namespace MemCheck {
  constexpr uint32_t FREE_HEAP_MIN = 20 * 1024;  ///< 低於此值發出警告（20 KB）

  /**
   * @brief 檢查 heap 是否健康
   * @return true = 正常，false = 低記憶體警告
   */
  bool heapOk();

  /**
   * @brief 印出目前 heap / stack 狀態（debug 用）
   * @param tag  呼叫者標識字串
   */
  void report(const char* tag);

  /**
   * @brief 檢查指定 Task 的 stack 剩餘空間
   * @param task  TaskHandle_t，nullptr = 目前 Task
   * @param minWords 低於此 word 數時警告（預設 64）
   */
  void checkStack(TaskHandle_t task = nullptr, uint32_t minWords = 64);
}
