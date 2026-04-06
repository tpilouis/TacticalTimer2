/**
 * @file    timer_core.cpp
 * @brief   TACTICAL TIMER 2 — 計時核心服務實作
 *
 * 實作遊戲狀態機（IDLE → AREYOUREADY → WAITING → BEEPING → GOING → SHOWCLEAR → STOP）、
 * 命中事件處理（雙層 mutex 保護）、音訊完成驅動、延遲計時、Par Time，
 * 以及 Web 指令路由。
 *
 * @version 2.0.0
 */

#include "timer_core.h"
#include <Arduino.h>
#include <cstring>


// ============================================================
// [T0] 模組內部狀態
// ============================================================
namespace {

  // --- 遊戲狀態（mutex 保護） ---
  GameState     s_state      = GameState::IDLE;
  uint8_t       s_gameIdx    = 0;   ///< 1-based，0 = 尚未開始
  uint8_t       s_hitIdx     = 0;   ///< 環形 buffer 寫入位置（0-based）
  uint32_t      s_totalShots = 0;   ///< 本局總命中數（超過 HIT_MAX 後繼續累加）
  uint8_t       s_maxShots   = 0;   ///< 0=無限（環形buffer）；1-50=到達後自動停
  GameRecord    s_records[Limits::GAMES_MAX]; ///< 環型 buffer，最近 GAMES_MAX 局

  unsigned long s_goingOpenAt = 0;  ///< 嗶聲後保護窗口結束時間（millis）

  // --- 最佳紀錄（僅 UI Task 讀寫，無需額外 mutex） ---
  float s_bestTotal = 0.0f;  ///< 歷史最佳總時間（秒）
  float s_bestAvg   = 0.0f;  ///< 歷史最佳平均 split（秒）

  // --- 延遲計時（僅 UI Task 讀寫） ---
  bool          s_delayActive  = false;
  unsigned long s_delayTimer   = 0;
  GameState     s_pendingState = GameState::IDLE;
  bool          s_randomDelay  = false;

  // --- Par Time（僅 UI Task 讀寫） ---
  uint32_t      s_parTimeMs     = 0;
  bool          s_parActive     = false;
  unsigned long s_parDeadline   = 0;

  // --- 音訊等待旗標（UI Task 讀寫） ---
  bool s_waitingAudio = false;

  // --- 本次延遲時長（ready → beep 的等待 ms，支援隨機）---
  uint32_t s_delayDurationMs = Timing::READY_DELAY_FIXED;

  /**
   * @brief 產生下次延遲時長
   *
   * 使用 ESP32 硬體 RNG（esp_random()）產生真隨機數，無需 seed。
   *
   * @return 固定 READY_DELAY_FIXED（關閉隨機）或 [MIN, MAX) 之間的隨機值
   */
  uint32_t nextDelay() {
    if (!s_randomDelay) return Timing::READY_DELAY_FIXED;
    uint32_t range = Timing::READY_DELAY_MAX - Timing::READY_DELAY_MIN;
    return Timing::READY_DELAY_MIN + (esp_random() % range);
  }

} // anonymous namespace


// ============================================================
// [T2-impl] 生命週期
// ============================================================

/**
 * @brief 初始化計時核心，重置所有狀態
 *
 * 清除 GameRecord 環型 buffer、重置最佳紀錄、停止所有計時器。
 * 必須在 IPC::createAll() 之後呼叫。
 */
void TimerCore::init() {
  {
    IPC::MutexGuard g(IPC::stateMutex);
    s_state      = GameState::IDLE;
    s_gameIdx    = 0;
    s_hitIdx     = 0;
    s_goingOpenAt= 0;
    memset(s_records, 0, sizeof(s_records));
  }
  s_bestTotal    = 0.0f;
  s_bestAvg      = 0.0f;
  s_delayActive  = false;
  s_randomDelay  = false;
  s_parTimeMs    = 0;
  s_parActive    = false;
  s_waitingAudio = false;
  Serial.println("[TimerCore] Initialized");
}


// ============================================================
// [T3-impl] 遊戲流程控制
// ============================================================

/**
 * @brief 開始新一局
 *
 * 分配環型 buffer 槽位，重置命中計數，
 * 切換到 AREYOUREADY 狀態（等待 READY 音效播完後進入延遲）。
 *
 * @return 本局 gameIdx（1-based）
 */
