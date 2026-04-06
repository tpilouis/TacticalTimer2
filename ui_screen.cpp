/**
 * @file    ui_screen.cpp
 * @brief   TACTICAL TIMER 2 — 螢幕顯示原語實作
 *
 * 封裝所有 M5Stack Core2 LCD 繪圖操作，提供各模式畫面的繪製函式。
 * 使用 M5.Lcd（ILI9342C 320×240）和 TFT_eSPI 圖形函式庫。
 * 字體使用 FreeFont FSS9（9pt sans-serif）。
 *
 * 所有函式在 UI Task（Core 1）執行，不需要額外 mutex 保護。
 *
 * @version 2.0.0
 */

#include "ui_screen.h"
#include <M5Core2.h>
#include "preset_mgr.h"


// ============================================================
// [UI-helpers] 內部繪圖輔助函式
// ============================================================
namespace {

/**
 * @brief 繪製圓角矩形徽章（badge）
 * @param x         左上角 X
 * @param y         左上角 Y
 * @param w         寬度
 * @param h         高度
 * @param text      顯示文字
 * @param bgColor   背景顏色（RGB565）
 * @param textColor 文字顏色（RGB565）
 */
void drawBadge(int16_t x, int16_t y, int16_t w, int16_t h,
               const char* text, uint16_t bgColor, uint16_t textColor) {
  M5.Lcd.fillRoundRect(x, y, w, h, 3, bgColor);
  M5.Lcd.setTextColor(textColor, bgColor);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.drawString(text, x + w/2, y + h/2, GFXFF);
}

/**
 * @brief 格式化時間 ms → "X.XX"（截斷不四捨五入）
 * @param ms   時間（ms）
 * @param buf  輸出緩衝區
 * @param len  緩衝區長度
 */
void fmtTime(unsigned long ms, char* buf, size_t len) {
  snprintf(buf, len, "%lu.%02lu", ms / 1000, (ms % 1000) / 10);
}

} // anonymous namespace


// ============================================================
// [UI1-impl] 系統層
// ============================================================

// ── Shot log 分頁狀態（file-level，供翻頁控制）────────────────
static uint32_t s_pageBase    = 1;   ///< 目前頁第一筆 hitIdx（1-based）
static uint32_t s_totalShotUI = 0;   ///< 目前局總發數（由 drawShotRow 更新）

// ── Shot Row 佈局常數（全域共用）─────────────────────────────
// START_Y = GAME_AREA_Y(20) + MODE_HDR_H(18) + 2 = 40
// STATS_H = 50（底部統計區，drawResultScreen 使用）
// SHOT_END_Y = IP_Y(204) - STATS_H(50) - DIVIDER(2) = 152
// SHOT_AREA  = 152 - 40 = 112px
// ROW_H = 16px → 7筆/頁（112/16=7）
static constexpr int16_t SHOT_START_Y    = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2; // 40
static constexpr int16_t SHOT_STATS_H   = 50;
static constexpr int16_t SHOT_END_Y_VAL = 204 - SHOT_STATS_H - 2;  // 152
static constexpr int16_t SHOT_ROW_H     = 16;
static constexpr uint8_t ROWS_PER_PAGE_UI = (SHOT_END_Y_VAL - SHOT_START_Y) / SHOT_ROW_H; // 7

/**
 * @brief 開機 Splash 畫面（顯示約 3 秒）
 *
 * 顯示應用程式名稱、版本、作者，以及 BM8563 RTC 目前日期時間。
 * 在所有模組初始化完成前呼叫，讓使用者看到啟動進度。
 */
void UIScreen::drawSplash() {
  M5.Lcd.clear(TFT_BLACK);

  // 頂部裝飾線
  M5.Lcd.fillRect(0, 0, HW::LCD_W, 3, TFT_CYAN);

  // 程式名稱
  M5.Lcd.setFreeFont(FSB12);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.drawString("TACTICAL", HW::LCD_W / 2, 55, GFXFF);
  M5.Lcd.drawString("TIMER 2", HW::LCD_W / 2, 82, GFXFF);

  // 副標題
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString("AIR GUN SHOT TIMER SYSTEM", HW::LCD_W / 2, 108, GFXFF);

  // 分隔線
  M5.Lcd.drawFastHLine(40, 122, HW::LCD_W - 80, UIColor::CARD_BORD);

  // 日期（從 RTC 讀取）
  HalRtc::DateTime dt;
  HalRtc::readDateTime(dt);
  char dateBuf[24];
  HalRtc::formatDate(dt, dateBuf, sizeof(dateBuf), "Syncing via NTP...");
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString(dateBuf, HW::LCD_W / 2, 140, GFXFF);

  // 作者
  M5.Lcd.setTextColor(0x6B4D, TFT_BLACK);
  M5.Lcd.drawString("Louis Chou", HW::LCD_W / 2, 160, GFXFF);

  // Copyright
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.drawString("(c) 2026  All Rights Reserved", HW::LCD_W / 2, 178, GFXFF);

  // Version
  char verBuf[16];
  snprintf(verBuf, sizeof(verBuf), "v%s", TT2Version::TT2_APP_VER);
  M5.Lcd.setTextColor(UIColor::CARD_BORD, TFT_BLACK);
  M5.Lcd.drawString(verBuf, HW::LCD_W / 2, 196, GFXFF);

  // 底部裝飾線
  M5.Lcd.fillRect(0, HW::LCD_H - 3, HW::LCD_W, 3, TFT_CYAN);

  delay(3000);
}

/**
 * @brief 繪製頂部狀態列（電池電量 + WiFi RSSI + 時間）
 *
 * 每 INFO_INTERVAL_MS（2 秒）由 GameFSM::update() 定時呼叫。
 * 高度 = STATUS_H（20px），Y = 0。
 */
void UIScreen::drawStatusBar() {
  M5.Lcd.fillRect(0, 0, HW::LCD_W, Layout::STATUS_H, TFT_BLACK);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(BL_DATUM);

  // WiFi RSSI
  int32_t rssi = NetworkMgr::getRSSI();
  M5.Lcd.setTextColor(rssi < -70 ? TFT_RED : TFT_GREEN, TFT_BLACK);
  char wifiBuf[16];
  snprintf(wifiBuf, sizeof(wifiBuf), "WiFi:%d", (int)rssi);
  M5.Lcd.drawString(wifiBuf, 0, Layout::STATUS_H, GFXFF);

  // 電池
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  char batBuf[12];
  snprintf(batBuf, sizeof(batBuf), "%.2fV", M5.Axp.GetBatVoltage());
  M5.Lcd.drawString(batBuf, 120, Layout::STATUS_H, GFXFF);

  // 日期時間（RTC）
  HalRtc::DateTime dt;
  HalRtc::readDateTime(dt);
  if (dt.isValid()) {
    char timeBuf[12];
    HalRtc::formatTime(dt, timeBuf, sizeof(timeBuf));
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString(timeBuf, 200, Layout::STATUS_H, GFXFF);
  }

  // Active Preset 名稱
  const Preset& p = PresetMgr::getActive();
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setTextDatum(BR_DATUM);
  M5.Lcd.drawString(p.name[0] ? p.name : "MIC",
                    HW::LCD_W, Layout::STATUS_H, GFXFF);
  // IP bar 只在 HOME 畫面顯示（由 drawHomeScreen 呼叫）
}

/**
 * @brief 繪製 IP 位址列
 *
 * STA 模式：顯示 Router 分配的 IP（藍綠色）
 * AP 模式：顯示 "[AP] 192.168.4.1"（橙色）
 * 未連線：灰色
 *
 * 高度 = IP_H（16px），Y = IP_Y（204）。
 */
