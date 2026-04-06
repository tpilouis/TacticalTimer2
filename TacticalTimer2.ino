/**
 * @file    TacticalTimer2.ino
 * @brief   TACTICAL TIMER 2 — 主程式入口
 *
 * 硬體：M5Stack Core2 (ESP32-D0WDQ6-V3, 雙核 240MHz)
 * IDE  ：Arduino IDE 1.8.x / ESP32 Arduino Core 2.x (IDF 4.4.x)
 *
 * 模組架構（include 順序即依賴順序）：
 *   Phase 1 Foundation    : config.h  →  safety.h  →  ipc.h
 *   Phase 2 HAL           : hal_mic   →  hal_audio  →  hal_rtc  →  hal_storage
 *   Phase 3 Services      : timer_core → mic_engine → session_mgr → preset_mgr → network_mgr
 *   Phase 4 Modes         : game_fsm  →  mode_free/drill/dryfire/spy/ro/history
 *   Phase 5 UI / Web      : ui_screen →  web_server
 *
 * RTOS 任務配置：
 *   Core 1  loopTask  prio=2  Arduino loop()（UI / GameFSM）
 *   Core 0  AudioTask prio=5  MP3 解碼 + I2S 輸出
 *   Core 0  MicTask   prio=4  PDM 採樣 + 音爆偵測
 *   Core 0  NetTask   prio=3  SSE 推播
 *
 * WiFi 憑證：建立 credentials.h 並填入 WIFI_SSID / WIFI_PASSWORD
 *
 * @version 2.1.0
 * @author  Louis Chou
 */

// ── 板級支援 ──────────────────────────────────────────────────
#include <M5Core2.h>
#include <Arduino.h>

// ── WiFi 憑證（使用者自行建立 credentials.h）────────────────
#include "credentials.h"
// credentials.h 範本：
//   #pragma once
//   #define WIFI_SSID     "your_ssid"
//   #define WIFI_PASSWORD "your_password"

// ── Phase 1 — Foundation ─────────────────────────────────────
#include "config.h"
#include "safety.h"
#include "ipc.h"

// ── Phase 2 — HAL ────────────────────────────────────────────
#include "hal_mic.h"
#include "hal_audio.h"
#include "hal_rtc.h"
#include "hal_storage.h"

// ── Phase 3 — Services ───────────────────────────────────────
#include "timer_core.h"
#include "mic_engine.h"
#include "session_mgr.h"
#include "preset_mgr.h"
#include "network_mgr.h"

// ── Phase 4 — Application Modes ──────────────────────────────
#include "game_fsm.h"
#include "mode_free.h"
#include "mode_drill.h"
#include "mode_dryfire.h"
#include "mode_spy.h"
#include "mode_ro.h"
#include "mode_history.h"

// ── Phase 5 — UI / Web ───────────────────────────────────────
#include "ui_screen.h"
#include "web_server.h"
#include <ArduinoOTA.h>


// ============================================================
// RTOS Task 函式（前向宣告，定義在下方）
// ============================================================
static void audioTask(void* param);
static void micTask(void* param);
static void netTask(void* param);

// Task handle（供診斷 / watchdog 使用）
static TaskHandle_t h_audioTask = nullptr;
static TaskHandle_t h_micTask   = nullptr;
static TaskHandle_t h_netTask   = nullptr;


// ============================================================
// setup()
// ============================================================

