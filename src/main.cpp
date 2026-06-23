#include <M5Unified.h>
#include <LittleFS.h>
#include <FS.h>

// WAVヘッダ構造体
struct __attribute__((packed)) WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;        // 全ファイルサイズ - 8
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtLen = 16;
    uint16_t audioFormat = 1;     // 1 = Linear PCM
    uint16_t numChannels = 1;     // 1 = Mono
    uint32_t sampleRate = 16000;  // 16kHz
    uint32_t byteRate = 32000;     // sampleRate * numChannels * (bitsPerSample/8)
    uint16_t blockAlign = 2;      // numChannels * (bitsPerSample/8)
    uint16_t bitsPerSample = 16;  // 16bit
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataLen = 0;         // 音声データ部のサイズ
};

// 状態定義
enum RecorderState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_PAUSED
};

// 動作パラメータ
const uint32_t SAMPLE_RATE = 16000;
const float SILENCE_THRESHOLD = 300.0f;       // 無音判定の閾値（環境音に合わせて調整）
const unsigned long SILENCE_DURATION_MS = 1500; // 無音検知後の分割トリガー秒数 (1.5秒)
const unsigned long MIN_RECORD_DURATION_MS = 30000; // 最低録音時間 (30秒)
const uint32_t STORAGE_MIN_FREE_BYTES = 102400; // 空き容量警告リミット (100KB)

// グローバル変数
M5Canvas canvas(&M5.Display);
RecorderState currentState = STATE_IDLE;
fs::File recFile;
WavHeader currentWavHeader;
uint32_t writtenAudioBytes = 0;
unsigned long recordStartTimeMs = 0;
unsigned long recordElapsedTimeMs = 0;
unsigned long silenceStartMs = 0;
bool isInSilence = false;
float latestVolume = 0.0f;
char currentFileName[32] = "";
int fileCount = 0;

// マイク入力バッファ
const size_t BUFFER_SIZE = 512;
int16_t micBuffer[BUFFER_SIZE];

// 関数プロトタイプ
bool startNewRecording();
void stopRecording(bool saveFile);
void handleRecordingProcess();
void drawUI();
void updateVolumeMeter(float volume);
void generateFileName(char* outName, size_t maxLength);

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    // シリアル初期化
    Serial.begin(115200);
    delay(100);
    Serial.println("=============================");
    Serial.println(" koe-roc Voice Recorder Starting");
    Serial.println("=============================");

    // 画面初期設定
    M5.Display.setRotation(1); // 横向き
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setTextDatum(top_left);
    M5.Display.clear(TFT_BLACK);

    // 明示的なボタンピンの入力モード設定
    pinMode(39, INPUT_PULLUP);

    // LittleFSの初期化
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed! Formatting...");
        M5.Display.setTextColor(TFT_RED);
        M5.Display.drawString("FS Mount Error!", 10, 10);
        return;
    }
    Serial.printf("LittleFS Storage: Total=%d KB, Used=%d KB\n",
                  LittleFS.totalBytes() / 1024, LittleFS.usedBytes() / 1024);

    // マイクの初期設定
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.stereo = false;
    mic_cfg.magnification = 8; // 感度調整
    mic_cfg.noise_filter_level = 2;
    M5.Mic.config(mic_cfg);

    // RTC初期化（動いていない場合はデフォルト時間にする）
    if (!M5.Rtc.isEnabled()) {
        Serial.println("RTC is not enabled. Initializing default RTC time.");
        // 2026/06/23 13:50:00 に仮設定
        m5::rtc_datetime_t dt;
        dt.date.year = 2026;
        dt.date.month = 6;
        dt.date.date = 23;
        dt.time.hours = 13;
        dt.time.minutes = 50;
        dt.time.seconds = 0;
        M5.Rtc.setDateTime(dt);
    }

    drawUI();
}