void UIScreen::drawIPBar() {
  M5.Lcd.fillRect(0, Layout::IP_Y, HW::LCD_W, Layout::IP_H, UIColor::BLUE_DARK);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(BC_DATUM);

  String ip = NetworkMgr::getIPString();
  uint16_t col;
  if (NetworkMgr::isWiFiConnected()) {
    col = TFT_CYAN;                    // STA 模式：藍綠色
  } else if (NetworkMgr::isAPMode()) {
    // AP 模式：橙色 + 顯示熱點 SSID 提示
    col = 0xFD20;  // 橙色
    String apStr = String("[AP] ") + ip;
    ip = apStr;
  } else {
    col = TFT_DARKGREY;                // 未連線
  }
  M5.Lcd.setTextColor(col, UIColor::BLUE_DARK);
  M5.Lcd.drawString(ip.c_str(),
                    HW::LCD_W / 2,
                    Layout::IP_Y + Layout::IP_H - 2, GFXFF);
}

/**
 * @brief 繪製底部按鍵標籤列（BtnA / BtnB / BtnC）
 *
 * 高度 20px，Y = BTN_Y（220）。
 * nullptr 標籤 → 對應位置留空（不繪製）。
 *
 * @param labelA  BtnA 標籤（左）
 * @param labelB  BtnB 標籤（中）
 * @param labelC  BtnC 標籤（右）
 */
void UIScreen::drawBtnBar(const char* labelA, const char* labelB, const char* labelC) {
  M5.Lcd.fillRect(0, Layout::BTN_Y, HW::LCD_W, 20, TFT_YELLOW);
  M5.Lcd.setTextColor(TFT_BLACK, TFT_YELLOW);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(MC_DATUM);   // 垂直居中
  M5.Lcd.setFreeFont(FSS9);        // FSB12 → FSS9（縮小字型）
  int16_t midY = Layout::BTN_Y + 10;  // 20px 列的垂直中心
  if (labelA) M5.Lcd.drawString(labelA, 55,             midY, GFXFF);
  if (labelB) M5.Lcd.drawString(labelB, HW::LCD_W / 2, midY, GFXFF);
  if (labelC) M5.Lcd.drawString(labelC, 270,            midY, GFXFF);
}

/**
 * @brief 繪製左下角 HOME 觸控區 overlay（供部分模式使用）
 *
 * 在遊戲區左下角繪製小型 [HOME] 標籤，
 * 觸控熱區對應 Layout::HOME_BTN_* 常數。
 */
void UIScreen::drawHomeBtnOverlay() {
  // 左下角半透明 [HOME] 觸控區
  M5.Lcd.fillRoundRect(Layout::HOME_BTN_X, Layout::HOME_BTN_Y,
                        Layout::HOME_BTN_W, Layout::HOME_BTN_H,
                        3, UIColor::CARD_BG);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, UIColor::CARD_BG);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString("HOME",
                    Layout::HOME_BTN_X + Layout::HOME_BTN_W / 2,
                    Layout::HOME_BTN_Y + Layout::HOME_BTN_H / 2, GFXFF);
}


// ============================================================
// [UI2-impl] 模式共用元件
// ============================================================
/**
 * @brief 清除遊戲區（STATUS_H 到 IP_Y，高度 = GAME_AREA_H = 184px）
 */
void UIScreen::clearGameArea() {
  M5.Lcd.fillRect(0, Layout::GAME_AREA_Y,
                  HW::LCD_W, Layout::GAME_AREA_H, TFT_BLACK);
  // 同時清除 IP bar 區域（避免 HOME 的 IP bar 在其他頁面留殘影）
  M5.Lcd.fillRect(0, Layout::IP_Y, HW::LCD_W, Layout::IP_H, TFT_BLACK);
}

void UIScreen::resetShotPage() {
  s_pageBase    = 1;
  s_totalShotUI = 0;
  // 清除 shot row 區域，避免新局開始時顯示舊成績
  M5.Lcd.fillRect(0, SHOT_START_Y, HW::LCD_W,
                  SHOT_END_Y_VAL - SHOT_START_Y + SHOT_ROW_H, TFT_BLACK);
}
bool UIScreen::shotPageHasPrev() {
  return s_pageBase > 1;
}
bool UIScreen::shotPageHasNext() {
  // 下一頁起點
  uint32_t nextBase = s_pageBase + ROWS_PER_PAGE_UI;
  return nextBase <= s_totalShotUI;
}
void UIScreen::shotPagePrev() {
  if (!shotPageHasPrev()) return;
  s_pageBase = (s_pageBase > ROWS_PER_PAGE_UI)
               ? s_pageBase - ROWS_PER_PAGE_UI : 1;
}
void UIScreen::shotPageNext() {
  if (!shotPageHasNext()) return;
  s_pageBase += ROWS_PER_PAGE_UI;
}

uint32_t UIScreen::getPageBase()    { return s_pageBase; }
uint8_t  UIScreen::getRowsPerPage() { return ROWS_PER_PAGE_UI; }
void     UIScreen::setTotalShotUI(uint32_t total) { s_totalShotUI = total; }

/**
 * @brief 繪製指定 hitIdx 在已知 pageBase 的列位置（不自動推頁）
 * 供翻頁重繪（redrawShotPage）使用，避免自動推頁覆蓋 s_pageBase。
 */
void UIScreen::drawShotRowAt(uint32_t hitIdx, uint32_t pageBase,
                              uint8_t stationId,
                              unsigned long elapsed, unsigned long split,
                              bool pass) {
  if (hitIdx < pageBase || hitIdx >= pageBase + ROWS_PER_PAGE_UI) return;

  constexpr int16_t START_Y    = SHOT_START_Y;
  constexpr int16_t SHOT_END_Y = SHOT_END_Y_VAL;
  constexpr int16_t ROW_H      = SHOT_ROW_H;

  uint8_t  localRow = (uint8_t)(hitIdx - pageBase);
  int16_t  y0       = START_Y + localRow * ROW_H;
  int16_t  yC       = y0 + ROW_H / 2;

  M5.Lcd.fillRect(0, y0, HW::LCD_W, ROW_H - 1, TFT_BLACK);

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  bool isOverflow = (hitIdx > Limits::HIT_MAX);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(isOverflow ? 0xFD20 : UIColor::TEXT_DIM, TFT_BLACK);
  char numBuf[5]; snprintf(numBuf, sizeof(numBuf), "#%02u", (unsigned)hitIdx);
  M5.Lcd.drawString(numBuf, 38, yC, GFXFF);

  bool isMic = (stationId == 0);
  uint16_t srcBg  = isMic ? 0x1A12 : 0x0A20;
  uint16_t srcCol = isMic ? 0xFFC0 : 0x40B8;
  char srcBuf[6];
  if (isMic) strlcpy(srcBuf, "MIC", sizeof(srcBuf));
  else snprintf(srcBuf, sizeof(srcBuf), "S%u", stationId);
  M5.Lcd.fillRoundRect(40, y0 + 1, 30, ROW_H - 3, 2, srcBg);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(srcCol, srcBg);
  M5.Lcd.drawString(srcBuf, 55, yC, GFXFF);

  char elBuf[10]; fmtTime(elapsed, elBuf, sizeof(elBuf));
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString(elBuf, 220, yC, GFXFF);

  char spBuf[10]; fmtTime(split, spBuf, sizeof(spBuf));
  bool fast = (split < 600);
  uint16_t spColor = pass
    ? (fast ? UIColor::SPLIT_FAST : UIColor::SPLIT_SLOW)
    : UIColor::FAIL_RED;
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(spColor, TFT_BLACK);
  M5.Lcd.drawString(spBuf, 316, yC, GFXFF);
}

/**
 * @brief 繪製模式標題列（模式名稱 + 狀態 badge）
 *
 * 高度 = MODE_HDR_H（18px），Y = GAME_AREA_Y（20）。
 *
 * @param title      模式名稱（左側，如 "FREE SHOOTING"）
 * @param badge      狀態文字（右側，如 "GOING"、"STANDBY"）
 * @param badgeColor badge 背景色（RGB565）
 */
