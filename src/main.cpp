#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// === НАСТРОЙКИ BLE ДЛЯ XBOX ===
static BLEUUID hidServiceUUID((uint16_t)0x1812);
static BLEUUID reportCharUUID((uint16_t)0x2A4D);

static boolean doConnect = false;
static boolean connected = false;
static BLEAdvertisedDevice* myDevice;
static BLERemoteCharacteristic* pRemoteCharacteristic;

// Глобальные переменные для обмена данными между BLE и логикой Dendy
uint8_t globalXboxData[20] = { 0 };
bool hasXboxData = false;

// === НАСТРОЙКИ ЭМУЛЯЦИИ DENDY ===
const int dendyDataPin = 7;  // Выход Data в сторону приставки Dendy (GPIO 7)
const int dendyLatchPin = 5; // Вход Latch (Strobe) от приставки Dendy
const int dendyClockPin = 6; // Вход Clock (Pulse) от приставки Dendy

// Буфер, который мгновенно читается внутри прерываний
volatile uint8_t dendyRegisterBuffer = 0xFF;
volatile int bitIndex = 0;

// Аппаратный флаг для Турбо-кнопок
volatile bool turboState = false;

// Индексы бит для кнопок NES/Dendy
enum DendyButtons {
    BTN_A = 0,
    BTN_B = 1,
    BTN_SELECT = 2,
    BTN_START = 3,
    BTN_UP = 4,
    BTN_DOWN = 5,
    BTN_LEFT = 6,
    BTN_RIGHT = 7
};

// === ИСПРАВЛЕННЫЕ ПРЕРЫВАНИЯ С ПРЯМОЙ ЗАПИСЬЮ В РЕГИСТРЫ ESP32-S3 ===
// GPIO 0-31 на ESP32-S3 управляются через регистры GPIO_OUT_W1TS_REG (Set) и GPIO_OUT_W1TC_REG (Clear)

void IRAM_ATTR onLatchInterrupt() {
    bitIndex = 0; // Сброс индекса

    // Мгновенно выставляем состояние первой кнопки (A) на пин 7 через регистры
    if (dendyRegisterBuffer & (1 << 0)) {
        REG_WRITE(GPIO_OUT_W1TS_REG, (1 << dendyDataPin)); // Установить HIGH
    }
    else {
        REG_WRITE(GPIO_OUT_W1TC_REG, (1 << dendyDataPin)); // Установить LOW
    }
}

void IRAM_ATTR onClockInterrupt() {
    // Безопасное инкрементирование для обхода варнинга компилятора
    int localIndex = bitIndex + 1;
    bitIndex = localIndex;

    if (localIndex < 8) {
        // Выставляем последующие биты по очереди через быстрые регистры
        if (dendyRegisterBuffer & (1 << localIndex)) {
            REG_WRITE(GPIO_OUT_W1TS_REG, (1 << dendyDataPin)); // Установить HIGH
        }
        else {
            REG_WRITE(GPIO_OUT_W1TC_REG, (1 << dendyDataPin)); // Установить LOW
        }
    }
    else {
        // Для всех бит дальше 8-го шлем единицу (HIGH)
        REG_WRITE(GPIO_OUT_W1TS_REG, (1 << dendyDataPin));
    }
}

// Прототип функции, так как она вызывается в notifyCallback
void updateNesState();

// === ОБРАБОТКА ВХОДЯЩИХ ДАННЫХ BLE ===
static void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify) {

    size_t bytesToCopy = (length > 20) ? 20 : length;
    memcpy(globalXboxData, pData, bytesToCopy);
    hasXboxData = true;

    // Мгновенно обновляем состояние буфера при получении пакета
    updateNesState();
}

