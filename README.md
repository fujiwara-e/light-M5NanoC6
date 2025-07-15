# light-M5NanoC6

[esp-matter のサンプルプロジェクト](https://github.com/fujiwara-e/esp-matter/tree/main/examples/light)を参考に，menuconfig から Wi-Fi 接続情報，Matter の SetupPIN コードを設定できるよう拡張したプロジェクトである．

    
## 使い方
1. [esp-idf](https://github.com/espressif/esp-idf)をセットアップする．
2. チップターゲットを設定する

    ```bash
    idf.py set-target esp32c6
    
    ```
3. プロジェクト構成メニューを開く

    ```bash
    idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.m5stack_c6_nano" menuconfig
        ```
        `Example Configuration --> WiFi SSID`
    
        `Example Configuration --> WiFi Password`にて WiFi に関する設定を行う．
        
        `Dynamic Passcode Configuration` --> Setup Passcode in Dynamic Passcode Commissionable Data Provider --> 8桁の Setup pincode を設定する． (11111111 や 12345678) はダメ
        
        ``
4. プロジェクトをビルドし，書き込む
    ```bash
    idf.py build 
    idf.py flash
    ```