void UIScreen::drawModeHeader(const char* title, const char* badge, uint16_t badgeColor) {
  // 多清 2px 確保字體 descender 殘影也被清除
  constexpr int16_t CLEAR_H = Layout::MODE_HDR_H + 2;
  M5.Lcd.fillRect(0, Layout::GAME_AREA_Y, HW::LCD_W, CLEAR_H, UIColor::CARD_BG);

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextDatum(ML_DATUM);
  // title 不帶背景色，避免 bounding box 溢出
  M5.Lcd.setTextColor(TFT_CYAN);
  M5.Lcd.drawString(title, 4, Layout::GAME_AREA_Y + Layout::MODE_HDR_H / 2, GFXFF);

  // 狀態徽章（右側）
  if (badge) {
    constexpr int16_t BW_MAX = 90;
    constexpr int16_t bh = 14;
    int16_t by = Layout::GAME_AREA_Y + 2;

    // 先清右側整塊區域（比 MODE_HDR_H 多 2px）
    M5.Lcd.fillRect(HW::LCD_W - BW_MAX - 6, Layout::GAME_AREA_Y,
                    BW_MAX + 6, CLEAR_H, UIColor::CARD_BG);

    // 動態計算 badge 寬度
    M5.Lcd.setFreeFont(FSS9);
    int16_t textW = M5.Lcd.textWidth(badge, GFXFF);
    int16_t bw = constrain((int16_t)(textW + 14), (int16_t)48, BW_MAX);
    int16_t bx = HW::LCD_W - bw - 4;

    // badge 背景（用 fillRect 替代 fillRoundRect，徹底清角落）
    M5.Lcd.fillRect(bx, by, bw, bh, badgeColor);
    // 文字不帶背景色，避免 drawString bounding box 超出 badge 範圍留殘影
    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.drawString(badge, bx + bw / 2, by + bh / 2, GFXFF);
  }
}

/**
 * @brief 繪製單筆命中記錄列
 *
 * 支援雙欄排版（1–5 左欄，6–10 右欄），
 * 每列高度 SHOT_ROW_H（20px）。
 * pass=false 時 split 顯示為紅色警告。
 *
 * @param hitIdx    命中序號（1-based）
 * @param stationId 來源（0=MIC, 1+=ESP-NOW）
 * @param elapsed   累積時間（ms）
 * @param split     分段時間（ms）
 * @param pass      是否在 par time 內（預設 true）
 */
void UIScreen::drawShotRow(uint32_t hitIdx, uint8_t stationId,
                           unsigned long elapsed, unsigned long split,
                           bool pass) {
  // hitIdx = 真實第幾發（1-based，可超過 HIT_MAX）
  if (hitIdx == 0) return;

  // ── 佈局常數（使用 file-level 共用常數）──────────────────────
  constexpr int16_t START_Y    = SHOT_START_Y;
  constexpr int16_t SHOT_END_Y = SHOT_END_Y_VAL;
  constexpr int16_t ROW_H      = SHOT_ROW_H;

  // 更新總發數記錄
  if (hitIdx > s_totalShotUI) s_totalShotUI = hitIdx;

  // 整頁翻頁：hitIdx 超出目前頁尾才推進（整頁對齊）
  // 頁 1：hitIdx 1-7，頁 2：8-14，頁 3：15-21 ...
  // s_pageBase = ((hitIdx-1) / ROWS_PER_PAGE_UI) * ROWS_PER_PAGE_UI + 1
  uint32_t correctBase = ((hitIdx - 1) / ROWS_PER_PAGE_UI) * ROWS_PER_PAGE_UI + 1;
  if (correctBase != s_pageBase) {
    s_pageBase = correctBase;
    // 多清一個 ROW_H 確保最後一行的行距也清除
    M5.Lcd.fillRect(0, START_Y, HW::LCD_W,
                    SHOT_END_Y - START_Y + ROW_H, TFT_BLACK);
  }

  uint8_t localRow = (uint8_t)(hitIdx - s_pageBase);
  if (localRow >= ROWS_PER_PAGE_UI) return;

  int16_t y0 = START_Y + localRow * ROW_H;
  int16_t yC = y0 + ROW_H / 2;
  M5.Lcd.fillRect(0, y0, HW::LCD_W, ROW_H - 1, TFT_BLACK);

  // ── 繪製各欄位（全寬 320px）────────────────────────────────
  // 序號 [#XX]  來源badge  elapsed    split
  // x:    2       22        180        280
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  // 序號（顯示真實發數，超過 HIT_MAX 用橙色提示環形覆寫）
  bool isOverflow = (hitIdx > Limits::HIT_MAX);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(isOverflow ? 0xFD20 : UIColor::TEXT_DIM, TFT_BLACK);
  char numBuf[5]; snprintf(numBuf, sizeof(numBuf), "#%02u", hitIdx);
  M5.Lcd.drawString(numBuf, 38, yC, GFXFF);

  // 來源徽章
  bool isMic = (stationId == 0);
  uint16_t srcBg  = isMic ? 0x1A12 : 0x0A20;
  uint16_t srcCol = isMic ? 0xFFC0 : 0x40B8;
  char srcBuf[6];
  if (isMic) strlcpy(srcBuf, "MIC", sizeof(srcBuf));
  else snprintf(srcBuf, sizeof(srcBuf), "S%u", stationId);
  M5.Lcd.fillRoundRect(40, y0 + 1, 30, ROW_H - 3, 2, srcBg);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(srcCol, srcBg);
  M5.Lcd.drawString(srcBuf, 55, yC, GFXFF);

  // 累計時間（右對齊到 240px）
  char elBuf[10]; fmtTime(elapsed, elBuf, sizeof(elBuf));
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString(elBuf, 220, yC, GFXFF);

  // 分段時間（右對齊到 316px）
  char spBuf[10]; fmtTime(split, spBuf, sizeof(spBuf));
  bool fast = (split < 600);
  uint16_t spColor = pass
    ? (fast ? UIColor::SPLIT_FAST : UIColor::SPLIT_SLOW)
    : UIColor::FAIL_RED;
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(spColor, TFT_BLACK);
  M5.Lcd.drawString(spBuf, 316, yC, GFXFF);
}

/**
 * @brief 繪製 FREE 模式結果畫面（成績統計 + 最佳紀錄）
 *
 * @param totalMs   本局總時間（ms）
 * @param hitCount  本局命中數
 * @param bestTotal 歷史最佳總時間（秒）
 * @param bestAvg   歷史最佳平均 split（秒）
 */