uint8_t TimerCore::startNew() {
  uint8_t newIdx;
  {
    IPC::MutexGuard g(IPC::stateMutex);
    TT2_ASSERT(g.locked());

    if (s_gameIdx >= Limits::GAMES_MAX) s_gameIdx = 0;
    memset(&s_records[s_gameIdx], 0, sizeof(GameRecord));
    s_gameIdx++;
    s_hitIdx     = 0;
    s_totalShots = 0;
    s_goingOpenAt= 0;
    s_state      = GameState::AREYOUREADY;
    newIdx       = s_gameIdx;
  }
  s_delayActive  = false;
  s_waitingAudio = true;   // 即將播放 READY 音效
  s_parActive    = false;

  Serial.printf("[TimerCore] startNew -> gameIdx=%u AREYOUREADY\n", newIdx);
  return newIdx;
}

/**
 * @brief 標記計時起點（beep 播完後呼叫）
 *
 * 設定 start_time_ms = millis()，計算保護窗口結束時間，
 * 切換狀態到 GOING，並啟動 Par Time 計時（若已設定）。
 *
 * @param guardMs  保護窗口時間（ms），預設 GOING_GUARD_MS
 */
void TimerCore::markStart(uint32_t guardMs) {
  IPC::MutexGuard g(IPC::stateMutex);
  TT2_ASSERT(g.locked());

  if (s_gameIdx == 0) return;  // 安全守衛

  unsigned long now = millis();
  s_records[s_gameIdx - 1].start_time_ms = now;
  s_goingOpenAt = now + guardMs;
  s_state       = GameState::GOING;

  // 啟動 par time（若已設定）
  if (s_parTimeMs > 0) {
    s_parActive  = true;
    s_parDeadline= now + s_parTimeMs;
  }

  Serial.printf("[TimerCore] markStart: start=%lu guard=%ums parTime=%ums\n",
                now, guardMs, s_parTimeMs);
}

/**
 * @brief 強制停止計時並進入 SHOWCLEAR 流程
 *
 * 停止 Par Time 和延遲計時，切換到 SHOWCLEAR，
 * 並啟動 AFTER_CLEAR_DELAY（讓 mode 有時間播放 Clear 音效）。
 */
void TimerCore::forceStop() {
  s_parActive   = false;
  s_waitingAudio= false;
  s_delayActive = false;

  {
    IPC::MutexGuard g(IPC::stateMutex);
    s_state        = GameState::SHOWCLEAR;
    s_pendingState = GameState::SHOWCLEAR;
  }
  // 啟動 AFTER_CLEAR_DELAY
  s_delayTimer  = millis();
  s_delayActive = true;

  Serial.println("[TimerCore] forceStop -> SHOWCLEAR");
}

/**
 * @brief 靜默中止：直接跳 IDLE，不觸發 SHOWCLEAR 或音效
 * 用於模式切換時重置狀態（例如從 FREE 返回 HOME 再重新進入）
 */
void TimerCore::abortSilent() {
  s_parActive   = false;
  s_waitingAudio= false;
  s_delayActive = false;
  {
    IPC::MutexGuard g(IPC::stateMutex);
    s_state        = GameState::IDLE;
    s_pendingState = GameState::IDLE;
  }
  // 清空 audioDoneQueue，避免舊局的 AudioDone 殘留
  // 造成新局 processAudioDone 誤觸發（產生兩次嗶聲）
  if (IPC::audioDoneQueue) {
    xQueueReset(IPC::audioDoneQueue);
  }
  // 同時清空 audioQueue（停止播放中的音效命令）
  if (IPC::audioQueue) {
    xQueueReset(IPC::audioQueue);
  }
  Serial.println("[TimerCore] abortSilent -> IDLE, queues flushed");
}

/**
 * @brief 處理 hitQueue 中的所有命中事件（在 UI Task 每幀呼叫）
 *
 * 雙層 mutex 保護：
 *  1. 快速無鎖讀取 GameState（第一層過濾）
 *  2. 持鎖確認 + 寫入 GameRecord（第二層防競態）
 *
 * 達到 HIT_MAX 時自動切換到 SHOWCLEAR。
 *
 * @param onHitCb  命中事件回呼（hitIdx, stationId, elapsed, split）
 */