void loop() {
    M5.update();

    // 側面ボタンBのクリック数と長押し状態を取得
    int clickCount = M5.BtnB.getClickCount();

    // 1. シングルクリック (1回押し): 録音の開始 / 一時停止
    if (clickCount == 1) {
        Serial.println("BtnB Single Click: Toggle Start/Pause");
        if (currentState == STATE_IDLE) {
            if (startNewRecording()) {
                currentState = STATE_RECORDING;
            }
        } else if (currentState == STATE_RECORDING) {
            M5.Mic.end(); // 一時停止時はマイクからの取得を止める
            currentState = STATE_PAUSED;
            Serial.println("Recording PAUSED");
        } else if (currentState == STATE_PAUSED) {
            M5.Mic.begin(); // 再開
            currentState = STATE_RECORDING;
            Serial.println("Recording RESUMED");
        }
        drawUI();
    }
    // 2. ダブルクリック (2回押し): 手動ファイル分割（切り替え）
    else if (clickCount == 2) {
        Serial.println("BtnB Double Click: Manual File Split");
        if (currentState == STATE_RECORDING || currentState == STATE_PAUSED) {
            stopRecording(true);
            if (startNewRecording()) {
                currentState = STATE_RECORDING;
            } else {
                currentState = STATE_IDLE;
            }
            drawUI();
        }
    }
    
    // 3. 長押し: 録音の完全停止（保存してIDLEへ戻る）
    if (M5.BtnB.wasHold()) {
        Serial.println("BtnB Long Press: Stop Recording");
        if (currentState == STATE_RECORDING || currentState == STATE_PAUSED) {
            stopRecording(true);
            currentState = STATE_IDLE;
            drawUI();
        }
    }

    // 録音処理の実行
    if (currentState == STATE_RECORDING) {
        handleRecordingProcess();
    }

    // 定期的な画面更新（録音中のタイマー等）
    static unsigned long lastUIUpdate = 0;
    if (millis() - lastUIUpdate > 200) {
        lastUIUpdate = millis();
        if (currentState == STATE_RECORDING) {
            recordElapsedTimeMs = millis() - recordStartTimeMs;
        }
        drawUI();
    }

    delay(10);
}


// 新しい録音ファイルを作成して開始する
bool startNewRecording() {
    // 空き容量チェック
    uint32_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeBytes < STORAGE_MIN_FREE_BYTES) {
        Serial.println("Storage space too low to start recording!");
        M5.Display.clear(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.drawString("Storage Full!", 10, 30);
        delay(2000);
        return false;
    }

    // ファイル名生成
    generateFileName(currentFileName, sizeof(currentFileName));
    
    // 新規ファイルオープン
    recFile = LittleFS.open(currentFileName, FILE_WRITE);
    if (!recFile) {
        Serial.println("Failed to open file for writing!");
        return false;
    }

    // ダミーWAVヘッダの書き込み
    writtenAudioBytes = 0;
    currentWavHeader.fileSize = sizeof(WavHeader) - 8;
    currentWavHeader.dataLen = 0;
    recFile.write((uint8_t*)&currentWavHeader, sizeof(WavHeader));

    // 音声入力開始
    M5.Mic.begin();
    recordStartTimeMs = millis();
    recordElapsedTimeMs = 0;
    isInSilence = false;
    latestVolume = 0.0f;

    Serial.printf("Start Recording: %s\n", currentFileName);
    return true;
}

// 現在の録音を停止し、ファイルを閉じる
void stopRecording(bool saveFile) {
    M5.Mic.end();

    if (recFile) {
        if (saveFile && writtenAudioBytes > 0) {
            // WAVヘッダのファイルサイズ情報を更新して先頭に上書き
            currentWavHeader.fileSize = sizeof(WavHeader) + writtenAudioBytes - 8;
            currentWavHeader.dataLen = writtenAudioBytes;
            
            recFile.seek(0);
            recFile.write((uint8_t*)&currentWavHeader, sizeof(WavHeader));
            Serial.printf("Saved %s (%d bytes, approx %.1f sec)\n", 
                          currentFileName, recFile.size(), (float)writtenAudioBytes / 32000.0f);
        } else {
            // 保存しない場合はファイルを閉じて削除
            recFile.close();
            LittleFS.remove(currentFileName);
            Serial.printf("Discarded empty file: %s\n", currentFileName);
            return;
        }
        recFile.close();
    }
}

// 録音データの読み込みとファイルへの書き込み、無音検知ロジック
void handleRecordingProcess() {
    // マイクからデータを読み込む (M5Unifiedは非同期でバッファにためるため、利用可能なら即座に取得できる)
    if (M5.Mic.record(micBuffer, BUFFER_SIZE, SAMPLE_RATE)) {
        // マイクデータの書き込み
        size_t bytesToWrite = BUFFER_SIZE * sizeof(int16_t);
        recFile.write((uint8_t*)micBuffer, bytesToWrite);
        writtenAudioBytes += bytesToWrite;

        // 音量（絶対値平均）の計算
        int32_t amplitudeSum = 0;
        for (size_t i = 0; i < BUFFER_SIZE; i++) {
            amplitudeSum += abs(micBuffer[i]);
        }
        latestVolume = (float)amplitudeSum / BUFFER_SIZE;

        // 音量のデバッグ出力
        // Serial.printf("Vol: %.1f\n", latestVolume);

        // 無音判定
        if (latestVolume < SILENCE_THRESHOLD) {
            if (!isInSilence) {
                silenceStartMs = millis();
                isInSilence = true;
            }
        } else {
            isInSilence = false;
        }

        // ストレージの空き容量確認
        uint32_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (freeBytes < STORAGE_MIN_FREE_BYTES) {
            Serial.println("Storage capacity reached limit. Stopping...");
            stopRecording(true);
            currentState = STATE_IDLE;
            drawUI();
            return;
        }

        // 自動分割条件のチェック：
        // 1. 録音時間が最低録音時間 (30秒) を超えているか
        // 2. 無音状態が規定時間 (1.5秒) 以上継続しているか
        if (recordElapsedTimeMs > MIN_RECORD_DURATION_MS) {
            if (isInSilence && (millis() - silenceStartMs > SILENCE_DURATION_MS)) {
                Serial.println("Silence detected. Auto-splitting file...");
                stopRecording(true);
                if (startNewRecording()) {
                    currentState = STATE_RECORDING;
                } else {
                    currentState = STATE_IDLE;
                }
                drawUI();
            }
        }
    }
}

