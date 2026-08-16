# AtomS3 Lite Joystick BLE Keyboard

M5Stack **AtomS3 Lite** と **Unit Joystick v1.1（U024-C）** を、標準的な **Bluetooth Low Energy HIDキーボード**として動作させるESP-IDFプロジェクトです。ジョイスティックを倒すと、接続先のPC、スマートフォン、タブレットへ矢印キーを送ります。

このプロジェクトはESP-IDFのBLE HID Device Demoに含まれるHIDプロファイル実装を取り込み、Joystick UnitのI²C入力をキーボードレポートへ変換します。[1] [2]

## 動作

| Joystick Unitの操作 | 送信するHIDキー |
|---|---|
| 上へ倒す | `↑` |
| 下へ倒す | `↓` |
| 左へ倒す | `←` |
| 右へ倒す | `→` |
| 斜めに倒す | 対応する2方向キーを同時送信 |
| スティックを押し込む（Zボタン） | `Enter` |

入力は25 ms間隔で読み取り、同じ状態が2回連続した時点で反映します。これにより、ニュートラル付近の微小な揺れによる誤入力を抑えます。方向判定の閾値は`main/main.c`内の`AXIS_LOW_THRESHOLD`および`AXIS_HIGH_THRESHOLD`で調整できます。

## 必要なもの

| 品目 | 数量 | 備考 |
|---|---:|---|
| M5Stack AtomS3 Lite | 1 | ESP32-S3搭載モデル |
| M5Stack Unit Joystick v1.1 | 1 | 型番U024-C、I²Cアドレスは`0x52` |
| HY2.0-4P Groveケーブル | 1 | Joystick Unitの付属品を使用可能 |
| USB Type-Cケーブル | 1 | 給電と書き込み用 |
| ESP-IDF | **v5.4.4固定** | ESP32-S3向けツールチェーンを含むもの。バージョンは`IDF_VERSION`にも記載 |

## 配線

AtomS3 LiteのHY2.0-4PポートとJoystick Unitを、Groveケーブルでそのまま接続してください。ファームウェアはAtomS3 Liteの外部I²Cポートとして**SCL = GPIO1、SDA = GPIO2**を使用します。Joystick Unit側のコネクタ色と信号の対応は次の通りです。[2] [3]

| Groveケーブル | 信号 | AtomS3 Lite側 |
|---|---|---|
| 黒 | GND | GND |
| 赤 | 5 V | 5 V |
| 黄 | SDA | GPIO2 |
| 白 | SCL | GPIO1 |

> **注意:** 本プロジェクトは「Unit Joystick v1.1（I²Cアドレス `0x52`）」用です。LED付きの**Unit Joystick2**は別製品のため、そのままでは対象外です。

## ビルドと書き込み

**ESP-IDF v5.4.4** を使用してください。別バージョンで生成済みの`build/`や`sdkconfig`があるとBluetoothのヘッダー構成が混在するため、切替時は必ず削除します。`/dev/ttyACM0`は実際に認識されたシリアルポートへ置き換えてください。

```bash
# ESP-IDF v5.4.4の導入（初回のみ）
git clone --branch v5.4.4 --depth 1 --recursive \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.4.4
cd ~/esp/esp-idf-v5.4.4
./install.sh esp32s3
. ./export.sh

# プロジェクトのクリーンビルド
cd /path/to/atoms3lite-joystick-ble-keyboard
rm -rf build sdkconfig sdkconfig.old
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

起動後、ホスト側のBluetooth設定で **`AtomS3 Joystick KB`** を検索し、キーボードとしてペアリングします。USBケーブルは給電と書き込みにのみ使用し、キー入力はBLEで送信されます。

### `esp_bt.h` が見つからない場合

`fatal error: esp_bt.h: No such file or directory`は、プロジェクトを別のESP-IDF版で構成済みであるか、Bluetoothコンポーネントが無効な状態で生成された`build/`・`sdkconfig`を再利用した場合に発生します。本プロジェクトは**ESP-IDF v5.4.4**を固定対象としています。上記のコマンドどおりにESP-IDF v5.4.4を有効化したうえで、`rm -rf build sdkconfig sdkconfig.old`から必ず実行してください。`main/CMakeLists.txt`ではBluetooth APIを提供する`bt`コンポーネントも明示的に依存関係へ含めています。[1]

## カスタマイズ

デバイス名、I²Cピン、ポーリング周期、方向判定の閾値は`main/main.c`の冒頭にある定数で変更できます。ジョイスティックのZボタンを使わない場合は、`joystick_to_keys()`内の`HID_KEY_ENTER`を追加する部分を削除してください。

## ライセンスと由来

このプロジェクト固有のファイルはMIT Licenseです。`main/esp_hidd_prf_api.*`、`main/hid_dev.*`、`main/hid_device_le_prf.c`、`main/hidd_le_prf_int.h`は、Espressif公式のBLE HID Device Demoから取得したCC0-1.0またはUnlicenseの実装です。元のライセンス表記は各ファイルに保持しています。[1]

## 参照

[1]: https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/ble/ble_hid_device_demo "Espressif BLE HID Device Demo"
[2]: https://docs.m5stack.com/en/unit/joystick_1.1 "M5Stack Unit Joystick v1.1"
[3]: https://github.com/m5stack/M5Unified "M5Unified: AtomS3 Lite I²C pin definition"
