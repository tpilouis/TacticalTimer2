/**
 * @file    hal_audio.cpp
 * @brief   TACTICAL TIMER 2 — I2S 音頻輸出 HAL 實作
 *
 * 封裝 ESP8266Audio 函式庫的 MP3 播放流程，處理 I2S port
 * 與 MicTask 之間的時分複用協調（播放前暫停 Mic，播放後恢復）。
 *
 * 破音改善措施：
 *  1. s_out 持久化，不每次重建（避免 I2S driver 重複 install/uninstall）
 *  2. play() 開始前寫入一幀靜音到 DMA buffer（清除隨機初始值）
 *  3. mp3->begin() 後延遲 5ms，讓 DMA pipeline 穩定
 *  4. OUTPUT_GAIN 調整，避免削波
 *
 * @note ESP8266Audio 的 AudioOutputI2S 析構不會呼叫
 *       i2s_driver_uninstall()，因此播放完成後必須手動呼叫，
 *       否則 HalMic::init() 會得到 ESP_ERR_INVALID_STATE (0x103)。
 *
 * @version 2.0.0
 */

#include "hal_audio.h"
#include <Arduino.h>
#include <driver/i2s.h>


// ============================================================
// [A0] 模組內部狀態（僅此檔案可見）
// ============================================================
static AudioGeneratorMP3*  s_mp3  = nullptr;  ///< MP3 解碼器
static AudioFileSourceSD*  s_file = nullptr;  ///< SD 卡音源
static AudioOutputI2S*     s_out  = nullptr;  ///< I2S 輸出（持久物件）
static AudioFileSourceID3* s_id3  = nullptr;  ///< ID3 標籤剝離層
static bool                s_outReady = false; ///< s_out 已初始化旗標


// ============================================================
// [A0.1] 內部輔助：I2S 靜音預填
// ============================================================

/**
 * @brief 向 I2S DMA buffer 寫入一幀靜音（消除啟動啪聲）
 *
 * 在 mp3->begin() 後、實際解碼開始前呼叫，
 * 確保 DMA 第一個 cycle 輸出的是 0 而非未初始化垃圾。
 */
static void fillSilence() {
  static const int16_t zeros[64] = {};  // 靜態靜音 buffer
  size_t written = 0;
  // 寫入 2 幀靜音，timeout=0 不阻塞
  i2s_write(I2S_NUM_0, zeros, sizeof(zeros), &written, 0);
  i2s_write(I2S_NUM_0, zeros, sizeof(zeros), &written, 0);
}


// ============================================================
// [A1-impl] 播放控制
// ============================================================

/**
 * @brief 開始播放 SD 卡上的 MP3 檔案
 *
 * 流程：
 *  1. 要求 MicTask 暫停並等待 I2S port 釋放
 *  2. 清除上次殘留的 MP3 解碼器 / 音源（s_out 持久保留）
 *  3. 建立 AudioFileSourceSD → AudioFileSourceID3
 *  4. 若 s_out 尚未建立則初始化（首次呼叫）
 *  5. 啟動 AudioGeneratorMP3
 *  6. 靜音預填 + 5ms 穩定延遲
 *
 * @param path  SD 卡絕對路徑，如 "/mp3/7001.mp3"
 * @return true   播放啟動成功
 * @return false  檔案不存在、記憶體不足或 MP3 初始化失敗
 */
bool HalAudio::play(const char* path) {
  TT2_NOT_NULL(path, return false);

  // ── 1. 要求 MicTask 暫停，等待 I2S port 釋放
  IPC::requestMicPause();

  // ── 2. 清除上次殘留的解碼器和音源（保留 s_out）
  if (s_mp3)  { s_mp3->stop(); delete s_mp3;  s_mp3  = nullptr; }
  if (s_id3)  {                delete s_id3;  s_id3  = nullptr; }
  if (s_file) {                delete s_file; s_file = nullptr; }

  // ── 3. 開啟音源檔案
  s_file = new AudioFileSourceSD(path);
  if (!s_file) {
    Serial.printf("[HalAudio] alloc AudioFileSourceSD failed: %s\n", path);
    IPC::releaseMicPause();
    return false;
  }
  if (!s_file->isOpen()) {
    Serial.printf("[HalAudio] File not found: %s\n", path);
    delete s_file; s_file = nullptr;
    IPC::releaseMicPause();
    Panic::warn(TT2Error::AUDIO_FILE_404, path);
    return false;
  }
  s_id3 = new AudioFileSourceID3(s_file);

  // ── 4. s_out 持久化：只在首次建立
  if (!s_outReady) {
    s_out = new AudioOutputI2S(0, 0);  // I2S_NUM_0, internal DAC off
    s_out->SetPinout(HW::I2S_BCLK, HW::I2S_LRCK, HW::I2S_DOUT);
    s_out->SetOutputModeMono(true);
    s_out->SetGain(HW::OUTPUT_GAIN);
    s_outReady = true;
    Serial.println("[HalAudio] AudioOutputI2S created (persistent)");
  }

  // ── 5. 啟動 MP3 解碼器
  s_mp3 = new AudioGeneratorMP3();
  if (!s_mp3->begin(s_id3, s_out)) {
    Serial.printf("[HalAudio] MP3 begin failed: %s\n", path);
    delete s_mp3;  s_mp3  = nullptr;
    delete s_id3;  s_id3  = nullptr;
    delete s_file; s_file = nullptr;
    // s_out 保留，但標記需重建（可能 I2S 狀態異常）
    delete s_out;  s_out  = nullptr; s_outReady = false;
    i2s_driver_uninstall(I2S_NUM_0);
    IPC::releaseMicPause();
    Panic::warn(TT2Error::AUDIO_PLAY_FAIL, path);
    return false;
  }

  // ── 6. 靜音預填 + DMA 穩定延遲（消除啟動啪聲）
  fillSilence();
  vTaskDelay(pdMS_TO_TICKS(5));

  Serial.printf("[HalAudio] Playing: %s\n", path);
  return true;
}