/**
 * @brief Arduino setup()：系統初始化（執行一次）
 *
 * 初始化順序：
 *  1. Serial / M5Core2 板級初始化
 *  2. FT6336 觸控晶片硬體重置（透過 AXP192 GPIO4 控制 RST 腳）
 *  3. Splash 畫面顯示（含 RTC 日期）
 *  4. IPC 原語建立（Queue / Mutex / EventGroup）
 *  5. TimerCore / AppSettings / PresetMgr / SessionMgr 初始化
 *  6. WiFi 連線 + NTP 時間同步 + ESP-NOW 初始化
 *  7. WebServer 啟動
 *  8. WDT（看門狗）初始化
 *  9. MicEngine 最終設定確認
 * 10. RTOS Tasks 建立（AudioTask / MicTask / NetTask）
 * 11. GameFSM 初始化（顯示 Home 畫面）
 * 12. 記憶體診斷輸出
 */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[System] %s v%s booting...\n",
                TT2Version::TT2_APP_NAME, TT2Version::TT2_APP_VER);

  // ── 板級初始化 ────────────────────────────────────────────
  M5.begin();
  M5.Axp.SetSpkEnable(true);
  M5.Lcd.setRotation(1);
  M5.Lcd.setBrightness(128);

  // ── AXP192 電源診斷（開機常駐，Serial 輸出；Web Console 在連線後自動更新）─
  {
    float batV    = M5.Axp.GetBatVoltage();
    float batI    = M5.Axp.GetBatCurrent();
    float vbusV   = M5.Axp.GetVBusVoltage();
    float vbusI   = M5.Axp.GetVBusCurrent();
    float axpTemp = M5.Axp.GetTempInAXP192();
    float apsV    = M5.Axp.GetAPSVoltage();
    bool  charging= (batI < 0);
    int   batPct  = (int)constrain((batV - 3.0f) / (4.2f - 3.0f) * 100.0f, 0.0f, 100.0f);

    Serial.println(F("[Power] ===== AXP192 Boot Diagnostics ====="));
    Serial.printf( "[Power] Battery  : %.3f V  (%d%%)\n",    batV, batPct);
    Serial.printf( "[Power] Current  : %+.1f mA  (%s)\n",   batI,
                   charging ? "Charging" : (batI > 10 ? "Discharging" : "Idle"));
    Serial.printf( "[Power] VBUS     : %.3f V / %.1f mA\n", vbusV, vbusI);
    Serial.printf( "[Power] APS      : %.3f V\n",            apsV);
    Serial.printf( "[Power] AXP Temp : %.1f C\n",            axpTemp);
    if (batV < 3.3f && !charging)
      Serial.println(F("[Power] *** WARNING: Low battery! May shutdown under load."));
    else if (batV < 3.6f && !charging)
      Serial.println(F("[Power] *** NOTICE: Battery below 50%, charge soon."));
    if (axpTemp > 60.0f)
      Serial.println(F("[Power] *** WARNING: AXP192 overheating!"));
    Serial.println(F("[Power] ========================================="));
  }

  // ★ FT6336 觸控晶片硬體重置（解決上傳後首次開機觸控鎖死問題）
  //
  // 根本原因：Arduino IDE 上傳時 ESP32 透過 USB 自動重置（EN 腳拉低再放開），
  // 但 AXP192 的 LDO2（為 LCD+FT6336 供電）不隨之斷電，
  // FT6336 I2C 狀態機殘留舊狀態，觸控完全失效。
  //
  // 修正：直接寫 AXP192 I2C 暫存器控制 GPIO4（接 FT6336 RST，active LOW）。
  // 使用原始 I2C 操作，不依賴 AXP library 版本的 GPIO API。
  // 注意：不能用 SetLDO2(false)，LDO2 同時供電給 LCD，
  //       斷電會導致 ILI9342C 控制器狀態損壞，螢幕變全黑。
  {
    // AXP192 I2C addr = 0x34
    // REG 0x95: GPIO4 control — bit[1:0]: 00=float, 01=output, 10=input
    // REG 0x96: GPIO4 output value — bit[4]: GPIO4 output level
    const uint8_t axpAddr = 0x34;  // AXP192 I2C address（不用 AXP_ADDR 避免與 AXP.h macro 衝突）

    // GPIO4 → output mode (REG 0x95, bit[1:0] = 01)
    Wire1.beginTransmission(axpAddr);
    Wire1.write(0x95); Wire1.write(0x01);
    Wire1.endTransmission();
    delay(1);

    // GPIO4 → LOW (RST assert, REG 0x96 bit4 = 0)
    Wire1.beginTransmission(axpAddr);
    Wire1.write(0x96); Wire1.write(0x00);
    Wire1.endTransmission();
    delay(10);  // reset pulse 寬度（spec: min 1ms）

    // GPIO4 → HIGH (RST release, REG 0x96 bit4 = 1)
    Wire1.beginTransmission(axpAddr);
    Wire1.write(0x96); Wire1.write(0x10);
    Wire1.endTransmission();
    delay(200); // FT6336 power-on 初始化（spec: ~200ms）
  }

  // 多次 update() 清除 FT6336 初始化期間產生的雜訊事件
  for (int i = 0; i < 5; i++) {
    M5.update();
    delay(10);
  }
  Serial.println("[Setup] FT6336 hard reset via AXP192 GPIO4 done");

  // ── Splash 畫面（含 RTC 日期，3 秒）──────────────────────
  UIScreen::drawSplash();

  // ── IPC 原語（所有 Queue / Mutex / EventGroup）───────────
  IPC::createAll();

  // ── 計時核心 ─────────────────────────────────────────────
  TimerCore::init();

  // ── NVS 設定載入 ─────────────────────────────────────────
  AppSettings settings;
  HalStorage::loadSettings(settings);

  // ── Preset 管理器（含出廠預設初始化 + 套用至 MicEngine）─
  PresetMgr::init(settings);

  // ── Session 管理器（SD 卡初始化）─────────────────────────
  SessionMgr::init();

  // ── WiFi + NTP（NTP 成功後自動回寫 BM8563 RTC）──────────
  NetworkMgr::initWiFi(WIFI_SSID, WIFI_PASSWORD);
  NetworkMgr::initEspNow();
  NetworkMgr::setHitSource(settings.hitSource);

  // ── Web 伺服器 ────────────────────────────────────────────
  WebServer::init();
  WebServer::initOTA();  // OTA 無線更新（方案 A+B）

  // ── 看門狗（10 秒超時）───────────────────────────────────
  WDT::init();

  // ── 強制確認 MicEngine 設定（Task 建立前最後一次設定）────
  // 確保 Preset NVS 亂碼不影響 MicEngine 的 source 設定
  {
    AppSettings eff = PresetMgr::getEffectiveSettings();
    // 安全守衛：若 threshold 明顯異常（>8000 表示 NVS 亂碼），回退到預設值
    int16_t  safeThresh = (eff.micThresh > 0 && eff.micThresh <= MicCfg::THRESH_MAX)
                          ? eff.micThresh : MicCfg::THRESH_DEF;
    // 安全守衛：hitSource 只能是 0/1/2，其他值視為 MIC
    HitSource safeSrc = (static_cast<uint8_t>(eff.hitSource) <= 2)
                        ? eff.hitSource : HitSource::MIC;
    MicEngine::setConfig(safeThresh, MicCfg::COOLDOWN_MS, safeSrc);
    Serial.printf("[Setup] MicEngine final: src=%d thresh=%d\n",
                  (int)safeSrc, safeThresh);
  }

  // ── RTOS Tasks ────────────────────────────────────────────
  // AudioTask: Core 0, prio 5（最高，保證音效即時）
  BaseType_t r1 = xTaskCreatePinnedToCore(
    audioTask, "AudioTask",
    TaskCfg::STACK_AUDIO, nullptr,
    TaskCfg::PRIO_AUDIO, &h_audioTask,
    TaskCfg::CORE_AUDIO
  );

  // MicTask: Core 0, prio 4（PDM 採樣不間斷）
  BaseType_t r2 = xTaskCreatePinnedToCore(
    micTask, "MicTask",
    TaskCfg::STACK_MIC, nullptr,
    TaskCfg::PRIO_MIC, &h_micTask,
    TaskCfg::CORE_MIC
  );

  // NetTask: Core 0, prio 3（SSE 推播）
  BaseType_t r3 = xTaskCreatePinnedToCore(
    netTask, "NetTask",
    TaskCfg::STACK_NET, nullptr,
    TaskCfg::PRIO_NET, &h_netTask,
    TaskCfg::CORE_NET
  );

  // ★ Task 建立結果診斷
  Serial.printf("[Setup] Tasks: Audio=%s Mic=%s Net=%s\n",
                r1 == pdPASS ? "OK" : "FAIL",
                r2 == pdPASS ? "OK" : "FAIL",
                r3 == pdPASS ? "OK" : "FAIL");
  if (r2 != pdPASS) {
    Serial.println("[Setup] FATAL: MicTask creation failed! Check stack size.");
  }

  // ── GameFSM 初始化（顯示 Home 畫面）──────────────────────
  GameFSM::init();

  // ── 記憶體診斷 ────────────────────────────────────────────
  MemCheck::report("setup_done");

  Serial.println("[System] Boot complete.");
}