void UIScreen::drawResultScreen(unsigned long totalMs, uint8_t hitCount,
                                float bestTotal, float bestAvg) {
  // ── Stats 區：2 行 × 24px + 2px 分隔線 = 50px ─────────────
  // STATS_Y = 204 - 50 = 154，行1 中心 Y=166，行2 中心 Y=190
  // FSS9 字高約 11px，行高 24px → 上下各留 ~6.5px，完全不超出
  constexpr int16_t STATS_H = 50;
  constexpr int16_t DIVIDER = 2;
  constexpr int16_t STATS_Y = Layout::IP_Y - STATS_H;   // 154
  constexpr int16_t ROW_H   = 24;
  constexpr int16_t COL_W   = HW::LCD_W / 3;            // 106px
  constexpr uint16_t BG     = 0x0821;

  // 分隔線 + 背景
  M5.Lcd.fillRect(0, STATS_Y - DIVIDER, HW::LCD_W, DIVIDER, UIColor::CARD_BORD);
  M5.Lcd.fillRect(0, STATS_Y, HW::LCD_W, STATS_H, BG);

  // 三欄垂直分隔線
  M5.Lcd.drawFastVLine(COL_W,     STATS_Y, STATS_H, UIColor::CARD_BORD);
  M5.Lcd.drawFastVLine(COL_W * 2, STATS_Y, STATS_H, UIColor::CARD_BORD);

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  unsigned long avg = (hitCount > 0 && totalMs > 0) ? (totalMs / hitCount) : 0;
  char totBuf[10]; fmtTime(totalMs, totBuf, sizeof(totBuf));
  char avgBuf[10]; fmtTime(avg,     avgBuf, sizeof(avgBuf));
  char buf[14];

  // 各欄水平分界（label 左對齊，值右對齊）
  // 欄0: x=0~105  欄1: x=106~211  欄2: x=212~319
  int16_t lx0 = 3,           rx0 = COL_W - 3;
  int16_t lx1 = COL_W + 3,   rx1 = COL_W * 2 - 3;
  int16_t lx2 = COL_W * 2 + 3, rx2 = HW::LCD_W - 3;

  // 行1 / 行2 垂直中心
  int16_t y1 = STATS_Y + ROW_H / 2;         // 154 + 12 = 166
  int16_t y2 = STATS_Y + ROW_H + ROW_H / 2; // 154 + 36 = 190

  // ── 行 1：SHOTS X/10 | TOTAL X.XXs | AVG X.XXs ────────────
  // 欄0: SHOTS
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  M5.Lcd.drawString("SH", lx0, y1, GFXFF);
  snprintf(buf, sizeof(buf), "%u/%u", hitCount, (unsigned)Limits::HIT_MAX);
  M5.Lcd.setTextColor(TFT_WHITE, BG);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.drawString(buf, rx0, y1, GFXFF);

  // 欄1: TOTAL
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  M5.Lcd.drawString("TOT", lx1, y1, GFXFF);
  snprintf(buf, sizeof(buf), "%ss", totBuf);
  M5.Lcd.setTextColor(TFT_WHITE, BG);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.drawString(buf, rx1, y1, GFXFF);

  // 欄2: AVG
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  M5.Lcd.drawString("AVG", lx2, y1, GFXFF);
  if (avg > 0) {
    snprintf(buf, sizeof(buf), "%ss", avgBuf);
    M5.Lcd.setTextColor(TFT_WHITE, BG);
  } else {
    strlcpy(buf, "--", sizeof(buf));
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  }
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.drawString(buf, rx2, y1, GFXFF);

  // ── 行 2：* BEST * | TOTAL best | AVG best ─────────────────
  // 欄0: ★ BEST 標籤
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(0x39C7, BG);
  M5.Lcd.drawString("* BEST *", COL_W / 2, y2, GFXFF);

  // 欄1: BEST TOTAL
  if (bestTotal > 0.0f) {
    snprintf(buf, sizeof(buf), "%.2fs", bestTotal);
    M5.Lcd.setTextColor(UIColor::PASS_GREEN, BG);
  } else {
    strlcpy(buf, "--", sizeof(buf));
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  }
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(buf, COL_W + COL_W / 2, y2, GFXFF);

  // 欄2: BEST AVG
  if (bestAvg > 0.0f) {
    snprintf(buf, sizeof(buf), "%.2fs", bestAvg);
    M5.Lcd.setTextColor(UIColor::PASS_GREEN, BG);
  } else {
    strlcpy(buf, "--", sizeof(buf));
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, BG);
  }
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(buf, COL_W * 2 + COL_W / 2, y2, GFXFF);
}

// ─── 廢棄注解已移除 ───


// ============================================================
// [UI3-impl] 模式專用畫面
// ============================================================
/**
 * @brief 繪製 Home 模式的 6-card grid 畫面
 *
 * 3×2 卡片排列（FREE / DRILL / DRY FIRE / SPY / RO / HISTORY），
 * 每張卡片顯示圖示和模式名稱，使用 CARD_* Layout 常數計算位置。
 */
void UIScreen::drawHomeScreen() {
  M5.Lcd.clear(TFT_BLACK);
  drawStatusBar();

  // 6-card grid
  constexpr int16_t GY  = Layout::HOME_GRID_Y;
  constexpr int16_t P   = Layout::CARD_PAD;
  constexpr int16_t G   = Layout::CARD_GAP;
  constexpr int16_t CW  = (HW::LCD_W - 2*P - 2*G) / 3;
  constexpr int16_t CH  = (Layout::HOME_GRID_H - 2*P - 1*G) / 2;

  struct CardInfo {
    const char* title;
    const char* sub;
    uint16_t    accentColor;
  };

  static const CardInfo cards[6] = {
    { "FREE",    "Shooting",  TFT_CYAN  },
    { "DRILLS",  "& Scoring", 0xFFC0    },
    { "DRY",     "Fire",      0xA000    },
    { "SPY",     "Mode",      TFT_GREEN },
    { "RO",      "Mode",      0xF800    },
    { "HISTORY", "SD Card",   0xFF80    },
  };

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t col = i % 3;
    uint8_t row = i / 3;
    int16_t x   = P + col * (CW + G);
    int16_t y   = GY + P + row * (CH + G);

    // 卡片背景與邊框
    M5.Lcd.fillRoundRect(x, y, CW, CH, 4, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(x, y, CW, CH, 4, UIColor::CARD_BORD);

    // 頂部色條（accent）
    M5.Lcd.fillRect(x + 1, y + 1, CW - 2, 3, cards[i].accentColor);

    // 標題（縮小：FSB12 → FSS9，避免溢出卡片）
    M5.Lcd.setFreeFont(FSS9);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString(cards[i].title, x + CW/2, y + CH/2 - 7, GFXFF);

    // 副標題（使用內建小字型更清晰）
    M5.Lcd.setFreeFont(nullptr);   // 回到內建 bitmap font
    M5.Lcd.setTextSize(1);         // 6x8 像素，最小清晰字型
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, UIColor::CARD_BG);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.drawString(cards[i].sub, x + CW/2, y + CH/2 + 9, GFXFF);
  }

  drawIPBar();
  drawBtnBar("STOP", "SETTINGS", "START");
}

/**
 * @brief 繪製 SETTINGS 模式畫面（4 個設定列）
 *
 * 列佈局（ROW_H = 41px，Y0 = GAME_AREA_Y + MODE_HDR_H + 2 = 40）：
 *  - Row 0：PRESET 槍型（◀ / 名稱 / ▶）
 *  - Row 1：SOURCE 三個按鈕（ESP-NOW / MIC / BOTH）
 *  - Row 2：THRESH RMS 門檻（− / 數值 / +）
 *  - Row 3：RAND DELAY 開關（OFF / ON）
 *
 * @param s  目前 AppSettings（_edit 暫存值）
 */
