/**
 * @file    ui_screen.h
 * @brief   TACTICAL TIMER 2 — 螢幕顯示原語宣告
 *
 * 職責：
 *  - 封裝 M5Core2 TFT_eSPI (M5.Lcd) 的所有繪製操作
 *  - 提供各畫面區塊的標準化繪製函式（StatusBar / IPBar / BtnBar）
 *  - 提供各模式所需的 drawXxx() 函式（供 ModeBase 子類呼叫）
 *  - 開機 Splash 畫面
 *
 * 色彩常數（RGB565）：
 *  TFT_BLACK / TFT_WHITE / TFT_CYAN / TFT_GREEN / TFT_YELLOW 來自 M5Core2.h
 *  自訂色彩在此定義
 *
 * 設計原則：
 *  - 純 Display 操作，不含任何業務邏輯
 *  - 所有函式只在 UI Task（Core 1 / loop()）呼叫
 *  - Composite pattern：頂層函式組合底層 primitive
 *
 * @version 2.0.0
 */

#pragma once

#include "config.h"
#include "safety.h"
#include "hal_rtc.h"
#include "timer_core.h"
#include "session_mgr.h"
#include "network_mgr.h"


// ============================================================
// [UI-COL] 自訂色彩（RGB565）
// ============================================================
namespace UIColor {
  constexpr uint16_t CYAN_DIM   = 0x2492;   ///< 暗青色（狀態列背景）
  constexpr uint16_t BLUE_DARK  = 0x0820;   ///< IP 列背景
  constexpr uint16_t GREEN_DIM  = 0x03E0;   ///< GOING 徽章
  constexpr uint16_t AMBER      = 0xFD20;   ///< WAITING / READY
  constexpr uint16_t PURPLE_DIM = 0x8010;   ///< SHOWCLEAR
  constexpr uint16_t CARD_BG    = 0x0821;   ///< 卡片背景
  constexpr uint16_t CARD_BORD  = 0x1A72;   ///< 卡片邊框
  constexpr uint16_t TEXT_DIM   = 0x5AB0;   ///< 次要文字
  constexpr uint16_t SPLIT_FAST = 0x07E0;   ///< 快速分段（綠）
  constexpr uint16_t SPLIT_SLOW = 0x5AB0;   ///< 慢速分段（灰藍）
  constexpr uint16_t PASS_GREEN = 0x07E0;   ///< Drill PASS
  constexpr uint16_t FAIL_RED   = 0xF800;   ///< Drill FAIL
}


namespace UIScreen {

  // ──────────────────────────────────────────────────────────
  // [UI1] 系統層繪製（永遠存在的列）
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 開機 Splash 畫面（3 秒）
   *
   * 顯示程式名稱、版本、日期、作者、Claude 版本、Copyright。
   * 從 HalRtc 讀取日期（若 RTC 未設定顯示 "Syncing via NTP..."）。
   * 只在 setup() 中呼叫，可使用 delay()。
   */
  void drawSplash();

  /**
   * @brief 更新頂部 Status Bar（WiFi RSSI / 電池 / 來源 / 日期）
   *
   * 每 INFO_INTERVAL_MS 由 GameFSM::update() 呼叫。
   */
  void drawStatusBar();

  /**
   * @brief 更新 IP 位址列（按鈕列上方）
   *
   * WiFi 連線時顯示 IP，未連線顯示 "No WiFi"。
   * 由 drawStatusBar() 內部呼叫。
   */
  void drawIPBar();

  /**
   * @brief 繪製底部實體按鍵提示列（黃底黑字）
   *
   * @param labelA  BtnA（左）提示文字
   * @param labelB  BtnB（中）提示文字
   * @param labelC  BtnC（右）提示文字
   */
  void drawBtnBar(const char* labelA, const char* labelB, const char* labelC);

  /**
   * @brief 繪製左下角 [HOME] 觸控覆蓋區
   *
   * 所有模式畫面共用（Home 以外）。
   */
  void drawHomeBtnOverlay();


  // ──────────────────────────────────────────────────────────
  // [UI2] 模式共用元件
  // ──────────────────────────────────────────────────────────