void TimerCore::processHits(void (*onHitCb)(uint32_t, uint8_t,
                                            unsigned long, unsigned long)) {
  HitMsg hm;
  while (IPC::receiveHit(hm)) {

    // ── 第一層守衛（無鎖，快速篩選）──────────────────────
    GameState gs;
    uint8_t hi, gi;
    {
      IPC::MutexGuard g(IPC::stateMutex, 10);
      if (!g.locked()) continue;
      gs = s_state;
      hi = s_hitIdx;
      gi = s_gameIdx;
    }

    if (gs != GameState::GOING) {
      Serial.printf("[TimerCore] Hit DISCARD: state=%d\n", (int)gs);
      continue;
    }
    if (millis() < s_goingOpenAt) {
      Serial.println("[TimerCore] Hit DISCARD: guard window");
      continue;
    }
    if (gi == 0) {
      Serial.println("[TimerCore] Hit DISCARD: gameIdx==0");
      continue;
    }
    // 環形 buffer 模式（maxShots=0）下不限制 hi，允許超過 HIT_MAX
    if (s_maxShots > 0 && hi >= Limits::HIT_MAX) {
      Serial.printf("[TimerCore] Hit DISCARD: hitIdx=%u >= HIT_MAX\n", hi);
      continue;
    }

    // ── 第二層守衛（持鎖，雙重確認 + 寫入）──────────────
    uint32_t newHi;   // 真實第幾發（環形模式可超過 HIT_MAX）
    unsigned long elapsed, split;
    {
      IPC::MutexGuard g(IPC::stateMutex);
      TT2_ASSERT(g.locked());

      // 重新確認（持鎖後可能已變化）
      if (s_state   != GameState::GOING) continue;
      if (s_gameIdx == 0)                continue;

      GameRecord& rec = s_records[s_gameIdx - 1];
      s_totalShots++;   // 真實總發數（永遠累加）

      // ── 環形 buffer 寫入位置計算 ────────────────────────────
      // RO 模式：hits[0] = Draw Time，永遠鎖定不覆寫
      // 環形範圍：[roOffset .. HIT_MAX-1]
      const bool isRO   = (rec.mode == AppMode::RO);
      const uint8_t roOffset = isRO ? 1 : 0;  // RO 保留 hits[0]
      const uint8_t ringSize = Limits::HIT_MAX - roOffset;

      // 寫入槽位：前 ringSize 發線性寫，超過後環形覆寫
      uint8_t slot;
      if (s_totalShots <= ringSize) {
        slot = roOffset + (s_totalShots - 1);  // 線性寫
      } else {
        // 環形：從 roOffset 開始循環
        slot = roOffset + ((s_totalShots - 1) % ringSize);
      }

      auto& h      = rec.hits[slot];
      h.station_id = hm.station_id;
      h.hit_time_ms= hm.hit_time_ms;

      // hit_count = min(totalShots, ringSize)，代表陣列中有效筆數
      rec.hit_count = (s_totalShots < ringSize) ? s_totalShots : ringSize;
      // s_hitIdx 追蹤線性計數（供 calcElapsed/calcSplit 使用）
      if (s_hitIdx < Limits::HIT_MAX) s_hitIdx = slot + 1;

      newHi   = s_totalShots;   // 顯示用：真實第幾發
      elapsed = calcElapsed(rec, slot + 1);
      split   = calcSplit(rec, slot + 1);

      // ── 自動停止判斷 ─────────────────────────────────────────
      // maxShots = 0：無限，打到 HIT_MAX 仍繼續（環形覆寫）
      // maxShots > 0：到達指定發數 → SHOWCLEAR
      bool autoStop = (s_maxShots > 0 && s_totalShots >= s_maxShots);
      if (autoStop) {
        s_state        = GameState::SHOWCLEAR;
        s_pendingState = GameState::SHOWCLEAR;
        s_parActive    = false;
      }
    }

    Serial.printf("[TimerCore] Hit #%u src=%u T=%lu split=%lu\n",
                  newHi, hm.station_id, elapsed, split);

    if (onHitCb) {
      onHitCb(newHi, hm.station_id, elapsed, split);
    }

    // SHOWCLEAR 後啟動延遲
    if (getState() == GameState::SHOWCLEAR) {
      s_delayTimer  = millis();
      s_delayActive = true;
    }
  }
}

