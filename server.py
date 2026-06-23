# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "flask",
# ]
# ///

import os
from flask import Flask, request, jsonify

app = Flask(__name__)
UPLOAD_FOLDER = './uploaded_voices'
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

@app.route('/upload', methods=['POST'])
def upload_file():
    if 'file' not in request.files:
        # バイナリ直接送信の場合のフォールバック
        file_data = request.data
        filename = request.headers.get('X-File-Name', 'voice.wav')
        if file_data:
            filepath = os.path.join(UPLOAD_FOLDER, filename)
            with open(filepath, 'wb') as f:
                f.write(file_data)
            print(f"Received file via Raw Binary: {filename} ({len(file_data)} bytes)")
            return jsonify({"status": "success", "message": "Raw data uploaded"}), 200
        return jsonify({"status": "error", "message": "No file data found"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"status": "error", "message": "No selected file"}), 400

    filepath = os.path.join(UPLOAD_FOLDER, file.filename)
    file.save(filepath)
    print(f"Received file via Multipart: {file.filename} ({os.path.getsize(filepath)} bytes)")
    return jsonify({"status": "success", "message": f"Saved as {file.filename}"}), 200

if __name__ == '__main__':
    # 0.0.0.0 で起動し、LAN内の他のデバイスからの接続を受け付ける
    print("==================================================")
    print(" koe-roc Upload Server is starting...")
    print(f" Access URL: http://<YOUR_PC_IP>:5000/upload")
    print("==================================================")
    app.run(host='0.0.0.0', port=8080)
