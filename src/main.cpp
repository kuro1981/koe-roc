#include <M5Unified.h>
#include <LittleFS.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>

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
    STATE_PAUSED,
    STATE_WIFI_SERVER
};

// 動作パラメータ
const uint32_t SAMPLE_RATE = 16000;
const float SILENCE_THRESHOLD = 300.0f;       // 無音判定の閾値（環境音に合わせて調整）
const unsigned long SILENCE_DURATION_MS = 1500; // 無音検知後の分割トリガー秒数 (1.5秒)
const unsigned long MIN_RECORD_DURATION_MS = 30000; // 最低録音時間 (30秒)
const uint32_t STORAGE_MIN_FREE_BYTES = 102400; // 空き容量警告リミット (100KB)

// グローバル変数
M5Canvas canvas(&M5.Display);
WebServer server(80);
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

// マイク入力バッファ (4面バッファ・公式サンプル移植)
const size_t record_number = 4;
const size_t record_length = 1024;
const size_t record_size = record_number * record_length;
int16_t rec_data[record_size];
size_t rec_record_idx = 2;
size_t draw_record_idx = 0;

// 関数プロトタイプ
bool startNewRecording();
void stopRecording(bool saveFile);
void handleRecordingProcess();
void drawUI();
void updateVolumeMeter(float volume);
void generateFileName(char* outName, size_t maxLength);
void startWiFiServer();
void stopWiFiServer();
void handleRoot();
void handleDelete();

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

    // WebServer ルートハンドラの設定
    server.on("/", handleRoot);
    server.on("/delete", handleDelete);
    server.serveStatic("/", LittleFS, "/");

    // 明示的なボタンピンの入力モード設定
    pinMode(35, INPUT_PULLUP);
    pinMode(37, INPUT_PULLUP);
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
    mic_cfg.magnification = 8; // 適正音量に戻す
    mic_cfg.noise_filter_level = 2;
    mic_cfg.dma_buf_len = 256;    // DMAバッファ長を拡張（音飛び防止）
    mic_cfg.dma_buf_count = 16;   // DMAバッファ数を拡張（音飛び防止）
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
    unsigned long now = millis();

    // 1. 録音中の時間更新と録音データ処理
    if (currentState == STATE_RECORDING) {
        recordElapsedTimeMs = now - recordStartTimeMs;
        handleRecordingProcess();
    }

    // 2. WiFiサーバーモード時のクライアント処理
    if (currentState == STATE_WIFI_SERVER) {
        server.handleClient();
    }

    // 3. ボタン入力判定用の共通変数
    static unsigned long lastActionTime = 0;
    static bool btnBLongPressedHandled = false;
    
    bool triggerShortPress = false;
    bool triggerLongPressB = false;

    // --- A. ボタンBの長押し判定 (1秒) ---
    if (M5.BtnB.pressedFor(1000)) {
        if (!btnBLongPressedHandled) {
            btnBLongPressedHandled = true;
            triggerLongPressB = true;
        }
    }

    // --- B. 各種ボタンの短押しトリガー判定 ---
    // 1) ボタンBが離されたとき (長押し処理の後でなければ短押しと判定)
    if (M5.BtnB.wasReleased()) {
        if (btnBLongPressedHandled) {
            btnBLongPressedHandled = false; // 長押し完了後のリリース時はフラグをクリアするだけ
        } else {
            triggerShortPress = true;
        }
    }
    // 2) 電源ボタン(BtnPWR)が押されたとき (予備)
    if (M5.BtnPWR.wasPressed()) {
        triggerShortPress = true;
    }
    // 3) 前面ボタンA(BtnA)が押されたとき (壊れているが念のため残す)
    if (M5.BtnA.wasPressed()) {
        triggerShortPress = true;
    }

    // --- C. アクション実行 (500msガード付) ---
    if (triggerLongPressB) {
        if (now - lastActionTime > 500) {
            lastActionTime = now;
            Serial.println("Action: BtnB Long Pressed");

            if (currentState == STATE_IDLE) {
                // WiFiサーバー起動
                currentState = STATE_WIFI_SERVER;
                startWiFiServer();
            } else if (currentState == STATE_RECORDING || currentState == STATE_PAUSED) {
                // 録音の完全停止（保存）
                stopRecording(true);
                currentState = STATE_IDLE;
            } else if (currentState == STATE_WIFI_SERVER) {
                // WiFiサーバー停止
                stopWiFiServer();
                currentState = STATE_IDLE;
            }
            drawUI();
        }
    }

    if (triggerShortPress) {
        if (now - lastActionTime > 500) { // 500ms強力デバウンスガード
            lastActionTime = now;
            Serial.println("Action: Short Press Triggered");

            if (currentState == STATE_IDLE) {
                // 録音開始
                if (startNewRecording()) {
                    currentState = STATE_RECORDING;
                }
            } else if (currentState == STATE_RECORDING) {
                // 一時停止
                M5.Mic.end();
                currentState = STATE_PAUSED;
                Serial.println("Recording Paused");
            } else if (currentState == STATE_PAUSED) {
                // 再開
                recordStartTimeMs = millis() - recordElapsedTimeMs;
                M5.Mic.begin();
                rec_record_idx = 2;
                draw_record_idx = 0;
                M5.Mic.record(&rec_data[0 * record_length], record_length);
                M5.Mic.record(&rec_data[1 * record_length], record_length);
                currentState = STATE_RECORDING;
                Serial.println("Recording Resumed");
            } else if (currentState == STATE_WIFI_SERVER) {
                // WiFiサーバー停止
                stopWiFiServer();
                currentState = STATE_IDLE;
            }
            drawUI();
        }
    }

    // 5. 画面描画の定期更新 (録音中は細かく、それ以外は適度に更新)
    static unsigned long lastUIUpdate = 0;
    unsigned long uiUpdateInterval = (currentState == STATE_RECORDING) ? 100 : 250;
    if (now - lastUIUpdate > uiUpdateInterval) {
        lastUIUpdate = now;
        drawUI();
    }

    delay(5); // CPU過負荷防止
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
    rec_record_idx = 2;
    draw_record_idx = 0;
    M5.Mic.record(&rec_data[0 * record_length], record_length);
    M5.Mic.record(&rec_data[1 * record_length], record_length);
    
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
    auto data = &rec_data[rec_record_idx * record_length];
    
    // 次のスロットへの録音要求が受け入れられた＝draw_record_idxのバッファの録音は完了している
    if (M5.Mic.record(data, record_length)) {
        auto readyData = &rec_data[draw_record_idx * record_length];
        
        // 録音完了したデータをファイルに書き込む (record_length * sizeof(int16_t) = バイト数)
        size_t bytesToWrite = record_length * sizeof(int16_t);
        recFile.write((uint8_t*)readyData, bytesToWrite);
        writtenAudioBytes += bytesToWrite;

        // 音量（絶対値平均）の計算
        int32_t amplitudeSum = 0;
        for (size_t i = 0; i < record_length; i++) {
            amplitudeSum += abs(readyData[i]);
        }
        latestVolume = (float)amplitudeSum / record_length;

        // 無音判定
        if (latestVolume < SILENCE_THRESHOLD) {
            if (!isInSilence) {
                silenceStartMs = millis();
                isInSilence = true;
            }
        } else {
            isInSilence = false;
        }

        // 描画および録音用のバッファインデックスを更新
        if (++draw_record_idx >= record_number) { draw_record_idx = 0; }
        if (++rec_record_idx >= record_number) { rec_record_idx = 0; }

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
    } else if (currentState == STATE_WIFI_SERVER) {
        statusBgColor = 0x780F; // 紫色
        statusText = "WIFI";
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

    if (currentState == STATE_WIFI_SERVER) {
        // WiFiサーバーモード用画面
        canvas.setTextColor(TFT_GREEN);
        canvas.setTextSize(1);
        canvas.drawString("SSID:  koe-roc", 8, 26);
        canvas.drawString("PASS:  12345678", 8, 38);
        
        canvas.setTextColor(TFT_WHITE);
        canvas.setTextSize(1.2);
        canvas.drawString("http://192.168.4.1/", 8, 54);
        
        canvas.setTextColor(TFT_LIGHTGRAY);
        canvas.setTextSize(1);
        canvas.drawString("Press BtnB to Stop", 8, canvas.height() - 14);
    } else {
        // 通常の録音UI
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
    }

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

// WiFiアクセスポイントとWebサーバーの開始
void startWiFiServer() {
    M5.Mic.end(); // マイクの終了
    WiFi.mode(WIFI_AP);
    WiFi.softAP("koe-roc", "12345678");
    server.begin();
    Serial.println("WiFi AP started. SSID: koe-roc, IP: 192.168.4.1");
}

// WiFiアクセスポイントの停止
void stopWiFiServer() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi AP stopped.");
}