/**
 * @brief 處理音訊完成事件（從 audioDoneQueue 讀取）
 *
 * 根據目前狀態決定下一步：
 *  - AREYOUREADY：計算延遲時長並啟動延遲計時
 *  - BEEPING：呼叫 markStart()，切換 GOING
 *  - SHOWCLEAR：切換 STOP，更新最佳紀錄
 *
 * @param onReadyDoneCb  READY 音效播完 callback
 * @param onBeepDoneCb   BEEP 音效播完 callback
 * @param onClearDoneCb  CLEAR 音效播完 callback
 */
void TimerCore::processAudioDone(void (*onReadyDoneCb)(),
                                 void (*onBeepDoneCb)(),
                                 void (*onClearDoneCb)()) {
  AudioDone done;
  if (!IPC::receiveAudioDone(done)) return;

  s_waitingAudio = false;
  GameState cur  = getState();

  switch (cur) {
    case GameState::AREYOUREADY:
      // 播完 Ready 音效 → 計算本次延遲時長並啟動
      s_delayDurationMs = nextDelay();
      s_delayTimer   = millis();
      s_delayActive  = true;
      s_pendingState = GameState::BEEPING;
      Serial.printf("[TimerCore] AudioDone: READY done, delay=%ums\n",
                    s_delayDurationMs);
      if (onReadyDoneCb) onReadyDoneCb();
      break;

    case GameState::BEEPING:
      // 播完 Beep → 標記計時起點，切換 GOING
      markStart();
      Serial.println("[TimerCore] AudioDone: BEEP done -> GOING");
      if (onBeepDoneCb) onBeepDoneCb();
      break;

    case GameState::SHOWCLEAR: {
      // 播完 Clear 音效 → 計算最佳，切換 STOP
      uint8_t hi, gi;
      {
        IPC::MutexGuard g(IPC::stateMutex);
        hi = s_hitIdx;
        gi = s_gameIdx;
        s_hitIdx = 0;
        s_state  = GameState::STOP;
      }
      // 更新最佳紀錄
      if (hi > 0 && gi > 0) {
        const GameRecord& rec = s_records[gi - 1];
        unsigned long totalMs = calcTotalMs(rec);
        if (totalMs > 0) {
          float totalSec = totalMs / 1000.0f;
          float avgSec   = totalSec / hi;
          updateBest(totalSec, avgSec);
        }
      }
      Serial.println("[TimerCore] AudioDone: CLEAR done -> STOP");
      if (onClearDoneCb) onClearDoneCb();
      break;
    }

    default:
      break;
  }
}

/**
 * @brief 驅動延遲計時器（每幀呼叫）
 *
 * 到期時呼叫 setState(s_pendingState) 並觸發 onExpiredCb。
 * pendingState == BEEPING：Ready→Beep 延遲（隨機或固定）
 * pendingState == SHOWCLEAR：AFTER_CLEAR_DELAY
 *
 * @param onExpiredCb  延遲到期 callback（pending GameState 作為參數）
 */
void TimerCore::processDelay(void (*onExpiredCb)(GameState)) {
  if (!s_delayActive) return;

  // 計算本次需要等待的時長
  uint32_t needed;
  if (s_pendingState == GameState::BEEPING) {
    // Ready → Beep 之前的延遲（支援隨機）
    // 延遲時長在 startDelay 時就已計算並存在 s_delayDuration
    needed = s_delayDurationMs;
  } else {
    needed = Timing::AFTER_CLEAR_DELAY;
  }

  if (millis() - s_delayTimer < needed) return;

  s_delayActive = false;
  setState(s_pendingState);

  Serial.printf("[TimerCore] Delay expired -> state=%d\n", (int)s_pendingState);

  if (onExpiredCb) onExpiredCb(s_pendingState);
}

/**
 * @brief 驅動 Par Time 計時器（每幀呼叫，僅 FREE / DRILL 模式使用）
 *
 * 到期時停用 Par Time 並呼叫 onParCb（通常播放第二聲 beep）。
 *
 * @param onParCb  Par Time 到期 callback
 */
void TimerCore::processParTime(void (*onParCb)()) {
  if (!s_parActive || s_parTimeMs == 0) return;
  if (getState() != GameState::GOING) {
    s_parActive = false;
    return;
  }
  if (millis() >= s_parDeadline) {
    s_parActive = false;
    Serial.println("[TimerCore] Par time expired");
    if (onParCb) onParCb();
  }
}