// ============================================================
// loop() — UI Task（Core 1, prio 2）
// ============================================================

/**
 * @brief Arduino loop()：主迴圈（UI Task，Core 1）
 *
 * 以 100Hz（10ms delay）驅動 GameFSM::update()，負責：
 *  - 觸控事件 debounce 和派發
 *  - 目前 Mode 的 update() 呼叫
 *  - Web 指令處理
 *  - Status bar 定時更新
 *
 * 使用 vTaskDelay 而非 delay，確保 FreeRTOS 排程器正常運作。
 * 每 30 秒強制刷新 FT6336 觸控狀態，防止 I2C 狀態機長時間鎖死。
 */
void loop() {
  ArduinoOTA.handle();
  GameFSM::update();

  // ★ 10ms delay：
  // 1. 讓 FreeRTOS 排程器正常運作
  // 2. 防止 I2C bus 過載（FT6336 觸控晶片 I2C 讀取頻率限制）
  // 3. 100Hz 更新率對觸控響應已完全足夠
  vTaskDelay(pdMS_TO_TICKS(10));

  // ── 電源週期監控（每 60 秒，Serial + Web Console 雙輸出）──
  static unsigned long s_lastPowerLog = 0;
  if (millis() - s_lastPowerLog >= 60000UL) {
    s_lastPowerLog = millis();
    float batV    = M5.Axp.GetBatVoltage();
    float batI    = M5.Axp.GetBatCurrent();
    float vbusV   = M5.Axp.GetVBusVoltage();
    float axpTemp = M5.Axp.GetTempInAXP192();
    int   batPct  = (int)constrain((batV - 3.0f) / (4.2f - 3.0f) * 100.0f, 0.0f, 100.0f);
    uint32_t upSec= millis() / 1000UL;

    // Serial
    Serial.printf("[Power] %.3fV (%d%%) %+.1fmA VBUS=%.2fV AXP=%.1fC uptime=%lus\n",
                  batV, batPct, batI, vbusV, axpTemp, (unsigned long)upSec);
    if (batV < 3.3f && batI >= 0)
      Serial.println(F("[Power] *** Low battery warning!"));

    // Web Console（SSE 推播 power 狀態 + log 訊息）
    WebServer::sendPower(batV, batI, vbusV, axpTemp, upSec);
    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "%.3fV(%d%%) %+.1fmA %s",
             batV, batPct, batI,
             (batI < 0) ? "⚡Charging" :
             (batV < 3.3f) ? "⚠ LOW BATTERY" :
             (batV < 3.6f) ? "▽ Low" : "OK");
    WebServer::sendLog(logBuf);
  }

  // ★ FT6336 觸控晶片定期重置（每 30 秒）
  // 防止 FT6336 I2C 狀態機在長時間運行後鎖死
  static unsigned long s_lastTouchReset = 0;
  if (millis() - s_lastTouchReset >= 30000UL) {
    s_lastTouchReset = millis();
    M5.Touch.update();   // 強制刷新觸控狀態
  }
}


