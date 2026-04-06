/**
 * @file    mic_engine.cpp
 * @brief   TACTICAL TIMER 2 — 麥克風音爆偵測服務實作
 *
 * 執行於獨立的 RTOS MicTask（Core 0），負責：
 *  - SPM1423 PDM 採樣（透過 HalMic）
 *  - RMS 計算與門檻比較
 *  - 與 AudioTask 的 I2S 時分複用協調（暫停握手）
 *  - 偵測到音爆時向 GameFSM 發送 HitMsg
 *  - Spy Mode：無論遊戲狀態皆偵測並回報
 *
 * @version 2.0.0
 */

#include "mic_engine.h"
#include <Arduino.h>


// ============================================================
// [ME0] 模組配置（volatile 成員，多核安全讀寫）
// ============================================================
namespace {
  /**
   * @brief 麥克風引擎執行時配置
   *
   * 使用 volatile 成員確保跨核讀寫不被編譯器最佳化掉。
   * 透過 MicEngine::setConfig() 由 loop Task (Core 1) 寫入，
   * 由 MicTask (Core 0) 讀取。
   */
  struct MicConfig {
    volatile int16_t  threshold  = MicCfg::THRESH_DEF;  ///< RMS 觸發門檻
    volatile uint32_t cooldownMs = MicCfg::COOLDOWN_MS; ///< 冷卻時間（ms）
    volatile uint8_t  source     = static_cast<uint8_t>(HitSource::MIC); ///< 命中來源
    volatile bool     spyMode    = false;                ///< Spy Mode 旗標
  };
  static MicConfig s_cfg;
}


// ============================================================
// [ME1-impl] 配置注入
// ============================================================

/**
 * @brief 更新麥克風偵測參數（執行緒安全，volatile 寫入）
 *
 * 由 PresetMgr::applySettings() 或 Web /setting 呼叫，
 * 即時更新 MicTask 讀取的配置，不需重啟 Task。
 *
 * @param threshold  RMS 觸發門檻（500–8000）
 * @param cooldownMs 擊發後冷卻時間（ms），防止同槍二次觸發
 * @param source     命中來源（ESP_NOW / MIC / BOTH）
 */
void MicEngine::setConfig(int16_t threshold, uint32_t cooldownMs, HitSource source) {
  s_cfg.threshold  = threshold;
  s_cfg.cooldownMs = cooldownMs;
  s_cfg.source     = static_cast<uint8_t>(source);
  Serial.printf("[MicEngine] Config: thresh=%d cooldown=%ums src=%d\n",
                threshold, cooldownMs, (int)source);
}

/**
 * @brief 切換 Spy Mode（不受 GameState 限制的偵測模式）
 *
 * Spy Mode 開啟時，MicTask 無論 GameState 為何皆進行偵測，
 * 適用於 Modespy 的被動監聽功能。
 *
 * @param enabled  true = 啟用，false = 停用
 */
void MicEngine::setSpyMode(bool enabled) {
  s_cfg.spyMode = enabled;
  Serial.printf("[MicEngine] SpyMode: %s\n", enabled ? "ON" : "OFF");
}

/**
 * @brief 查詢 Spy Mode 是否啟用
 * @return true  Spy Mode 啟用中
 * @return false 正常模式
 */
bool MicEngine::isSpyMode() {
  return s_cfg.spyMode;
}


// ============================================================
// [ME2-impl] MicTask 主函式
// ============================================================

/**
 * @brief MicTask 主迴圈（RTOS Task，執行於 Core 0）
 *
 * 狀態機邏輯：
 *  - [A] 模式守衛：若 source=ESP_NOW 且非 Spy Mode，跳過採樣
 *  - [B] 暫停握手：AudioTask 要求時，deinit I2S 並等待恢復
 *  - [C] Driver 確保安裝：必要時呼叫 HalMic::init()
 *  - [D] 採樣：i2s_read 讀取一幀 PCM
 *  - [E] 二次暫停檢查：採樣後再確認無暫停請求
 *  - [F] RMS 計算：HalMic::calcRMS()，每 50 幀印一次診斷
 *  - [G] 狀態守衛：非 GOING 且非 Spy Mode 則跳過
 *  - [H] 保護窗口：GOING_GUARD_MS 內忽略（避免嗶聲誤觸發）
 *  - [I] 門檻觸發 + 冷卻：超過 threshold 且冷卻結束則發送 HitMsg
 *
 * @param param  保留（未使用），FreeRTOS Task 參數
 */