/**
 * @brief 推進 MP3 解碼（每幀呼叫）
 *
 * 在 AudioTask 主迴圈中持續呼叫，驅動 MP3 解碼並輸出到 I2S。
 *
 * @return true   播放仍在進行中
 * @return false  播放已結束或未啟動（呼叫方應呼叫 onPlaybackDone()）
 */
bool HalAudio::loop() {
  if (!s_mp3 || !s_mp3->isRunning()) return false;
  return s_mp3->loop();
}

/**
 * @brief 播放自然結束時的清理（正常結束路徑）
 *
 * 流程：
 *  1. 釋放 MP3 / ID3 / File 物件（s_out 持久保留）
 *  2. 手動呼叫 i2s_driver_uninstall(I2S_NUM_0)
 *  3. 標記 s_outReady = false（下次 play 重建 s_out）
 *  4. 呼叫 IPC::releaseMicPause()，通知 MicTask 恢復採樣
 */
void HalAudio::onPlaybackDone() {
  // ── 1. 停止並釋放解碼器 / 音源
  if (s_mp3)  { s_mp3->stop(); delete s_mp3;  s_mp3  = nullptr; }
  if (s_id3)  {                delete s_id3;  s_id3  = nullptr; }
  if (s_file) {                delete s_file; s_file = nullptr; }

  // ── 2. 釋放 s_out 並 uninstall I2S port
  // ESP8266Audio 已知問題：析構不呼叫 i2s_driver_uninstall()
  if (s_out)  {                delete s_out;  s_out  = nullptr; }
  s_outReady = false;
  i2s_driver_uninstall(I2S_NUM_0);
  Serial.println("[HalAudio] I2S_NUM_0 uninstalled, Mic may reinstall");

  // ── 3. 通知 MicTask 恢復
  IPC::releaseMicPause();
}

/**
 * @brief 強制中止播放（外部中斷路徑）
 *
 * 若目前不在播放中，直接返回。
 * 呼叫 onPlaybackDone() 完成清理，
 * 並向 audioDoneQueue 送出 {ok=false} 通知 GameFSM。
 */
void HalAudio::stop() {
  if (!isPlaying()) return;

  onPlaybackDone();

  // 通知 GameFSM 播放被中途停止
  AudioDone done = { false };
  IPC::sendAudioCmd(AudioCmd{AudioCmd::Op::STOP, {}}, 0);
  xQueueSend(IPC::audioDoneQueue, &done, 0);
}

/**
 * @brief 釋放所有 Audio 物件記憶體（包含 s_out）
 *
 * 完整清理版本，用於異常情況。
 * 正常播放結束請用 onPlaybackDone()。
 */
void HalAudio::release() {
  if (s_mp3)  { s_mp3->stop(); delete s_mp3;  s_mp3  = nullptr; }
  if (s_id3)  {                delete s_id3;  s_id3  = nullptr; }
  if (s_out)  {                delete s_out;  s_out  = nullptr; s_outReady = false; }
  if (s_file) {                delete s_file; s_file = nullptr; }
}


// ============================================================
// [A2-impl] 狀態查詢
// ============================================================

/**
 * @brief 查詢目前是否正在播放音訊
 * @return true   MP3 解碼器存在且 isRunning()
 * @return false  未播放或已結束
 */
bool HalAudio::isPlaying() {
  return s_mp3 && s_mp3->isRunning();
}