/**
 * @brief 處理 Web 指令（從 uiCmdQueue 讀取，在 loop Task 執行）
 *
 * 路由 START / STOP / DRY_SET_BEAT / SPY_CLEAR 到對應 callback。
 * START 指令會無條件轉發（讓 mode 自行判斷是否可執行）。
 *
 * @param onStartCb    START 指令 callback
 * @param onStopCb     STOP 指令 callback
 * @param onSetBeatCb  DRY_SET_BEAT 指令 callback（帶 beatMs 參數）
 * @param onSpyClearCb SPY_CLEAR 指令 callback
 */
void TimerCore::processWebCmd(void (*onStartCb)(), void (*onStopCb)(),
                              void (*onSetBeatCb)(uint32_t),
                              void (*onSpyClearCb)(),
                              void (*onSetParCb)(uint32_t)) {
  UiCmdMsg cmd;
  while (IPC::receiveUiCmd(cmd)) {
    switch (cmd.cmd) {
      case WebCmd::START: {
        GameState s = getState();
        if (s == GameState::IDLE || s == GameState::STOP) {
          Serial.println("[TimerCore] WebCmd: START");
          if (onStartCb) onStartCb();
        } else {
          // SPY / DRY_FIRE 不依賴 GameState，也轉發
          if (onStartCb) onStartCb();
        }
        break;
      }
      case WebCmd::STOP:
        Serial.println("[TimerCore] WebCmd: STOP");
        if (onStopCb) onStopCb();
        break;
      case WebCmd::DRY_SET_BEAT:
        Serial.printf("[TimerCore] WebCmd: DRY_SET_BEAT beatMs=%u\n", cmd.beatMs);
        if (onSetBeatCb) onSetBeatCb(cmd.beatMs);
        break;
      case WebCmd::SPY_CLEAR:
        Serial.println("[TimerCore] WebCmd: SPY_CLEAR");
        if (onSpyClearCb) onSpyClearCb();
        break;
      case WebCmd::SET_PAR:
        Serial.printf("[TimerCore] WebCmd: SET_PAR parMs=%u\n", cmd.parMs);
        if (onSetParCb) onSetParCb(cmd.parMs);
        break;
      default:
        break;
    }
  }
}


// ============================================================
// [T4-impl] 狀態查詢
// ============================================================

/// @brief 取得目前 GameState（mutex 保護）
GameState TimerCore::getState() {
  IPC::MutexGuard g(IPC::stateMutex);
  return g.locked() ? s_state : GameState::IDLE;
}

/// @brief 設定 GameState（mutex 保護）
void TimerCore::setState(GameState s) {
  IPC::MutexGuard g(IPC::stateMutex);
  if (g.locked()) s_state = s;
}

/// @brief 取得目前 gameIdx（1-based，0=尚未開始）
uint8_t TimerCore::getGameIdx() {
  IPC::MutexGuard g(IPC::stateMutex);
  return g.locked() ? s_gameIdx : 0;
}

/// @brief 取得目前局命中計數（mutex 保護）
uint8_t TimerCore::getHitIdx() {
  IPC::MutexGuard g(IPC::stateMutex);
  return g.locked() ? s_hitIdx : 0;
}

/// @brief 取得保護窗口結束時間（millis），MicTask 讀取不需 mutex
unsigned long TimerCore::getGoingOpenAt() {
  return s_goingOpenAt;  // 僅 MicTask 讀，UI Task 寫（不同 core，volatile 夠用）
}

/**
 * @brief 取得指定局的 GameRecord 指標（環型 buffer，mutex 保護）
 *
 * @param reverseIdx  0 = 最近一局，1 = 前一局，以此類推
 * @return GameRecord 指標，無效時回傳 nullptr
 */
const GameRecord* TimerCore::getRecord(uint8_t reverseIdx) {
  IPC::MutexGuard g(IPC::stateMutex);
  if (!g.locked() || s_gameIdx == 0) return nullptr;

  // reverseIdx=0 → 最近一局；環型 buffer 換算
  int idx = static_cast<int>(s_gameIdx) - 1 - static_cast<int>(reverseIdx);
  if (idx < 0) idx += Limits::GAMES_MAX;
  TT2_BOUNDS(static_cast<uint8_t>(idx), Limits::GAMES_MAX, return nullptr);
  return &s_records[idx];
}

/// @brief 取得歷史最佳總時間（秒），0 = 尚無紀錄
float TimerCore::getBestTotal() { return s_bestTotal; }