void MicEngine::micTaskFn(void* /*param*/) {
  Serial.printf("[MicEngine] Task started on Core %d\n", (int)xPortGetCoreID());
  WDT::subscribe();

  int16_t       micBuf[HW::MIC_FRAME];
  bool          installed      = false;
  unsigned long cooldownUntil  = 0;
  uint32_t      frameCount     = 0;

  // ★ 啟動診斷：印出初始設定
  Serial.printf("[MicEngine] Init: src=%d thresh=%d cooldown=%u spyMode=%d\n",
                (int)s_cfg.source, (int)s_cfg.threshold,
                (unsigned)s_cfg.cooldownMs, (int)s_cfg.spyMode);

  // ★ 強制確認：若 source 不是 MIC，立即印出警告
  if (static_cast<HitSource>(s_cfg.source) == HitSource::ESP_NOW) {
    Serial.println("[MicEngine] WARNING: source=ESP_NOW, Mic will NOT sample!");
    Serial.println("[MicEngine] This is likely a configuration issue.");
  }

  for (;;) {
    WDT::feed();

    // ── [A] 模式守衛 ────────────────────────────────────────
    HitSource curSrc = static_cast<HitSource>(s_cfg.source);
    bool isMicMode = (curSrc == HitSource::MIC) || (curSrc == HitSource::BOTH);
    bool spyActive = s_cfg.spyMode;

    if (!isMicMode && !spyActive) {
      if (IPC::isMicPauseRequested()) {
        IPC::confirmMicSuspended();
        IPC::waitMicResume(5000);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // ── [B] 暫停握手（AudioTask 要求釋放 I2S）──────────────
    if (IPC::isMicPauseRequested()) {
      if (installed) {
        HalMic::deinit();
        installed = false;
        Serial.println("[MicEngine] I2S released for AudioTask");
      }
      IPC::confirmMicSuspended();
      IPC::waitMicResume(5000);
      Serial.println("[MicEngine] Pause released, reinitializing...");
      continue;
    }

    // ── [C] Driver 確保安裝 ──────────────────────────────────
    if (!installed) {
      installed = HalMic::init();
      if (!installed) {
        Serial.println("[MicEngine] Init failed, retry in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      Serial.println("[MicEngine] Driver installed, sampling...");
      frameCount = 0;
    }

    // ── [D] 採樣 ─────────────────────────────────────────────
    size_t samples = 0;
    if (!HalMic::read(micBuf, HW::MIC_FRAME, samples, 30)) {
      if (!HalMic::isInstalled()) {
        installed = false;
      }
      continue;
    }

    // ── [E] 二次暫停檢查 ─────────────────────────────────────
    if (IPC::isMicPauseRequested()) continue;

    frameCount++;

    // ── [F] RMS 計算 ─────────────────────────────────────────
    uint32_t rms = HalMic::calcRMS(micBuf, samples);

    // ★ 診斷：每 50 幀（~0.8s）印一次 — 比原本的 100 幀更頻繁，方便初期除錯
    if (frameCount % 50 == 0) {
      GameState gs = TimerCore::getState();
      Serial.printf("[MicEngine] frame=%lu gs=%d rms=%u thresh=%d src=%d spy=%d\n",
                    (unsigned long)frameCount, (int)gs, rms,
                    (int)s_cfg.threshold, (int)s_cfg.source, (int)s_cfg.spyMode);
    }

    // ── [G] 狀態守衛 ─────────────────────────────────────────
    GameState gs = TimerCore::getState();
    bool goingNow = (gs == GameState::GOING);

    if (!goingNow && !spyActive) continue;

    // ── [H] 保護窗口（Spy Mode 下跳過）──────────────────────
    if (!spyActive && millis() < TimerCore::getGoingOpenAt()) continue;

    // ── [I] 門檻觸發 + 冷卻 ─────────────────────────────────
    if (HalMic::isShot(rms, s_cfg.threshold)) {
      unsigned long now = millis();
      if (now >= cooldownUntil) {
        cooldownUntil = now + s_cfg.cooldownMs;
        HitMsg hm = { 0, now };
        if (IPC::sendHit(hm, 0)) {
          Serial.printf("[MicEngine] BANG! rms=%u thresh=%d T=%lu%s\n",
                        rms, (int)s_cfg.threshold, now,
                        spyActive ? " (SPY)" : "");
        }
      } else {
        // ★ 診斷：在冷卻期內的觸發（每局最多印 3 次）
        static uint8_t cooldownLog = 0;
        if (cooldownLog < 3) {
          Serial.printf("[MicEngine] Shot in cooldown, rms=%u\n", rms);
          cooldownLog++;
        }
        if (gs == GameState::IDLE) cooldownLog = 0;  // 重置
      }
    }
  }

  WDT::unsubscribe();
  vTaskDelete(nullptr);
}