// Webサーバーのメインページ（ファイル一覧とダウンロード）の配信
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>koe-roc Voice Files</title>";
    html += "<style>body{background:#121212;color:#e0e0e0;font-family:sans-serif;margin:0;padding:20px;}";
    html += "h1{color:#ff3b30;border-bottom:1px solid #333;padding-bottom:10px;}";
    html += "table{width:100%;border-collapse:collapse;margin-top:20px;}";
    html += "th,td{padding:12px;text-align:left;border-bottom:1px solid #222;}";
    html += "th{background:#1e1e1e;}";
    html += "a{color:#0a84ff;text-decoration:none;}a:hover{text-decoration:underline;}";
    html += ".btn-del{color:#ff453a;margin-left:15px;}";
    html += ".info{background:#1c1c1e;padding:15px;border-radius:8px;margin-bottom:20px;}";
    html += "</style></head><body>";
    html += "<h1>koe-roc WAV Files</h1>";
    
    // ストレージ情報の計算
    uint32_t total = LittleFS.totalBytes();
    uint32_t used = LittleFS.usedBytes();
    html += "<div class='info'>";
    html += "Storage: " + String(used / 1024) + " KB / " + String(total / 1024) + " KB used (" + String((total - used) / 1024) + " KB free)<br>";
    html += "</div>";

    html += "<table><tr><th>File Name</th><th>Size</th><th>Actions</th></tr>";

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    int count = 0;
    while (file) {
        String name = file.name();
        // LittleFSのルートファイル名には先頭に '/' が含まれる場合があるため整形
        String displayName = name;
        if (displayName.startsWith("/")) {
            displayName = displayName.substring(1);
        }
        
        if (name.endsWith(".wav")) {
            count++;
            uint32_t size = file.size();
            html += "<tr>";
            html += "<td><a href='/" + displayName + "' download>" + displayName + "</a></td>";
            html += "<td>" + String(size / 1024.0f, 1) + " KB</td>";
            html += "<td>";
            html += "<a href='/" + displayName + "' download>Download</a>";
            html += "<a class='btn-del' href='/delete?file=" + name + "' onclick=\"return confirm('Delete this file?');\">Delete</a>";
            html += "</td>";
            html += "</tr>";
        }
        file = root.openNextFile();
    }
    if (count == 0) {
        html += "<tr><td colspan='3' style='text-align:center;color:#888;'>No recording files found.</td></tr>";
    }
    html += "</table></body></html>";
    server.send(200, "text/html", html);
}

// ファイルの削除処理
void handleDelete() {
    if (server.hasArg("file")) {
        String filename = server.arg("file");
        // 先頭に '/' がない場合は強制付与 (LittleFSの要件)
        if (!filename.startsWith("/")) {
            filename = "/" + filename;
        }
        
        Serial.printf("Request to delete file: %s\n", filename.c_str());
        
        if (LittleFS.exists(filename)) {
            if (LittleFS.remove(filename)) {
                Serial.printf("Successfully deleted file: %s\n", filename.c_str());
            } else {
                Serial.printf("Failed to delete file (remove returned false): %s\n", filename.c_str());
            }
        } else {
            Serial.printf("Delete failed: File does not exist: %s\n", filename.c_str());
        }
    }
    // ルートページへリダイレクト
    server.sendHeader("Location", "/");
    server.send(303);
}
