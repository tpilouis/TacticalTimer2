/**
 * @file    hal_mic.h
 * @brief   TACTICAL TIMER 2 — SPM1423 PDM 麥克風 HAL 宣告
 *
 * 職責：
 *  - 封裝 ESP-IDF I2S PDM driver 的 install / uninstall / read
 *  - 提供 RMS 計算（整數運算，避免 FPU 爭用）
 *  - 提供音爆偵測門檻相關計算輔助
 *
 * 硬體說明（M5Core2）：
 *  - 麥克風型號：SPM1423 (PDM 輸出)
 *  - I2S Port   : I2S_NUM_0（與 Audio 輸出共用，時分複用）
 *  - CLK Pin    : GPIO 0
 *  - DATA Pin   : GPIO 34
 *  - LR 腳接 GND → 右聲道 (I2S_CHANNEL_FMT_ONLY_RIGHT)
 *
 * 重要：
 *  - 此 HAL 不管理暫停/恢復邏輯，那屬於 MicTask (mic_engine)
 *  - init() 前會先無條件 uninstall，解決 0x103 (ESP_ERR_INVALID_STATE)
 *  - 只有 MicTask 可呼叫此模組；AudioTask 不得直接操作 PDM driver
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include <driver/i2s.h>


namespace HalMic {

  // ──────────────────────────────────────────────────────────
  // [M1] Driver 生命週期
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 安裝 SPM1423 PDM I2S driver
   *
   * 實作細節：
   *  1. 先呼叫 i2s_driver_uninstall（忽略錯誤）確保乾淨狀態
   *  2. 設定 PDM RX 模式，16kHz，16-bit，右聲道
   *  3. 設定 pin mapping (CLK=GPIO0, DATA=GPIO34)
   *  4. 呼叫 i2s_zero_dma_buffer 清除 DMA 殘留資料
   *
   * @return true  = 成功
   * @return false = i2s_driver_install 回傳錯誤
   */
  bool init();

  /**
   * @brief 卸載 PDM I2S driver，釋放 I2S_NUM_0 port
   *
   * AudioTask 播放前，MicTask 必須先呼叫此函式（由 mic_engine 協調）。
   */
  void deinit();

  /**
   * @brief 讀取一幀 PCM 樣本
   *
   * @param buf        輸出緩衝區（int16_t 陣列，長度 >= HW::MIC_FRAME）
   * @param bufSamples 緩衝區樣本數
   * @param outSamples 實際讀到的樣本數（輸出）
   * @param timeoutMs  i2s_read 等待超時（ms），建議 30
   * @return true  = 成功且讀到資料
   * @return false = 錯誤或 0 bytes
   */
  bool read(int16_t* buf, size_t bufSamples,
            size_t& outSamples, uint32_t timeoutMs = 30);


  // ──────────────────────────────────────────────────────────
  // [M2] 訊號處理
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 計算 int16 樣本陣列的 RMS 值（整數運算）
   *
   * 使用 Newton-Raphson 整數平方根，避免 FPU 在 ISR 中的爭用風險。
   * 適合在 MicTask 高頻呼叫。
   *
   * @param buf  樣本陣列
   * @param n    樣本數
   * @return     RMS 值（0–32767 的近似整數）
   */
  uint32_t calcRMS(const int16_t* buf, size_t n);

  /**
   * @brief 判斷 RMS 是否超過門檻（音爆偵測）
   *
   * @param rms       calcRMS 回傳值
   * @param threshold 設定的 RMS 門檻
   * @return true = 疑似音爆
   */
  inline bool isShot(uint32_t rms, int16_t threshold) {
    return rms >= static_cast<uint32_t>(threshold);
  }

  /**
   * @brief 取得 driver 目前的安裝狀態（供 mic_engine 查詢）
   * @return true = driver 已安裝
   */
  bool isInstalled();

} // namespace HalMic
