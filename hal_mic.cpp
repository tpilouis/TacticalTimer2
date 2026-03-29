/**
 * @file    hal_mic.cpp
 * @brief   TACTICAL TIMER 2 — SPM1423 PDM 麥克風 HAL 實作
 *
 * 封裝 ESP32 I2S PDM RX driver 的安裝、讀取與卸載，
 * 並提供 RMS 計算（Newton-Raphson 整數平方根）。
 *
 * @version 2.0.0
 */

#include "hal_mic.h"
#include <Arduino.h>


// ============================================================
// [M0] 模組內部狀態（僅此檔案可見）
// ============================================================
static bool s_installed = false;  ///< I2S PDM driver 安裝狀態


// ============================================================
// [M1-impl] Driver 生命週期
// ============================================================

/**
 * @brief 安裝並啟動 SPM1423 PDM RX driver
 *
 * 步驟：
 *  1. 無條件先 uninstall（解除 ESP_ERR_INVALID_STATE 0x103）
 *     — AudioOutputI2S 析構不呼叫 uninstall，port 可能殘留
 *  2. 配置 I2S PDM RX 參數（16kHz / 16-bit / RIGHT channel）
 *  3. 配置 GPIO pin（CLK=GPIO0, DATA=GPIO34）
 *  4. 安裝 driver
 *  5. 清除 DMA buffer（避免讀到上次播放的音訊殘影）
 *
 * @return true   driver 安裝成功
 * @return false  i2s_driver_install 失敗
 */
bool HalMic::init() {
  // ── 步驟 1：無條件先 uninstall（解除 0x103 ESP_ERR_INVALID_STATE）
  // AudioOutputI2S 的析構函式不呼叫 i2s_driver_uninstall，
  // 若上次播放後 port 仍在 registry 中登記，再次 install 就會失敗。
  i2s_driver_uninstall((i2s_port_t)HW::MIC_PORT);
  s_installed = false;

  // ── 步驟 2：配置 PDM RX
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate          = HW::MIC_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = TT2_MIC_CH_FMT;    // RIGHT: LR=GND on M5Core2
  cfg.communication_format = TT2_MIC_COMM_FMT;  // IDF 4.x: I2S_COMM_FORMAT_I2S
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 4;
  cfg.dma_buf_len          = HW::MIC_FRAME;
  cfg.use_apll             = false;

  // ── 步驟 3：Pin 配置
  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_PIN_NO_CHANGE;   // PDM 無 BCK
  pins.ws_io_num    = HW::MIC_CLK_PIN;     // GPIO 0
  pins.data_out_num = I2S_PIN_NO_CHANGE;   // RX only
  pins.data_in_num  = HW::MIC_DATA_PIN;    // GPIO 34

  // ── 步驟 4：Install driver
  esp_err_t err = i2s_driver_install(
      (i2s_port_t)HW::MIC_PORT, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[HalMic] Install failed: 0x%x\n", err);
    return false;
  }

  i2s_set_pin((i2s_port_t)HW::MIC_PORT, &pins);

  // ── 步驟 5：清除 DMA 殘留（避免讀到上次播放的音訊殘影）
  i2s_zero_dma_buffer((i2s_port_t)HW::MIC_PORT);

  s_installed = true;
  Serial.println("[HalMic] PDM driver installed");
  return true;
}

/**
 * @brief 卸載 PDM driver，釋放 I2S port
 *
 * 供 AudioTask 在播放前呼叫，與 init() 配對使用。
 * 卸載後 isInstalled() 回傳 false。
 */
void HalMic::deinit() {
  i2s_driver_uninstall((i2s_port_t)HW::MIC_PORT);
  s_installed = false;
  Serial.println("[HalMic] PDM driver uninstalled");
}

/**
 * @brief 從 PDM driver 讀取一幀 PCM 樣本
 *
 * 使用 i2s_read() 阻塞讀取，最多等待 timeoutMs。
 * 若 driver 未安裝或讀取失敗，回傳 false 並設 outSamples=0。
 *
 * @param buf         輸出 PCM 緩衝區（int16_t 陣列）
 * @param bufSamples  緩衝區容量（樣本數）
 * @param[out] outSamples 實際讀取的樣本數
 * @param timeoutMs   等待超時（ms）
 * @return true   成功讀取 > 0 個樣本
 * @return false  driver 未安裝、讀取失敗或逾時
 */
bool HalMic::read(int16_t* buf, size_t bufSamples,
                  size_t& outSamples, uint32_t timeoutMs) {
  TT2_NOT_NULL(buf, return false);

  if (!s_installed) {
    outSamples = 0;
    return false;
  }

  size_t bytesRead = 0;
  esp_err_t err = i2s_read(
      (i2s_port_t)HW::MIC_PORT,
      buf,
      bufSamples * sizeof(int16_t),
      &bytesRead,
      pdMS_TO_TICKS(timeoutMs));

  if (err != ESP_OK) {
    // i2s_read 失敗通常代表 driver 被意外卸載
    Serial.printf("[HalMic] i2s_read error: 0x%x\n", err);
    s_installed = false;  // 標記需要重新 init
    outSamples = 0;
    return false;
  }

  outSamples = bytesRead / sizeof(int16_t);
  return outSamples > 0;
}

/**
 * @brief 查詢 PDM driver 是否已安裝
 * @return true  driver 已安裝，可呼叫 read()
 * @return false driver 未安裝或已被 deinit()
 */
bool HalMic::isInstalled() {
  return s_installed;
}


// ============================================================
// [M2-impl] 訊號處理
// ============================================================

/**
 * @brief 計算 PCM 緩衝區的 RMS 值（均方根）
 *
 * 演算法：
 *  1. 以 int64 累加各樣本平方和，避免 int32 溢位
 *  2. 除以樣本數得均方值
 *  3. 以 Newton-Raphson 整數平方根求 RMS
 *
 * @param buf  PCM 樣本陣列（int16_t）
 * @param n    樣本數
 * @return     RMS 值（0 = 靜音或輸入無效）
 *
 * @note 回傳值範圍 0–32767（對應 16-bit full scale）
 */
uint32_t HalMic::calcRMS(const int16_t* buf, size_t n) {
  if (!buf || n == 0) return 0;

  // 累加平方和（使用 int64 避免溢位）
  int64_t sumSq = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = static_cast<int32_t>(buf[i]);
    sumSq += s * s;
  }

  // 均方值
  uint32_t mean = static_cast<uint32_t>(sumSq / static_cast<int64_t>(n));
  if (mean == 0) return 0;

  // Newton-Raphson 整數平方根
  // 收斂條件：y >= x（單調遞減直到收斂）
  uint32_t x = mean;
  uint32_t y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + mean / x) / 2;
  }
  return x;
}