/// @brief 取得歷史最佳平均 split（秒），0 = 尚無紀錄
float TimerCore::getBestAvg()   { return s_bestAvg; }

/**
 * @brief 更新最佳紀錄（若新值更優則更新）
 *
 * @param totalSec  本局總時間（秒）
 * @param avgSec    本局平均 split（秒）
 * @return true  有更新（創新高）
 * @return false 未更新
 */
bool TimerCore::updateBest(float totalSec, float avgSec) {
  bool updated = false;
  if (s_bestTotal == 0.0f || totalSec < s_bestTotal) {
    s_bestTotal = totalSec;
    updated = true;
  }
  if (s_bestAvg == 0.0f || avgSec < s_bestAvg) {
    s_bestAvg = avgSec;
    updated = true;
  }
  if (updated) {
    Serial.printf("[TimerCore] New best: total=%.2fs avg=%.2fs\n",
                  s_bestTotal, s_bestAvg);
  }
  return updated;
}


// ============================================================
// [T5-impl] 延遲 / Par Time 設定
// ============================================================

/**
 * @brief 設定隨機延遲模式
 * @param enabled  true = 隨機（1–4 秒），false = 固定（2 秒）
 */
void TimerCore::setRandomDelay(bool enabled) {
  s_randomDelay = enabled;
}

/**
 * @brief 設定 Par Time
 * @param ms  Par Time（ms），0 = 停用，上限 PAR_TIME_MAX_MS
 */
void TimerCore::setParTime(uint32_t ms) {
  // 夾在合法範圍內
  if (ms > Timing::PAR_TIME_MAX_MS) ms = Timing::PAR_TIME_MAX_MS;
  s_parTimeMs = ms;
}

/// @brief 取得目前 Par Time（ms）
uint32_t TimerCore::getParTime()    { return s_parTimeMs; }

/// @brief 查詢隨機延遲是否啟用
bool     TimerCore::isRandomDelay() { return s_randomDelay; }

/// @brief 設定 Free/RO 最大發數（0=無限環形，1-HIT_MAX=到達自動停）
void TimerCore::setMaxShots(uint8_t n) {
  s_maxShots = (n > Limits::HIT_MAX) ? Limits::HIT_MAX : n;
}
uint8_t  TimerCore::getMaxShots()   { return s_maxShots; }
uint32_t TimerCore::getTotalShots() { return s_totalShots; }


// ============================================================
// [T6-impl] 計算輔助
// ============================================================

/**
 * @brief 計算指定命中的累積時間（elapsed）
 *
 * @param rec          GameRecord
 * @param hitIdx1based 1-based 命中索引
 * @return elapsed（ms），無效時回傳 0
 */
unsigned long TimerCore::calcElapsed(const GameRecord& rec, uint8_t hitIdx1based) {
  if (hitIdx1based == 0 || hitIdx1based > Limits::HIT_MAX) return 0;
  if (rec.start_time_ms == 0) return 0;
  return rec.hits[hitIdx1based - 1].hit_time_ms - rec.start_time_ms;
}

/**
 * @brief 計算指定命中的分段時間（split）
 *
 * 第一槍 split = elapsed（反應時間）；後續 = 與前一槍的間隔。
 *
 * @param rec          GameRecord
 * @param hitIdx1based 1-based 命中索引
 * @return split（ms），無效時回傳 0
 */
unsigned long TimerCore::calcSplit(const GameRecord& rec, uint8_t hitIdx1based) {
  if (hitIdx1based == 0 || hitIdx1based > Limits::HIT_MAX) return 0;
  if (hitIdx1based == 1) {
    return calcElapsed(rec, 1);
  }
  return rec.hits[hitIdx1based - 1].hit_time_ms
       - rec.hits[hitIdx1based - 2].hit_time_ms;
}

/**
 * @brief 計算本局總時間（第一槍到最後一槍）
 *
 * @param rec  GameRecord
 * @return 總時間（ms），hit_count==0 或 start_time_ms==0 時回傳 0
 */
unsigned long TimerCore::calcTotalMs(const GameRecord& rec) {
  if (rec.hit_count == 0 || rec.start_time_ms == 0) return 0;
  uint8_t last = rec.hit_count;
  TT2_BOUNDS(last, Limits::HIT_MAX + 1, return 0);
  return rec.hits[last - 1].hit_time_ms - rec.start_time_ms;
}