// === ФУНКЦИЯ ОБРАБОТКИ И КАЛИБРОВКИ ВВОДА ===
void updateNesState() {
    if (!hasXboxData)
        return;

    static uint8_t lastNesState = 0xFF;

    // Сборка осей левого стика
    uint16_t joyXVal = globalXboxData[0] | (globalXboxData[1] << 8);
    uint16_t joyYVal = globalXboxData[2] | (globalXboxData[3] << 8);
    const uint16_t centerPos = 32768;
    const uint16_t threshold = 20000;

    bool currentLUp = (joyYVal < (centerPos - threshold));
    bool currentLDown = (joyYVal > (centerPos + threshold));
    bool currentLLeft = (joyXVal < (centerPos - threshold));
    bool currentLRight = (joyXVal > (centerPos + threshold));

    // Считывание кнопок из пакета Xbox
    uint8_t dpadState = globalXboxData[12] & 0x0F;
    uint8_t btnByte13 = globalXboxData[13];
    uint8_t btnByte14 = globalXboxData[14];

    uint8_t currentNesState = 0xFF; // По умолчанию все отпущено (0xFF)

    // --- ОБЫЧНЫЕ КНОПКИ ---
    if (btnByte13 & 0x08)
        currentNesState &= ~(1 << BTN_B); // Xbox [X] -> NES [B]
    if (btnByte13 & 0x01)
        currentNesState &= ~(1 << BTN_A); // Xbox [A] -> NES [A]

    // --- ТУРБО КНОПКИ ---
    if ((btnByte13 & 0x10) && turboState)
        currentNesState &= ~(1 << BTN_B); // Xbox [Y] -> Turbo B
    if ((btnByte13 & 0x02) && turboState)
        currentNesState &= ~(1 << BTN_A); // Xbox [B] -> Turbo A

    // --- СИСТЕМНЫЕ КНОПКИ ---
    if (btnByte14 & 0x04)
        currentNesState &= ~(1 << BTN_SELECT); // NES [SELECT]
    if (btnByte14 & 0x08)
        currentNesState &= ~(1 << BTN_START); // NES [START]

    // --- НАПРАВЛЕНИЯ (D-PAD + СТИК) ---
    bool moveUp = (dpadState == 1 || dpadState == 2 || dpadState == 8 || currentLUp);
    bool moveDown = (dpadState == 4 || dpadState == 5 || dpadState == 6 || currentLDown);
    bool moveLeft = (dpadState == 6 || dpadState == 7 || dpadState == 8 || currentLLeft);
    bool moveRight = (dpadState == 2 || dpadState == 3 || dpadState == 4 || currentLRight);

    if (moveUp)
        currentNesState &= ~(1 << BTN_UP);
    if (moveDown)
        currentNesState &= ~(1 << BTN_DOWN);
    if (moveLeft)
        currentNesState &= ~(1 << BTN_LEFT);
    if (moveRight)
        currentNesState &= ~(1 << BTN_RIGHT);

    // Обновляем буфер для прерываний приставки
    dendyRegisterBuffer = currentNesState;

    // Вывод логов только при изменении состояния
    if (currentNesState != lastNesState) {
        lastNesState = currentNesState;
        if (currentNesState != 0xFF) {
            Serial.print("[XBOX -> ДЕНДИ] Нажато: ");
            if (!(currentNesState & (1 << BTN_UP)))
                Serial.print("ВВЕРХ ");
            if (!(currentNesState & (1 << BTN_DOWN)))
                Serial.print("ВНИЗ ");
            if (!(currentNesState & (1 << BTN_LEFT)))
                Serial.print("ВЛЕВО ");
            if (!(currentNesState & (1 << BTN_RIGHT)))
                Serial.print("ВПРАВО ");
            if (!(currentNesState & (1 << BTN_A)))
                Serial.print("[A] ");
            if (!(currentNesState & (1 << BTN_B)))
                Serial.print("[B] ");
            if (!(currentNesState & (1 << BTN_SELECT)))
                Serial.print("[SELECT] ");
            if (!(currentNesState & (1 << BTN_START)))
                Serial.print("[START] ");
            Serial.println();
        }
        else {
            Serial.println("[XBOX -> ДЕНДИ] Все кнопки отпущены");
        }
    }
}

// === БЕЗОПАСНОСТЬ И СОПРЯЖЕНИЕ BLE ===
class MySecurity : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onConfirmPIN(uint32_t pin) override { return true; }
    bool onSecurityRequest() override { return true; }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (desc->sec_state.encrypted) {
            Serial.println("Шифрование и сопряжение с Xbox успешно завершено!");
        }
        else {
            Serial.println("Ошибка: соединение установлено, но НЕ зашифровано.");
        }
    }
};

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(hidServiceUUID)) {
            Serial.print("Найден Xbox/HID контроллер: ");
            Serial.println(advertisedDevice.toString().c_str());
            BLEDevice::getScan()->stop();
            myDevice = new BLEAdvertisedDevice(advertisedDevice);
            doConnect = true;
        }
    }
};

bool connectToServer() {
    Serial.print("Подключение к ");
    Serial.println(myDevice->getAddress().toString().c_str());

    BLEClient* pClient = BLEDevice::createClient();
    if (!pClient->connect(myDevice)) {
        Serial.println("Не удалось установить физическое соединение.");
        return false;
    }
    Serial.println(" - Соединение установлено");

    BLERemoteService* pRemoteService = pClient->getService(hidServiceUUID);
    if (pRemoteService == nullptr) {
        Serial.println("Не найден сервис HID.");
        pClient->disconnect();
        return false;
    }

    pRemoteCharacteristic = pRemoteService->getCharacteristic(reportCharUUID);
    if (pRemoteCharacteristic == nullptr) {
        Serial.println("Не найдена характеристика репортов");
        pClient->disconnect();
        return false;
    }

    if (pRemoteCharacteristic->canNotify()) {
        pRemoteCharacteristic->registerForNotify(notifyCallback);
    }

    connected = true;
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("Запуск BLE хоста и высокоскоростного эмулятора Dendy...");

    // Инициализация физических пинов Dendy
    pinMode(dendyLatchPin, INPUT);
    pinMode(dendyClockPin, INPUT);
    pinMode(dendyDataPin, OUTPUT);
    digitalWrite(dendyDataPin, HIGH);

    // Привязка прерываний
    attachInterrupt(digitalPinToInterrupt(dendyLatchPin), onLatchInterrupt, RISING);
    attachInterrupt(digitalPinToInterrupt(dendyClockPin), onClockInterrupt, RISING);

    // Инициализация BLE хоста
    BLEDevice::init("ESP32-S3-GamepadHost");

    BLESecurity security;
    security.setAuthenticationMode(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM | BLE_SM_PAIR_AUTHREQ_SC);
    security.setCapability(BLE_HS_IO_NO_INPUT_OUTPUT);

    BLEDevice::setSecurityCallbacks(new MySecurity());

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setInterval(1349);
    pBLEScan->setWindow(449);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(10, false);
}

void loop() {
    if (doConnect == true) {
        if (connectToServer()) {
            Serial.println("Геймпад готов. Нажимайте кнопки.");
        }
        else {
            Serial.println("Не удалось настроить геймпад.");
        }
        doConnect = false;
    }

    // Независимый генератор частоты для Турбо-кнопок
    static unsigned long lastTurboTime = 0;
    if (millis() - lastTurboTime >= 30) {
        turboState = !turboState;
        lastTurboTime = millis();

        if (hasXboxData) {
            updateNesState();
        }
    }
}