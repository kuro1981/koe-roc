#include <M5Unified.h>
#include <LittleFS.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

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

// WiFiおよびサーバー設定
String wifiSsid = "YOUR_WIFI_SSID";
String wifiPass = "YOUR_WIFI_PASSWORD";
String uploadUrl = "http://192.168.1.100:5000/upload";

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

// 新規プロトタイプ
void loadConfig();
void saveConfig(String ssid, String pass, String url);
void handleSaveConfig();
void uploadAllFiles();
void drawGaugeUI(unsigned long pressedMs);
void handleShortPressAction();

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

    // 設定ロード
    loadConfig();

    // WebServer ルートハンドラの設定
    server.on("/", handleRoot);
    server.on("/delete", handleDelete);
    server.on("/save-config", handleSaveConfig);
    server.serveStatic("/", LittleFS, "/");

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

    // 3. ボタンBの押し込み状態追跡と長押しゲージUI
    static bool isBtnBPressed = false;
    static unsigned long btnBPressStartTime = 0;
    static bool btnBActionTriggered = false;

    if (M5.BtnB.isPressed()) {
        if (!isBtnBPressed) {
            isBtnBPressed = true;
            btnBPressStartTime = now;
            btnBActionTriggered = false;
        }

        unsigned long elapsed = now - btnBPressStartTime;

        // 5秒以上押されていてアクション未実行ならAPモードをその場で確定起動
        if (elapsed >= 5000 && !btnBActionTriggered) {
            btnBActionTriggered = true;
            Serial.println("Action: BtnB 5s Long Pressed - Trigger AP Mode");
            
            // 確定のフラッシュフィードバック
            M5.Display.fillScreen(TFT_PURPLE);
            delay(150);

            if (currentState == STATE_WIFI_SERVER) {
                stopWiFiServer();
                currentState = STATE_IDLE;
            } else {
                if (currentState == STATE_RECORDING || currentState == STATE_PAUSED) {
                    stopRecording(true);
                }
                currentState = STATE_WIFI_SERVER;
                startWiFiServer();
            }
            drawUI();
        }

        // 長押し中は毎フレーム専用ゲージUIを描画 (確定後はスキップ)
        if (isBtnBPressed && !btnBActionTriggered) {
            drawGaugeUI(elapsed);
            delay(5);
            return; // 通常のUI更新等はスキップ
        }
    }

    // ボタンBが離されたときの判定
    if (M5.BtnB.wasReleased()) {
        isBtnBPressed = false;
        unsigned long elapsed = now - btnBPressStartTime;

        if (btnBActionTriggered) {
            btnBActionTriggered = false; // 既に5秒長押しで処理済みの場合は何もしない
        } else {
            // 1秒以上5秒未満で離された場合 -> 自動アップロード実行
            if (elapsed >= 1000) {
                Serial.println("Action: BtnB 1s-5s Long Pressed - Trigger Upload");
                if (currentState == STATE_RECORDING || currentState == STATE_PAUSED) {
                    stopRecording(true);
                    currentState = STATE_IDLE;
                }
                uploadAllFiles();
                drawUI();
            }
            // 1秒未満で離された場合 -> 通常の短押し (開始/一時停止/再開/WiFi停止)
            else {
                Serial.println("Action: Short Press Triggered via BtnB");
                handleShortPressAction();
            }
        }
    }

    // 予備ボタン（電源ボタン、前面ボタンA）の短押し判定
    if (M5.BtnPWR.wasPressed() || M5.BtnA.wasPressed()) {
        Serial.println("Action: Short Press Triggered via Fallback Button");
        handleShortPressAction();
    }

    // 5. 画面描画の定期更新 (録音中以外も含む。長押し中以外のみ)
    static unsigned long lastUIUpdate = 0;
    unsigned long uiUpdateInterval = (currentState == STATE_RECORDING) ? 100 : 250;
    if (now - lastUIUpdate > uiUpdateInterval && !isBtnBPressed) {
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

// Webサーバーのメインページ（ファイル一覧とダウンロード、設定フォーム）の配信
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
    html += "h3{margin-top:0;color:#ff9500;}";
    html += "</style></head><body>";
    html += "<h1>koe-roc WAV Files</h1>";
    
    // ストレージ情報の計算
    uint32_t total = LittleFS.totalBytes();
    uint32_t used = LittleFS.usedBytes();
    html += "<div class='info'>";
    html += "Storage: " + String(used / 1024) + " KB / " + String(total / 1024) + " KB used (" + String((total - used) / 1024) + " KB free)<br>";
    html += "</div>";

    // 設定フォームの追加
    html += "<div class='info'>";
    html += "<h3>System Configuration</h3>";
    html += "<form action='/save-config' method='POST'>";
    html += "  <label>WiFi SSID:</label><br>";
    html += "  <input type='text' name='ssid' value='" + wifiSsid + "' style='width:90%;max-width:300px;padding:6px;margin:4px 0;background:#333;color:#fff;border:1px solid #555;border-radius:4px;'><br>";
    html += "  <label>WiFi Password:</label><br>";
    html += "  <input type='password' name='pass' value='" + wifiPass + "' style='width:90%;max-width:300px;padding:6px;margin:4px 0;background:#333;color:#fff;border:1px solid #555;border-radius:4px;'><br>";
    html += "  <label>Upload URL:</label><br>";
    html += "  <input type='text' name='url' value='" + uploadUrl + "' style='width:90%;max-width:400px;padding:6px;margin:4px 0;background:#333;color:#fff;border:1px solid #555;border-radius:4px;'><br>";
    html += "  <input type='submit' value='Save Config' style='background:#34c759;color:#fff;border:none;padding:8px 15px;border-radius:4px;cursor:pointer;margin-top:8px;'>";
    html += "</form>";
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

// === 新規追加関数群 ===

// WiFi設定ロード
void loadConfig() {
    if (!LittleFS.exists("/config.json")) {
        Serial.println("Config file not found. Creating default...");
        saveConfig(wifiSsid, wifiPass, uploadUrl);
        return;
    }

    File configFile = LittleFS.open("/config.json", FILE_READ);
    if (!configFile) {
        Serial.println("Failed to open config file for reading");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        Serial.println("Failed to parse config file. Using defaults.");
        return;
    }

    if (doc["wifi_ssid"].is<String>()) wifiSsid = doc["wifi_ssid"].as<String>();
    if (doc["wifi_pass"].is<String>()) wifiPass = doc["wifi_pass"].as<String>();
    if (doc["upload_url"].is<String>()) uploadUrl = doc["upload_url"].as<String>();

    Serial.println("Config loaded successfully:");
    Serial.printf("  SSID: %s\n", wifiSsid.c_str());
    Serial.printf("  URL: %s\n", uploadUrl.c_str());
}

// WiFi設定保存
void saveConfig(String ssid, String pass, String url) {
    File configFile = LittleFS.open("/config.json", FILE_WRITE);
    if (!configFile) {
        Serial.println("Failed to open config file for writing");
        return;
    }

    JsonDocument doc;
    doc["wifi_ssid"] = ssid;
    doc["wifi_pass"] = pass;
    doc["upload_url"] = url;

    if (serializeJson(doc, configFile) == 0) {
        Serial.println("Failed to write to config file");
    } else {
        Serial.println("Config saved successfully");
    }
    configFile.close();

    wifiSsid = ssid;
    wifiPass = pass;
    uploadUrl = url;
}

// 設定保存用Webサーバーエンドポイント
void handleSaveConfig() {
    if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("url")) {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        String url = server.arg("url");
        saveConfig(ssid, pass, url);
        
        String res = "<html><body style='background:#121212;color:#e0e0e0;font-family:sans-serif;padding:20px;text-align:center;'>";
        res += "<h2>Config Saved Successfully!</h2>";
        res += "<p>SSID: " + ssid + "</p>";
        res += "<a href='/' style='color:#0a84ff;text-decoration:none;'>Back to File List</a>";
        res += "</body></html>";
        server.send(200, "text/html", res);
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

// 画面進捗表示付きのアップロード処理
void uploadAllFiles() {
    M5.Mic.end(); // マイクを確実に停止

    M5.Display.clear(TFT_BLACK);
    M5.Display.setTextSize(1.2);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.drawString("Connecting to WiFi...", 10, 10);
    M5.Display.drawString(wifiSsid.substring(0, 15).c_str(), 10, 25);
    Serial.printf("Connecting to WiFi: %s...\n", wifiSsid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    unsigned long startConnect = millis();
    bool connected = false;
    while (millis() - startConnect < 15000) { // 15秒タイムアウト
        M5.update();
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        int dots = ((millis() - startConnect) / 500) % 4;
        String dotStr = "";
        for (int i = 0; i < dots; i++) dotStr += ".";
        M5.Display.fillRect(10, 45, 100, 15, TFT_BLACK);
        M5.Display.drawString(dotStr.c_str(), 10, 45);
        delay(100);
    }

    if (!connected) {
        Serial.println("WiFi connection failed!");
        M5.Display.setTextColor(TFT_RED);
        M5.Display.drawString("Connection Failed!", 10, 45);
        delay(2000);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    Serial.print("WiFi Connected! IP: ");
    Serial.println(WiFi.localIP());
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString("Connected!", 10, 45);
    
    // IPアドレスを画面に大きく表示
    M5.Display.clear(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Connected to WiFi", 8, 10);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString(WiFi.localIP().toString().c_str(), 8, 30);
    delay(3000); // 確認しやすいように3秒表示

    // ルートディレクトリをスキャンしてWAVファイルを抽出
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    int totalFiles = 0;
    while (file) {
        String name = file.name();
        if (name.endsWith(".wav")) {
            totalFiles++;
        }
        file = root.openNextFile();
    }
    root.close();

    if (totalFiles == 0) {
        M5.Display.clear(TFT_BLACK);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("No files to upload.", 10, 30);
        delay(2000);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    root = LittleFS.open("/");
    file = root.openNextFile();
    int currentIdx = 0;

    while (file) {
        String name = file.name();
        if (name.endsWith(".wav")) {
            currentIdx++;
            String displayName = name;
            if (displayName.startsWith("/")) {
                displayName = displayName.substring(1);
            }

            M5.Display.clear(TFT_BLACK);
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.drawString("Uploading...", 10, 8);
            M5.Display.setTextSize(1);
            M5.Display.drawString(displayName.c_str(), 10, 24);
            
            char progressStr[32];
            snprintf(progressStr, sizeof(progressStr), "File: %d / %d", currentIdx, totalFiles);
            M5.Display.drawString(progressStr, 10, 40);

            Serial.printf("Uploading file %d/%d: %s\n", currentIdx, totalFiles, name.c_str());

            HTTPClient http;
            http.begin(uploadUrl);
            http.setTimeout(15000); // アップロードは少しタイムアウト長めに(15秒)
            http.addHeader("Content-Type", "audio/wav");
            http.addHeader("X-File-Name", displayName);

            File uploadFile = LittleFS.open(name, FILE_READ);
            int httpResponseCode = -1;
            if (uploadFile) {
                httpResponseCode = http.sendRequest("POST", &uploadFile, uploadFile.size());
                uploadFile.close();
            } else {
                Serial.println("Failed to open file for read");
            }

            if (httpResponseCode == 200 || httpResponseCode == 201) {
                Serial.printf("Upload success for %s. Response: %d\n", name.c_str(), httpResponseCode);
                M5.Display.setTextColor(TFT_GREEN);
                M5.Display.drawString("Upload OK!", 10, 56);
                
                // 成功したらファイルをクローズ後に削除
                String fileToDelete = name;
                file.close(); // ディレクトリ内のポインタ維持のため閉じる
                LittleFS.remove(fileToDelete);
                Serial.printf("Deleted local file: %s\n", fileToDelete.c_str());
                
                // ディレクトリポインタを再生成して次を探索
                root.close();
                root = LittleFS.open("/");
                // 現在のインデックス分だけスキップ
                for (int i = 0; i < currentIdx; i++) {
                    file = root.openNextFile();
                }
                continue; // ポインタ引き直し後、ループ継続
            } else {
                Serial.printf("Upload failed for %s. Error: %d\n", name.c_str(), httpResponseCode);
                M5.Display.setTextColor(TFT_RED);
                M5.Display.drawString("Upload Failed!", 10, 56);
                delay(2000);
            }
            http.end();
            delay(500);
        }
        file = root.openNextFile();
    }
    root.close();

    M5.Display.clear(TFT_BLACK);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.drawString("All Done!", 10, 30);
    delay(2000);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// 長押し進行中ゲージUI描画
void drawGaugeUI(unsigned long pressedMs) {
    canvas.fillRect(0, 0, canvas.width(), canvas.height(), 0x101010); // 深いグレー背景

    canvas.setTextColor(TFT_WHITE);
    canvas.setTextSize(1.2);
    canvas.drawString("MODE SELECT", 8, 8);

    // 5秒を最大値としてマッピング
    float progress = (float)pressedMs / 5000.0f;
    if (progress > 1.0f) progress = 1.0f;

    int barMaxWidth = canvas.width() - 32;
    int barWidth = barMaxWidth * progress;

    bool reached1s = (pressedMs >= 1000);
    bool reached5s = (pressedMs >= 5000);

    uint32_t barColor = TFT_BLUE;
    if (reached5s) {
        barColor = 0x780F; // 紫色
    } else if (reached1s) {
        barColor = TFT_GREEN;
    }

    // 枠
    canvas.drawRect(16, 26, barMaxWidth, 12, TFT_DARKGRAY);
    // バー本体
    if (barWidth > 2) {
        canvas.fillRect(17, 27, barWidth - 2, 10, barColor);
    }

    // 操作ガイドメッセージ
    canvas.setTextSize(1);
    if (reached5s) {
        canvas.setTextColor(TFT_MAGENTA);
        canvas.drawString("> RELEASE FOR AP MODE <", 16, 44);
    } else if (reached1s) {
        canvas.setTextColor(TFT_GREEN);
        canvas.drawString("> RELEASE FOR UPLOAD <", 16, 44);
    } else {
        canvas.setTextColor(TFT_LIGHTGRAY);
        canvas.drawString("Keep pressing...", 16, 44);
    }

    canvas.pushSprite(0, 0);
}

// 共通短押し処理アクション
void handleShortPressAction() {
    static unsigned long lastActionTime = 0;
    unsigned long now = millis();
    if (now - lastActionTime > 500) { // 500ms強力デバウンスガード
        lastActionTime = now;
        
        if (currentState == STATE_IDLE) {
            if (startNewRecording()) {
                currentState = STATE_RECORDING;
            }
        } else if (currentState == STATE_RECORDING) {
            M5.Mic.end();
            currentState = STATE_PAUSED;
            Serial.println("Recording Paused");
        } else if (currentState == STATE_PAUSED) {
            recordStartTimeMs = millis() - recordElapsedTimeMs;
            M5.Mic.begin();
            rec_record_idx = 2;
            draw_record_idx = 0;
            M5.Mic.record(&rec_data[0 * record_length], record_length);
            M5.Mic.record(&rec_data[1 * record_length], record_length);
            currentState = STATE_RECORDING;
            Serial.println("Recording Resumed");
        } else if (currentState == STATE_WIFI_SERVER) {
            stopWiFiServer();
            currentState = STATE_IDLE;
        }
        drawUI();
    }
}