// 時刻に基づいた一意のファイル名を生成する (例: /rec_135022.wav)
void generateFileName(char* outName, size_t maxLength) {
    auto dt = M5.Rtc.getDateTime();
    snprintf(outName, maxLength, "/rec_%02d%02d%02d.wav", 
             dt.time.hours, dt.time.minutes, dt.time.seconds);
             
    // 同名ファイルが存在する場合は連番を付与
    if (LittleFS.exists(outName)) {
        snprintf(outName, maxLength, "/rec_%02d%02d%02d_%d.wav", 
                 dt.time.hours, dt.time.minutes, dt.time.seconds, ++fileCount);
    }
}

// 画面UIの描画処理
void drawUI() {
    // 1. 上部ステータスバー
    uint32_t statusBgColor = TFT_DARKGRAY;
    const char* statusText = "IDLE";
    
    if (currentState == STATE_RECORDING) {
        statusBgColor = TFT_RED;
        statusText = "REC";
    } else if (currentState == STATE_PAUSED) {
        statusBgColor = TFT_ORANGE;
        statusText = "PAUSE";
    }
    
    canvas.fillRect(0, 0, canvas.width(), 20, statusBgColor);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(1.5);
    canvas.drawString(statusText, 8, 3);

    // バッテリ残量の描画
    int battery = M5.Power.getBatteryLevel();
    canvas.drawRightString(String(battery) + "%", canvas.width() - 8, 3);

    // 2. メイン表示エリア (背景色)
    canvas.fillRect(0, 20, canvas.width(), canvas.height() - 20, 0x101010); // 深いグレー

    // ファイル名の表示
    canvas.setTextColor(TFT_LIGHTGRAY);
    canvas.setTextSize(1);
    if (currentState != STATE_IDLE) {
        canvas.drawString(currentFileName, 8, 26);
    } else {
        canvas.drawString("No Active File", 8, 26);
    }

    // タイマー（録音時間）の描画
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(2);
    char timerStr[16];
    int elapsedSeconds = recordElapsedTimeMs / 1000;
    snprintf(timerStr, sizeof(timerStr), "%02d:%02d", elapsedSeconds / 60, elapsedSeconds % 60);
    canvas.drawString(timerStr, 8, 40);

    // 3. 音量メーターの描画
    updateVolumeMeter(latestVolume);

    // 4. ストレージ情報の描画
    uint32_t totalKB = LittleFS.totalBytes() / 1024;
    uint32_t usedKB = LittleFS.usedBytes() / 1024;
    uint32_t freeKB = totalKB - usedKB;
    float freePercent = (float)freeKB / totalKB * 100.0f;

    canvas.setTextColor(TFT_DARKGRAY);
    canvas.setTextSize(1);
    char storageStr[32];
    snprintf(storageStr, sizeof(storageStr), "Free: %.1fMB (%.0f%%)", (float)freeKB / 1024.0f, freePercent);
    canvas.drawString(storageStr, 8, canvas.height() - 14);

    // 画面に一括反映
    canvas.pushSprite(0, 0);
}

// 音量メーターの描画
void updateVolumeMeter(float volume) {
    int maxBarWidth = canvas.width() - 16;
    // ボリューム値を画面幅にマッピング（上限を設定してスケーリング）
    float normVolume = volume / 1500.0f; // 音量上限1500として正規化
    if (normVolume > 1.0f) normVolume = 1.0f;
    int barWidth = maxBarWidth * normVolume;

    // 音量バーの枠
    canvas.drawRect(8, canvas.height() - 25, maxBarWidth, 6, TFT_DARKGRAY);
    // 音量バーの塗りつぶし（無音閾値を超えたら緑、超えなければ薄青）
    uint32_t barColor = (volume > SILENCE_THRESHOLD) ? TFT_GREEN : TFT_BLUE;
    
    canvas.fillRect(9, canvas.height() - 24, barWidth - 2, 4, barColor);
    // 残りの領域を背景色で消去
    canvas.fillRect(9 + barWidth - 2, canvas.height() - 24, maxBarWidth - barWidth, 4, 0x101010);
}