void UIScreen::drawSettingsScreen(const AppSettings& s) {
  // ── 清除 + Header ────────────────────────────────────────────
  M5.Lcd.fillRect(0, Layout::GAME_AREA_Y, HW::LCD_W, Layout::GAME_AREA_H, TFT_BLACK);
  drawModeHeader("SETTINGS", nullptr, 0);

  // 可用高度 = 184 - 18 = 166px，5行 × 33px = 165px
  constexpr int16_t Y0    = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2;
  constexpr int16_t ROW_H = 33;
  constexpr int16_t LBL_X = 4;
  constexpr int16_t VAL_X = 110;
  constexpr int16_t CX    = HW::LCD_W / 2;

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  // 分隔線輔助 lambda
  auto drawDiv = [&](uint8_t row) {
    M5.Lcd.drawFastHLine(0, Y0 + row * ROW_H, HW::LCD_W, UIColor::CARD_BORD);
  };

  // ── Row 0：PRESET ────────────────────────────────────────────
  {
    constexpr int16_t RY  = Y0;
    constexpr int16_t RCY = RY + ROW_H / 2;
    constexpr int16_t AW  = 24, AH = 22;
    constexpr int16_t LX  = VAL_X;
    constexpr int16_t RX  = HW::LCD_W - AW - 4;
    constexpr int16_t NCX = (LX + AW + RX) / 2;  // 名稱中心 = 兩箭頭之間中點

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString("PRESET", LBL_X, RCY, GFXFF);

    // ◀ 按鈕
    M5.Lcd.fillRoundRect(LX, RY + 6, AW, AH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(LX, RY + 6, AW, AH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString("<", LX + AW/2, RCY, GFXFF);

    // Preset 名稱（置中於兩箭頭之間）
    Preset p; PresetMgr::get(s.activePreset, p);
    char nameBuf[24];
    snprintf(nameBuf, sizeof(nameBuf), "%u:%s", s.activePreset + 1, p.name);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.drawString(nameBuf, NCX, RCY, GFXFF);

    // ▶ 按鈕
    M5.Lcd.fillRoundRect(RX, RY + 6, AW, AH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(RX, RY + 6, AW, AH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString(">", RX + AW/2, RCY, GFXFF);
    drawDiv(1);
  }

  // ── Row 1：SOURCE ────────────────────────────────────────────
  {
    constexpr int16_t RY   = Y0 + ROW_H;
    constexpr int16_t RCY  = RY + ROW_H / 2;
    // ESP-NOW 縮寫為 E-NOW，FSS9 約 5×9=45px，BW=64 綽綽有餘
    // 3×64 + 2×6 = 204，起點 VAL_X=110，終點 110+204=314
    constexpr int16_t SX   = VAL_X;
    constexpr int16_t BW   = 64, BH = 22, GAP = 6;
    static const char* srcLabels[] = { "E-NOW", "MIC", "BOTH" };

    M5.Lcd.setFreeFont(FSS9);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString("SOURCE", LBL_X, RCY, GFXFF);

    for (uint8_t i = 0; i < 3; i++) {
      int16_t bx = SX + i * (BW + GAP);
      bool    sel = (static_cast<uint8_t>(s.hitSource) == i);
      uint16_t bg  = sel ? 0x0060 : UIColor::CARD_BG;
      uint16_t col = sel ? TFT_GREEN : UIColor::TEXT_DIM;
      M5.Lcd.fillRoundRect(bx, RY + 6, BW, BH, 3, bg);
      M5.Lcd.drawRoundRect(bx, RY + 6, BW, BH, 3,
                           sel ? TFT_GREEN : UIColor::CARD_BORD);
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextColor(col, bg);
      M5.Lcd.drawString(srcLabels[i], bx + BW/2, RCY, GFXFF);
    }
    drawDiv(2);
  }

  // ── Row 2：THRESH ────────────────────────────────────────────
  {
    constexpr int16_t RY  = Y0 + 2 * ROW_H;
    constexpr int16_t RCY = RY + ROW_H / 2;
    constexpr int16_t BW  = 28, BH = 22;
    constexpr int16_t LX  = VAL_X;
    constexpr int16_t RX  = HW::LCD_W - BW - 4;
    constexpr int16_t NCX = (LX + BW + RX) / 2;  // 數值中心 = 兩按鈕之間中點

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString("THRESH", LBL_X, RCY, GFXFF);

    // − 按鈕
    M5.Lcd.fillRoundRect(LX, RY + 6, BW, BH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(LX, RY + 6, BW, BH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString("-", LX + BW/2, RCY, GFXFF);

    // 數值（置中於兩按鈕之間）
    char buf[8]; snprintf(buf, sizeof(buf), "%d", s.micThresh);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.drawString(buf, NCX, RCY, GFXFF);

    // + 按鈕
    M5.Lcd.fillRoundRect(RX, RY + 6, BW, BH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(RX, RY + 6, BW, BH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString("+", RX + BW/2, RCY, GFXFF);
    drawDiv(3);
  }

  // ── Row 3：RAND DELAY ────────────────────────────────────────
  {
    constexpr int16_t RY  = Y0 + 3 * ROW_H;
    constexpr int16_t RCY = RY + ROW_H / 2;
    constexpr int16_t BW  = 60, BH = 22;

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString("RAND DLY", LBL_X, RCY, GFXFF);

    bool on = s.randomDelay;
    uint16_t bg  = on ? 0x0060 : UIColor::CARD_BG;
    uint16_t col = on ? TFT_GREEN : UIColor::TEXT_DIM;
    M5.Lcd.fillRoundRect(VAL_X, RY + 6, BW, BH, 3, bg);
    M5.Lcd.drawRoundRect(VAL_X, RY + 6, BW, BH, 3,
                         on ? TFT_GREEN : UIColor::CARD_BORD);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(col, bg);
    M5.Lcd.drawString(on ? "ON" : "OFF", VAL_X + BW/2, RCY, GFXFF);
    drawDiv(4);
  }

  // ── Row 4：MAX SHOTS（Free/RO 最大發數）────────────────────
  {
    constexpr int16_t RY  = Y0 + 4 * ROW_H;
    constexpr int16_t RCY = RY + ROW_H / 2;
    constexpr int16_t BW  = 28, BH = 20;
    constexpr int16_t LX  = VAL_X;
    constexpr int16_t RX  = HW::LCD_W - BW - 4;
    constexpr int16_t NCX = (LX + BW + RX) / 2;

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString("MAX SHOTS", LBL_X, RCY, GFXFF);

    // − 按鈕
    M5.Lcd.fillRoundRect(LX, RY + 4, BW, BH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(LX, RY + 4, BW, BH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString("-", LX + BW/2, RCY, GFXFF);

    // 數值（0 顯示 "∞"）
    char buf[8];
    if (s.maxShots == 0) strlcpy(buf, "INF", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%u", s.maxShots);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(s.maxShots == 0 ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
    M5.Lcd.drawString(buf, NCX, RCY, GFXFF);

    // + 按鈕
    M5.Lcd.fillRoundRect(RX, RY + 4, BW, BH, 3, UIColor::CARD_BG);
    M5.Lcd.drawRoundRect(RX, RY + 4, BW, BH, 3, UIColor::CARD_BORD);
    M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
    M5.Lcd.drawString("+", RX + BW/2, RCY, GFXFF);
  }

  drawBtnBar("HOME", nullptr, "SAVE");
  // IP bar 清除（Settings 模式不顯示 IP）
  M5.Lcd.fillRect(0, Layout::IP_Y, HW::LCD_W, Layout::IP_H, TFT_BLACK);
}

/**
 * @brief 繪製 Par Time 設定畫面
 *
 * 顯示目前 par time 值和 −500ms / +500ms 調整按鈕，
 * 以及 CANCEL / DONE 操作按鈕。
 *
 * @param parMs  目前 par time（ms），0 = 停用
 */
void UIScreen::drawParSetScreen(uint32_t parMs) {
  // ── 清除遊戲區 ──────────────────────────────────────────────
  M5.Lcd.fillRect(0, Layout::GAME_AREA_Y, HW::LCD_W, Layout::GAME_AREA_H, TFT_BLACK);

  // ── Header ──────────────────────────────────────────────────
  drawModeHeader("FREE SHOOTING", "PAR SET", 0xFD20);

  constexpr int16_t CX   = HW::LCD_W / 2;          // 160
  constexpr int16_t Y0   = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 8;

  // ── 標籤 ─────────────────────────────────────────────────────
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString("PAR TIME", CX, Y0 + 14, GFXFF);

  // ── 數值（大字）─────────────────────────────────────────────
  char valBuf[12];
  if (parMs == 0) {
    strlcpy(valBuf, "OFF", sizeof(valBuf));
  } else {
    snprintf(valBuf, sizeof(valBuf), "%.1f s", parMs / 1000.0f);
  }
  M5.Lcd.setFreeFont(FSB12);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.drawString(valBuf, CX, Y0 + 52, GFXFF);

  // ── − / + 按鈕（各 50×36px）────────────────────────────────
  constexpr int16_t BTN_W  = 50;
  constexpr int16_t BTN_H  = 36;
  constexpr int16_t BTN_Y  = Y0 + 34;
  constexpr int16_t BTN_LX = CX - 80 - BTN_W;   // 左按鈕 X = 30
  constexpr int16_t BTN_RX = CX + 80;            // 右按鈕 X = 240

  M5.Lcd.fillRoundRect(BTN_LX, BTN_Y, BTN_W, BTN_H, 4, UIColor::CARD_BG);
  M5.Lcd.drawRoundRect(BTN_LX, BTN_Y, BTN_W, BTN_H, 4, UIColor::CARD_BORD);
  M5.Lcd.setFreeFont(FSB12);
  M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString("-", BTN_LX + BTN_W/2, BTN_Y + BTN_H/2, GFXFF);

  M5.Lcd.fillRoundRect(BTN_RX, BTN_Y, BTN_W, BTN_H, 4, UIColor::CARD_BG);
  M5.Lcd.drawRoundRect(BTN_RX, BTN_Y, BTN_W, BTN_H, 4, UIColor::CARD_BORD);
  M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
  M5.Lcd.drawString("+", BTN_RX + BTN_W/2, BTN_Y + BTN_H/2, GFXFF);

  // ── Step 提示 ────────────────────────────────────────────────
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString("step: 0.5s  (0 = OFF)", CX, Y0 + 84, GFXFF);

  // ── CANCEL / DONE 按鈕 ───────────────────────────────────────
  constexpr int16_t ACT_W  = 100;
  constexpr int16_t ACT_H  = 28;
  constexpr int16_t ACT_Y  = Y0 + 104;
  constexpr int16_t ACT_LX = CX - 110;   // CANCEL X = 50
  constexpr int16_t ACT_RX = CX + 10;    // DONE X   = 170

  M5.Lcd.fillRoundRect(ACT_LX, ACT_Y, ACT_W, ACT_H, 4, 0x4000);
  M5.Lcd.drawRoundRect(ACT_LX, ACT_Y, ACT_W, ACT_H, 4, 0xF800);
  M5.Lcd.setTextColor(0xF800, 0x4000);
  M5.Lcd.drawString("CANCEL", ACT_LX + ACT_W/2, ACT_Y + ACT_H/2, GFXFF);

  M5.Lcd.fillRoundRect(ACT_RX, ACT_Y, ACT_W, ACT_H, 4, 0x0060);
  M5.Lcd.drawRoundRect(ACT_RX, ACT_Y, ACT_W, ACT_H, 4, TFT_GREEN);
  M5.Lcd.setTextColor(TFT_GREEN, 0x0060);
  M5.Lcd.drawString("DONE", ACT_RX + ACT_W/2, ACT_Y + ACT_H/2, GFXFF);

  // ── Btn bar ──────────────────────────────────────────────────
  drawBtnBar(nullptr, nullptr, nullptr);
}

/**
 * @brief 繪製 DRILL SETUP 畫面（發數 / Par Time / Pass % / Rand Delay）
 * @param def  目前 DrillDef 設定
 */
void UIScreen::drawDrillSetup(const DrillDef& def) {
  // 可用高度 = GAME_AREA_H - MODE_HDR_H = 184 - 18 = 166px
  // 4 行 × 40px = 160px，剩 6px 頂部邊距
  constexpr int16_t ROW_H = 40;
  constexpr int16_t LBL_X = 4;
  constexpr int16_t Y0    = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 4;
  constexpr int16_t CX    = HW::LCD_W / 2;

  // 按鍵佈局：值在中間，[-] 靠左，[+] 靠右，各留足夠空間
  constexpr int16_t BTN_W = 40;   // 按鍵寬度（比原本 28px 大）
  constexpr int16_t BTN_H = 28;   // 按鍵高度
  constexpr int16_t VAL_W = 70;   // 數值顯示寬度
  constexpr int16_t LBL_W = 80;   // 標籤寬度

  // 按鍵 X 座標：[-] 在標籤右邊，[+] 靠右側
  constexpr int16_t BTN_L_X = LBL_W + 4;          // [-] 起點 x=84
  constexpr int16_t VAL_X   = BTN_L_X + BTN_W + 4; // 數值中心基準
  constexpr int16_t BTN_R_X = HW::LCD_W - BTN_W - 4; // [+] 靠右 x=276

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  auto drawRow = [&](uint8_t row, const char* label, const char* val,
                     bool hasBtn, bool isBig = false) {
    int16_t ry  = Y0 + row * ROW_H;
    int16_t rcy = ry + ROW_H / 2;

    // 分隔線
    if (row > 0)
      M5.Lcd.drawFastHLine(0, ry, HW::LCD_W, UIColor::CARD_BORD);

    // 標籤
    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString(label, LBL_X, rcy, GFXFF);

    if (hasBtn) {
      // [-] 按鍵
      M5.Lcd.fillRoundRect(BTN_L_X, ry + 6, BTN_W, BTN_H, 4, UIColor::CARD_BG);
      M5.Lcd.drawRoundRect(BTN_L_X, ry + 6, BTN_W, BTN_H, 4, UIColor::CARD_BORD);
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
      M5.Lcd.drawString("-", BTN_L_X + BTN_W/2, rcy, GFXFF);

      // 數值
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Lcd.drawString(val, CX, rcy, GFXFF);

      // [+] 按鍵
      M5.Lcd.fillRoundRect(BTN_R_X, ry + 6, BTN_W, BTN_H, 4, UIColor::CARD_BG);
      M5.Lcd.drawRoundRect(BTN_R_X, ry + 6, BTN_W, BTN_H, 4, UIColor::CARD_BORD);
      M5.Lcd.setTextColor(TFT_WHITE, UIColor::CARD_BG);
      M5.Lcd.drawString("+", BTN_R_X + BTN_W/2, rcy, GFXFF);
    } else {
      // 無按鍵：只顯示數值 badge
      uint16_t bg  = (strcmp(val, "ON") == 0)  ? 0x0060 : UIColor::CARD_BG;
      uint16_t col = (strcmp(val, "ON") == 0)  ? TFT_GREEN : UIColor::TEXT_DIM;
      M5.Lcd.fillRoundRect(VAL_X, ry + 6, VAL_W, BTN_H, 4, bg);
      M5.Lcd.drawRoundRect(VAL_X, ry + 6, VAL_W, BTN_H, 4,
                           (strcmp(val, "ON") == 0) ? TFT_GREEN : UIColor::CARD_BORD);
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextColor(col, bg);
      M5.Lcd.drawString(val, VAL_X + VAL_W/2, rcy, GFXFF);
    }
  };

  // Row 0：Shots
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", def.shotCount);
  drawRow(0, "Shots", buf, true);

  // Row 1：Par Time
  if (def.parTimeMs > 0) {
    char parBuf[10]; fmtTime(def.parTimeMs, parBuf, sizeof(parBuf));
    snprintf(buf, sizeof(buf), "%ss", parBuf);
  } else {
    strlcpy(buf, "OFF", sizeof(buf));
  }
  drawRow(1, "Par Time", buf, true);

  // Row 2：Pass %
  snprintf(buf, sizeof(buf), "%u%%", def.passPct);
  drawRow(2, "Pass %", buf, false);

  // Row 3：Random
  drawRow(3, "Random", def.randomDelay ? "ON" : "OFF", false);
}

/**
 * @brief 繪製 DRILL 結果畫面（PASSED/FAILED + 達成率 + 最佳紀錄）
 * @param result  DrillResult 結構（含 def / record / score / passed）
 */
void UIScreen::drawDrillResult(const DrillResult& result) {
  constexpr int16_t Y0 = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 6;
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextDatum(TL_DATUM);

  // Score (大字)
  char scoreBuf[16];
  snprintf(scoreBuf, sizeof(scoreBuf), "%.0f%%", result.score);
  M5.Lcd.setFreeFont(FSB12);
  M5.Lcd.setTextColor(
    result.passed ? UIColor::PASS_GREEN : UIColor::FAIL_RED, TFT_BLACK);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(scoreBuf, HW::LCD_W / 2, Y0 + 20, GFXFF);

  // Pass / Fail 徽章
  drawBadge(HW::LCD_W/2 - 40, Y0 + 40, 80, 18,
            result.passed ? "PASSED" : "FAILED",
            result.passed ? 0x0060 : 0x6000,
            result.passed ? TFT_GREEN : TFT_RED);

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.setTextDatum(TL_DATUM);

  char buf[32];
  snprintf(buf, sizeof(buf), "Shots: %u / %u",
           result.record.hit_count, result.def.shotCount);
  M5.Lcd.drawString(buf, 8, Y0 + 68, GFXFF);

  snprintf(buf, sizeof(buf), "Pass : %u shots", result.passCount);
  M5.Lcd.drawString(buf, 8, Y0 + 88, GFXFF);

  unsigned long totalMs = TimerCore::calcTotalMs(result.record);
  char totBuf[10]; fmtTime(totalMs, totBuf, sizeof(totBuf));
  snprintf(buf, sizeof(buf), "Total: %ss", totBuf);
  M5.Lcd.drawString(buf, 8, Y0 + 108, GFXFF);
}

/**
 * @brief 繪製 DRY FIRE 模式畫面（節拍間隔 / 拍數 / 計時）
 *
 * @param beatMs    目前節拍間隔（ms）
 * @param beatCount 已播放拍數
 * @param elapsedMs session 已執行時間（ms），未執行時為 0
 * @param running   true = 正在執行，false = 停止
 */
void UIScreen::drawDryFireScreen(uint32_t beatMs, uint32_t beatCount,
                                 unsigned long elapsedMs, bool running) {
  constexpr int16_t Y0 = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 8;
  M5.Lcd.setTextDatum(MC_DATUM);

  // Beat interval（大字）
  M5.Lcd.setFreeFont(FSB12);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  char beatBuf[12];
  snprintf(beatBuf, sizeof(beatBuf), "%.1fs", beatMs / 1000.0f);
  M5.Lcd.drawString(beatBuf, HW::LCD_W / 2, Y0 + 28, GFXFF);

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString("BEAT INTERVAL", HW::LCD_W / 2, Y0 + 48, GFXFF);

  if (running) {
    // Beat count
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    char cntBuf[16];
    snprintf(cntBuf, sizeof(cntBuf), "Beat #%u", beatCount);
    M5.Lcd.drawString(cntBuf, HW::LCD_W / 2, Y0 + 70, GFXFF);

    // Elapsed
    char elBuf[12]; fmtTime(elapsedMs, elBuf, sizeof(elBuf));
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%ss", elBuf);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    M5.Lcd.drawString(timeBuf, HW::LCD_W / 2, Y0 + 88, GFXFF);
  }
}

/**
 * @brief 繪製 SPY 模式畫面（命中計數 / 總時間 / split 列表）
 *
 * @param shotCount  目前命中數
 * @param totalMs    從第一槍到最後一槍的總時間（ms）
 * @param hitsPtr    SpyHit 陣列指標（void* 避免標頭依賴）
 * @param listening  true = 監聽中（顯示綠色狀態）
 */
void UIScreen::drawSpyScreen(uint8_t shotCount, unsigned long totalMs,
                              const void* hitsPtr, bool listening) {
  // 使用與 drawShotRow 相同的佈局常數，確保一致性
  constexpr int16_t START_Y    = SHOT_START_Y;      // 40
  constexpr int16_t SHOT_END_Y = SHOT_END_Y_VAL;    // 152
  constexpr int16_t ROW_H      = SHOT_ROW_H;        // 16
  constexpr int16_t STATS_H    = 32;
  constexpr int16_t STATS_Y    = SHOT_END_Y;        // 152

  struct SpyHit { unsigned long timestamp; unsigned long split; };
  const SpyHit* hits = static_cast<const SpyHit*>(hitsPtr);

  // 更新總發數；只有新命中才自動推頁（手動翻頁時不覆蓋 s_pageBase）
  bool isNewShot = ((uint32_t)shotCount > s_totalShotUI);
  if (isNewShot) {
    s_totalShotUI = shotCount;
    if (shotCount > 0) {
      uint32_t correctBase = ((shotCount - 1) / ROWS_PER_PAGE_UI) * ROWS_PER_PAGE_UI + 1;
      if (correctBase != s_pageBase) {
        s_pageBase = correctBase;
        M5.Lcd.fillRect(0, START_Y, HW::LCD_W, SHOT_END_Y - START_Y + ROW_H, TFT_BLACK);
      }
    }
  }

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  // 繪製目前頁的 rows
  for (uint8_t row = 0; row < ROWS_PER_PAGE_UI; row++) {
    uint32_t shotIdx = s_pageBase - 1 + row;  // 0-based hit index
    int16_t y0 = START_Y + row * ROW_H;
    int16_t yC = y0 + ROW_H / 2;

    M5.Lcd.fillRect(0, y0, HW::LCD_W, ROW_H - 1, TFT_BLACK);

    // 序號
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
    char idxBuf[6]; snprintf(idxBuf, sizeof(idxBuf), "#%02u", (unsigned)(shotIdx + 1));
    M5.Lcd.drawString(idxBuf, 38, yC, GFXFF);

    if (shotIdx < shotCount) {
      // elapsed = timestamp - firstShotAt（相對第一槍）
      unsigned long firstTs = hits[0].timestamp;
      unsigned long elapsed = (shotIdx == 0) ? 0 : (hits[shotIdx].timestamp - firstTs);
      unsigned long split   = hits[shotIdx].split;

      char elBuf[10]; fmtTime(elapsed, elBuf, sizeof(elBuf));
      M5.Lcd.setTextDatum(MR_DATUM);
      M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      char elStr[12]; snprintf(elStr, sizeof(elStr), "%ss", elBuf);
      M5.Lcd.drawString(elStr, 220, yC, GFXFF);

      char spBuf[10]; fmtTime(split, spBuf, sizeof(spBuf));
      M5.Lcd.setTextColor(shotIdx == 0 ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
      char spStr[12];
      if (shotIdx == 0) snprintf(spStr, sizeof(spStr), "%ss", spBuf);
      else              snprintf(spStr, sizeof(spStr), "+%ss", spBuf);
      M5.Lcd.drawString(spStr, 316, yC, GFXFF);
    } else {
      M5.Lcd.setTextDatum(MR_DATUM);
      M5.Lcd.setTextColor(UIColor::CARD_BORD, TFT_BLACK);
      M5.Lcd.drawString("---", 316, yC, GFXFF);
    }
  }

  // 底部摘要列
  M5.Lcd.fillRect(0, STATS_Y, HW::LCD_W, STATS_H, 0x0821);
  M5.Lcd.drawFastHLine(0, STATS_Y, HW::LCD_W, UIColor::CARD_BORD);
  M5.Lcd.setFreeFont(FSS9); M5.Lcd.setTextSize(1);
  char buf[32];
  snprintf(buf, sizeof(buf), "Shots:%u", shotCount);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, 0x0821);
  M5.Lcd.drawString(buf, 6, STATS_Y + STATS_H / 2, GFXFF);
  if (totalMs > 0) {
    char totBuf[10]; fmtTime(totalMs, totBuf, sizeof(totBuf));
    snprintf(buf, sizeof(buf), "Tot:%ss", totBuf);
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, 0x0821);
    M5.Lcd.drawString(buf, HW::LCD_W - 4, STATS_Y + STATS_H / 2, GFXFF);
  }
}
/**
 * @brief 繪製 RO 模式底部資訊列（Draw Time / 命中數 / 總時間）
 *
 * 只更新底部統計區，不清除 shot rows，避免閃爍。
 *
 * @param drawMs    拔槍時間（第一槍 elapsed，ms）
 * @param hitCount  目前命中數
 * @param totalMs   從第一槍到最後一槍的總時間（ms）
 */
void UIScreen::drawROScreen(unsigned long drawMs, uint8_t hitCount,
                            unsigned long totalMs) {
  // ── Draw Time 固定在 game area 底部（Shot Row 上方保留空間）──
  // STATS_Y = IP_Y - 32 = 172，高度 32px，不在 Shot Row 繪製範圍
  constexpr int16_t DY  = Layout::IP_Y - 32;   // 172
  constexpr int16_t DH  = 32;
  constexpr int16_t DCY = DY + DH / 2;
  constexpr int16_t CX  = HW::LCD_W / 2;

  // 背景
  M5.Lcd.fillRect(0, DY, HW::LCD_W, DH, 0x0821);
  M5.Lcd.drawFastHLine(0, DY, HW::LCD_W, UIColor::CARD_BORD);

  // "DRAW TIME" 標籤
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, 0x0821);
  M5.Lcd.drawString("DRAW", 6, DCY, GFXFF);

  // Draw Time 數值
  if (drawMs > 0) {
    char drawBuf[10]; fmtTime(drawMs, drawBuf, sizeof(drawBuf));
    char buf[16]; snprintf(buf, sizeof(buf), "%ss", drawBuf);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_CYAN, 0x0821);
    M5.Lcd.drawString(buf, CX, DCY, GFXFF);
  } else {
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(UIColor::CARD_BORD, 0x0821);
    M5.Lcd.drawString("- - -", CX, DCY, GFXFF);
  }

  // Total（右側）
  if (totalMs > 0) {
    char totBuf[10]; fmtTime(totalMs, totBuf, sizeof(totBuf));
    char buf[16]; snprintf(buf, sizeof(buf), "TOT:%ss", totBuf);
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, 0x0821);
    M5.Lcd.drawString(buf, HW::LCD_W - 6, DCY, GFXFF);
  }

  // IP bar 清除（RO 模式不顯示 IP）
  M5.Lcd.fillRect(0, Layout::IP_Y, HW::LCD_W, Layout::IP_H, TFT_BLACK);
}

/**
 * @brief 繪製 HISTORY 列表畫面（分頁列表 + SD 空間資訊）
 *
 * @param paths      目前頁路徑陣列
 * @param count      目前頁筆數
 * @param total      全部 session 總筆數
 * @param pageStart  目前頁起始索引（用於顯示頁碼）
 * @param freeKB     SD 卡剩餘空間（KB）
 */
void UIScreen::drawHistoryList(const char paths[][Limits::SD_PATH_LEN],
                               uint8_t count, uint16_t total,
                               uint16_t pageStart, uint32_t freeKB) {
  // 可用高度 = GAME_AREA_H - MODE_HDR_H - 頁腳 = 184 - 18 - 18 = 148px
  // 6筆 × 24px = 144px
  constexpr int16_t Y0     = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2;
  constexpr int16_t ITEM_H = 24;
  constexpr int16_t IDX_W  = 28;   // 左側序號欄寬

  M5.Lcd.setFreeFont(FSS9);

  for (uint8_t i = 0; i < count; i++) {
    int16_t iy  = Y0 + i * ITEM_H;
    int16_t icy = iy + ITEM_H / 2;

    // 分隔線
    if (i > 0)
      M5.Lcd.drawFastHLine(0, iy, HW::LCD_W, UIColor::CARD_BORD);

    // 底色
    M5.Lcd.fillRect(0, iy + (i > 0 ? 1 : 0),
                    HW::LCD_W, ITEM_H - (i > 0 ? 1 : 0), TFT_BLACK);

    // 序號 badge（左側）
    char idxBuf[6];
    snprintf(idxBuf, sizeof(idxBuf), "%u", pageStart + i + 1);
    M5.Lcd.fillRoundRect(2, iy + 4, IDX_W, ITEM_H - 8, 3, UIColor::CARD_BG);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(UIColor::TEXT_DIM, UIColor::CARD_BG);
    M5.Lcd.drawString(idxBuf, 2 + IDX_W/2, icy, GFXFF);

    // 檔名（去掉路徑和 .json）
    const char* fn = strrchr(paths[i], '/');
    fn = fn ? fn + 1 : paths[i];
    char disp[32];
    strlcpy(disp, fn, sizeof(disp));
    char* dot = strrchr(disp, '.');
    if (dot) *dot = '\0';

    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.drawString(disp, IDX_W + 6, icy, GFXFF);
  }

  // 頁腳：總數 + 剩餘空間
  int16_t footerY = Y0 + count * ITEM_H + 4;
  M5.Lcd.drawFastHLine(0, Y0 + count * ITEM_H, HW::LCD_W, UIColor::CARD_BORD);
  char footer[32];
  snprintf(footer, sizeof(footer), "Total:%u  Free:%uKB", total, freeKB);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  M5.Lcd.drawString(footer, 4, footerY, GFXFF);
}

/**
 * @brief 繪製 HISTORY 詳情畫面（shot rows + DRILL pass/fail highlight）
 *
 * DRILL mode 且 drillDef != nullptr 時，
 * 超過 par time 的 split 以紅色標記。
 *
 * @param path      session 檔案路徑（用於標題顯示）
 * @param rec       GameRecord（命中資料）
 * @param drillDef  DRILL 課題定義（nullptr = 不做 par 判斷）
 */
void UIScreen::drawHistoryDetail(const char* path, const GameRecord& rec,
                                 const DrillDef* drillDef) {
  // 單欄佈局，使用 s_pageBase 翻頁
  constexpr int16_t START_Y  = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 18;
  constexpr int16_t SHOT_END_Y = Layout::IP_Y - 2;
  constexpr int16_t SHOT_AREA  = SHOT_END_Y - START_Y;
  constexpr int16_t ROW_H      = SHOT_AREA / ROWS_PER_PAGE_UI;

  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextSize(1);

  // 標題行
  constexpr int16_t HDR_Y = Layout::GAME_AREA_Y + Layout::MODE_HDR_H + 2;
  M5.Lcd.fillRect(0, HDR_Y, HW::LCD_W, 16, TFT_BLACK);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(UIColor::TEXT_DIM, TFT_BLACK);
  if (drillDef && drillDef->name[0]) {
    char hdr[32]; char parBuf[10]; fmtTime(drillDef->parTimeMs, parBuf, sizeof(parBuf));
    snprintf(hdr, sizeof(hdr), "%s  par:%ss", drillDef->name, parBuf);
    M5.Lcd.drawString(hdr, 4, HDR_Y, GFXFF);
  } else {
    const char* fn = strrchr(path, '/');
    M5.Lcd.drawString(fn ? fn + 1 : path, 4, HDR_Y, GFXFF);
  }

  uint8_t hits = rec.hit_count < Limits::HIT_MAX ? rec.hit_count : Limits::HIT_MAX;
  if ((uint32_t)hits > s_totalShotUI) s_totalShotUI = hits;

  // Pass/Fail threshold
  uint32_t passThresh = 0;
  if (drillDef && drillDef->parTimeMs > 0 && drillDef->shotCount > 0)
    passThresh = drillDef->parTimeMs / drillDef->shotCount;

  M5.Lcd.fillRect(0, START_Y, HW::LCD_W, SHOT_END_Y - START_Y, TFT_BLACK);

  for (uint8_t row = 0; row < ROWS_PER_PAGE_UI; row++) {
    uint8_t i = (uint8_t)(s_pageBase - 1) + row;  // 0-based hit index
    int16_t y0 = START_Y + row * ROW_H;
    int16_t yC = y0 + ROW_H / 2;

    M5.Lcd.fillRect(0, y0, HW::LCD_W, ROW_H - 1, TFT_BLACK);
    if (i >= hits) break;

    unsigned long elapsed = rec.hits[i].hit_time_ms;
    unsigned long split   = (i == 0) ? elapsed : elapsed - rec.hits[i-1].hit_time_ms;
    bool pass = true;
    if (passThresh > 0 && i > 0) pass = (split <= passThresh);

    uint16_t rowBg = pass ? TFT_BLACK : 0x3000;
    if (!pass) M5.Lcd.fillRect(0, y0, HW::LCD_W, ROW_H - 1, rowBg);

    // 序號
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(pass ? UIColor::TEXT_DIM : 0xF800, rowBg);
    char idxBuf[6]; snprintf(idxBuf, sizeof(idxBuf), "#%02u", i + 1);
    M5.Lcd.drawString(idxBuf, 38, yC, GFXFF);

    // elapsed
    char elBuf[8]; fmtTime(elapsed, elBuf, sizeof(elBuf));
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(TFT_WHITE, rowBg);
    char elStr[12]; snprintf(elStr, sizeof(elStr), "%ss", elBuf);
    M5.Lcd.drawString(elStr, 220, yC, GFXFF);

    // split
    char spBuf[8]; fmtTime(split, spBuf, sizeof(spBuf));
    char spStr[12];
    if (i == 0) snprintf(spStr, sizeof(spStr), "%ss", spBuf);
    else        snprintf(spStr, sizeof(spStr), "+%ss", spBuf);
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(pass ? TFT_CYAN : 0xF800, rowBg);
    M5.Lcd.drawString(spStr, 316, yC, GFXFF);
  }

}