// ============================================================
// RTOS Task 實作
// ============================================================

/**
 * @brief AudioTask（Core 0, prio 5）
 *
 * 從 audioQueue 取命令：
 *  - PLAY → HalAudio::play()，然後持續 HalAudio::loop() 直到播放結束
 *  - STOP → HalAudio::stop()
 *
 * 播放自然結束 → HalAudio::onPlaybackDone() + 送 audioDoneQueue{ok=true}
 * 強制停止 → 送 audioDoneQueue{ok=false}
 *
 * @param param  保留（未使用）
 */
static void audioTask(void* /*param*/) {
  Serial.printf("[AudioTask] Core %d prio %u\n",
                (int)xPortGetCoreID(),
                (unsigned)uxTaskPriorityGet(nullptr));
  WDT::subscribe();

  for (;;) {
    WDT::feed();

    AudioCmd cmd;
    // 有指令 → 立即處理（非阻塞取出）
    if (xQueueReceive(IPC::audioQueue, &cmd, 0) == pdTRUE) {
      if (cmd.op == AudioCmd::Op::PLAY) {
        HalAudio::play(cmd.path);
      } else {
        if (HalAudio::isPlaying()) {
          HalAudio::stop();
          AudioDone done = { false };
          xQueueSend(IPC::audioDoneQueue, &done, 0);
        }
      }
    }

    // 持續 loop() 驅動解碼
    if (HalAudio::isPlaying()) {
      if (!HalAudio::loop()) {
        // 播放自然結束
        HalAudio::onPlaybackDone();
        AudioDone done = { true };
        xQueueSend(IPC::audioDoneQueue, &done, 0);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  WDT::unsubscribe();
  vTaskDelete(nullptr);
}

/**
 * @brief MicTask（Core 0, prio 4）
 *
 * 完整邏輯封裝於 MicEngine::micTaskFn()。
 * 負責 PDM 採樣、RMS 計算、音爆偵測與 I2S 時分複用協調。
 *
 * @param param  傳遞給 MicEngine::micTaskFn()（目前未使用）
 */
static void micTask(void* param) {
  MicEngine::micTaskFn(param);
}

/**
 * @brief NetTask（Core 0, prio 3）
 *
 * 完整邏輯封裝於 NetworkMgr::networkTaskFn()。
 * 負責從 sseQueue 取出 SSE 訊息並推播給 Web Dashboard。
 *
 * @param param  傳遞給 NetworkMgr::networkTaskFn()（目前未使用）
 */
static void netTask(void* param) {
  NetworkMgr::networkTaskFn(param);
}