  /**
   * @brief 清除遊戲區域（STATUS_BAR_H 到 IP_BAR_Y）
   */
  void clearGameArea();
  void resetShotPage();          ///< 新局開始時重設分頁狀態
  void shotPagePrev();           ///< 上一頁（BtnA）
  void shotPageNext();           ///< 下一頁（BtnC）
  bool shotPageHasPrev();        ///< 是否有上一頁
  bool shotPageHasNext();        ///< 是否有下一頁
  uint32_t getPageBase();        ///< 取得目前頁第一筆 hitIdx
  uint8_t  getRowsPerPage();     ///< 每頁筆數
  void     setTotalShotUI(uint32_t total);  ///< 手動設定總發數（DRILL/HISTORY用）
  void drawShotRowAt(uint32_t hitIdx, uint32_t pageBase, uint8_t stationId,
                     unsigned long elapsed, unsigned long split,
                     bool pass = true);  ///< 不自動推頁版本（翻頁重繪用）

  /**
   * @brief 繪製模式標題列（模式名稱 + 狀態徽章）
   *
   * @param title       模式名稱（e.g. "FREE SHOOTING"）
   * @param badge       狀態文字（e.g. "GOING"）
   * @param badgeColor  徽章顏色（RGB565）
   */
  void drawModeHeader(const char* title, const char* badge, uint16_t badgeColor);

  /**
   * @brief 繪製一行擊發時間紀錄
   *
   * 自動分配左欄（1–5）/ 右欄（6–10）。
   *
   * @param hitIdx    1-based 擊發序號
   * @param stationId 0=MIC, 1–6=ESP-NOW 站台
   * @param elapsed   累計時間（ms）
   * @param split     分段時間（ms）
   * @param pass      是否達標（Drill 模式用，Free 模式傳 true）
   */
  void drawShotRow(uint32_t hitIdx, uint8_t stationId,
                   unsigned long elapsed, unsigned long split,
                   bool pass = true);

  /**
   * @brief 繪製結果畫面底部統計列
   *
   * @param totalMs    本局總時間（ms）
   * @param hitCount   命中數
   * @param bestTotal  最佳總時間（秒），0 = 尚無紀錄
   * @param bestAvg    最佳平均分段（秒），0 = 尚無紀錄
   */
  void drawResultScreen(unsigned long totalMs, uint8_t hitCount,
                        float bestTotal, float bestAvg);


  // ──────────────────────────────────────────────────────────
  // [UI3] 模式專用畫面
  // ──────────────────────────────────────────────────────────

  /** Home 六張卡片選單 */
  void drawHomeScreen();

  /** Drill SETUP 設定畫面 */
  void drawDrillSetup(const DrillDef& def);

  /** Drill 結果畫面（含評分） */
  void drawDrillResult(const DrillResult& result);

  /** Dry Fire 狀態畫面 */
  void drawDryFireScreen(uint32_t beatMs, uint32_t beatCount,
                         unsigned long elapsedMs, bool running);

  /** Spy Mode 偵測畫面 */
  void drawSpyScreen(uint8_t shotCount, unsigned long totalMs,
                     const void* hits, bool listening);

  /** RO Mode 拔槍時間畫面 */
  void drawROScreen(unsigned long drawMs, uint8_t hitCount,
                    unsigned long totalMs);

  /** Settings 畫面（Preset / Source / Thresh / Random） */
  void drawSettingsScreen(const AppSettings& s);

  /** Par Time 設定畫面（Free 模式 inline）
   *  @param parMs  目前 par time（ms），0 = 停用
   */
  void drawParSetScreen(uint32_t parMs);

  /** History 列表畫面 */
  void drawHistoryList(const char paths[][Limits::SD_PATH_LEN],
                       uint8_t count, uint16_t total,
                       uint16_t pageStart, uint32_t freeKB);

  /** History 詳情畫面（drillDef 非 nullptr 時顯示 Pass/Fail highlight）*/
  void drawHistoryDetail(const char* path, const GameRecord& rec,
                         const DrillDef* drillDef = nullptr);

} // namespace UIScreen
