/**
 * @file    game_fsm.h
 * @brief   TACTICAL TIMER 2 — 主狀態機 & ModeBase 抽象基類宣告
 *
 * 架構設計（State Pattern）：
 *
 *   GameFSM
 *     │  持有  ModeBase* current
 *     │  dispatch: current->onEnter / update / onExit
 *     └──────────────────────────────────────────────
 *           ModeBase (abstract)
 *           ├── ModeFree       (Free Shooting + Par Time)
 *           ├── ModeDrill      (Drills + Scoring)
 *           ├── ModeDryFire    (Dry Fire Rhythm)
 *           ├── ModeSpy        (Silent Passive Capture)
 *           ├── ModeRO         (Range Officer + Draw Time)
 *           └── ModeHistory    (SD Card Session Browser)
 *
 * ModeBase 介面：
 *   onEnter()  — 進入模式時初始化（清畫面、重置計數器）
 *   update()   — 每 loop() 幀呼叫（處理輸入、計時、SSE）
 *   onExit()   — 離開模式時清理
 *   getMode()  — 回傳 AppMode 枚舉（用於 SessionMgr 記錄）
 *
 * 模式切換：
 *   GameFSM::switchTo(AppMode) → current->onExit → new->onEnter
 *   Home 選單也是一個模式（AppMode::HOME）
 *
 * 通知機制（observer-lite）：
 *   每個模式內部直接呼叫 WebServer 的 SSE 推播函式（前向宣告）
 *   避免引入完整 WebServer 標頭（避免循環依賴）
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "ipc.h"
#include "timer_core.h"
#include "session_mgr.h"
#include "preset_mgr.h"
#include "mic_engine.h"


// ============================================================
// [F0] WebServer 前向宣告（避免循環 include）
// ============================================================
namespace WebServer {
  void sendGameState();
  void sendNewGame();
  void sendBest(float totalSec, float avgSec);
  void sendHit(const SseMsg& sm);
}


// ============================================================
// [F1] ModeBase — 抽象基類
// ============================================================
class ModeBase {
public:
  virtual ~ModeBase() = default;

  /**
   * @brief 進入此模式（切換時呼叫一次）
   *
   * 實作：清除畫面、重置局部計數器、初始化音效序列。
   */
  virtual void onEnter() = 0;

  /**
   * @brief 每幀更新（在 loop() 中呼叫）
   *
   * 實作：處理觸控輸入、計時邏輯、SSE 推播、顯示更新。
   * 若需要切換模式，呼叫 GameFSM::requestSwitch()。
   */
  virtual void update() = 0;

  /**
   * @brief 離開此模式（切換前呼叫一次）
   *
   * 實作：停止音效、儲存 session、清理局部狀態。
   */
  virtual void onExit() = 0;

  /** @return 此模式對應的 AppMode 枚舉 */
  virtual AppMode getMode() const = 0;

  /**
   * @brief 實體按鍵事件（BtnA/B/C）
   *
   * 由 GameFSM 在 update() 之前呼叫，讓模式可以攔截按鍵。
   * 預設實作為空（不處理），子類選擇性 override。
   *
   * @param btnIdx  0=BtnA(STOP) / 1=BtnB(SETTING) / 2=BtnC(START)
   */
  virtual void onButton(uint8_t btnIdx) {}

  /**
   * @brief 觸控事件（Home 按鈕或模式內的觸控區）
   *
   * @param x  觸控 X 座標（LCD 像素）
   * @param y  觸控 Y 座標（LCD 像素）
   */
  virtual void onTouch(int16_t x, int16_t y) {}

protected:
  // ── 共用輔助：播放音效（傳入 AudioCmd 到 audioQueue）──────
  void playAudio(const char* path) {
    AudioCmd cmd;
    cmd.op = AudioCmd::Op::PLAY;
    strlcpy(cmd.path, path, sizeof(cmd.path));
    IPC::sendAudioCmd(cmd);
  }

  void stopAudio() {
    AudioCmd cmd;
    cmd.op = AudioCmd::Op::STOP;
    cmd.path[0] = '\0';
    IPC::sendAudioCmd(cmd);
  }

  // ── 共用輔助：送出命中事件到 SSE ──────────────────────────
  void pushHitSSE(uint8_t hitIdx, uint8_t stationId,
                  unsigned long elapsed, unsigned long split) {
    SseMsg sm;
    sm.hitIdx    = hitIdx;
    sm.stationId = stationId;
    sm.elapsed   = elapsed;
    sm.split     = split;
    IPC::sendSse(sm);
  }
};


// ============================================================
// [F2] GameFSM — 主狀態機
// ============================================================
namespace GameFSM {

  /**
   * @brief 初始化 FSM（setup() 中呼叫）
   *
   * 建立所有 Mode 實例，設定初始模式為 HOME。
   */
  void init();

  /**
   * @brief 主更新函式（在 Arduino loop() 中呼叫）
   *
   * 流程：
   *  1. M5.update()（實體按鍵與觸控更新）
   *  2. 讀取觸控事件 → current->onTouch()
   *  3. 讀取按鍵事件 → current->onButton()
   *  4. current->update()
   *  5. 執行模式切換（若有 requestSwitch）
   *  6. TimerCore::processWebCmd()（Web 遠端指令）
   *  7. 定時更新 Status bar
   */
  void update();

  /**
   * @brief 請求切換到指定模式（在 update() 完成後執行）
   *
   * 在 ModeBase::update() 或 onButton/onTouch 內呼叫，
   * 避免在 update() 執行期間立刻切換（reentrancy 問題）。
   *
   * @param mode  目標 AppMode
   */
  void requestSwitch(AppMode mode);

  /**
   * @brief 立即切換模式（內部使用，確保在安全時間點執行）
   */
  void switchTo(AppMode mode);

  /** @return 目前 AppMode */
  AppMode currentMode();

  /** @return 目前模式的 ModeBase 指標（只讀，測試用）*/
  const ModeBase* current();

  /**
   * @brief Home 畫面觸控路由（內部使用）
   *
   * 將觸控座標映射到 6-card grid 的 AppMode，
   * 並呼叫 switchTo()。
   */
  void handleHomeTap(int16_t x, int16_t y);

} // namespace GameFSM
