/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

// WiFi connection includes
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string.h>

// DPP includes
#include <esp_dpp.h>
#include <esp_timer.h>
#include <qrcode.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <esp_matter_providers.h>

#include <app_priv.h>
#include <app_reset.h>
#include <common_macros.h>
#include <driver/gpio.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <custom_provider/dynamic_commissionable_data_provider.h>

static const char *TAG = "app_main";
uint16_t light_endpoint_id = 0;

// DPP configuration
#ifdef CONFIG_ESP_DPP_LISTEN_CHANNEL_LIST
#define EXAMPLE_DPP_LISTEN_CHANNEL_LIST CONFIG_ESP_DPP_LISTEN_CHANNEL_LIST
#else
#define EXAMPLE_DPP_LISTEN_CHANNEL_LIST "6"
#endif

#ifdef CONFIG_ESP_DPP_BOOTSTRAPPING_KEY
#define EXAMPLE_DPP_BOOTSTRAPPING_KEY CONFIG_ESP_DPP_BOOTSTRAPPING_KEY
#else
#define EXAMPLE_DPP_BOOTSTRAPPING_KEY NULL
#endif

#ifdef CONFIG_ESP_DPP_DEVICE_INFO
#define EXAMPLE_DPP_DEVICE_INFO CONFIG_ESP_DPP_DEVICE_INFO
#else
#define EXAMPLE_DPP_DEVICE_INFO NULL
#endif

#define CURVE_SEC256R1_PKEY_HEX_DIGITS 64

// WiFi configuration - Legacy for fallback
#define WIFI_SSID CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PASS CONFIG_EXAMPLE_WIFI_PASSWORD
#define WIFI_MAXIMUM_RETRY 10

#if CONFIG_DYNAMIC_PASSCODE_COMMISSIONABLE_DATA_PROVIDER
dynamic_commissionable_data_provider g_dynamic_passcode_provider;
#endif

// DPP and WiFi state variables
wifi_config_t s_dpp_wifi_config = {0};
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define DPP_AUTH_FAIL_BIT BIT2
int64_t dpp_start_time = 0;
static bool dpp_initialized = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t ret = esp_supp_dpp_start_listen();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start DPP listen: %s", esp_err_to_name(ret));
            xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
        } else {
            ESP_LOGI(TAG, "Started listening for DPP Authentication");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void dpp_enrollee_event_cb(esp_supp_dpp_event_t event, void *data)
{
    switch (event) {
    case ESP_SUPP_DPP_URI_READY: {
        int64_t uri_ready_time = esp_timer_get_time();
        ESP_LOGI(TAG, "ESP_SUPP_DPP_URI_READY Time: %lld microseconds", uri_ready_time);
        if (data != NULL) {
            esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
            ESP_LOGI(TAG, "DPP URI: %s", (const char *)data);
            ESP_LOGI(TAG, "Scan below QR Code to configure the enrollee:\n");
            esp_qrcode_generate(&cfg, (const char *)data);
        } else {
            ESP_LOGW(TAG, "DPP URI data is NULL");
        }
    } break;
    case ESP_SUPP_DPP_CFG_RECVD: {
        int64_t recv_cfg_result_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Recv DPP Configuration Result Time: %lld microseconds", recv_cfg_result_time);
        if (data != NULL) {
            memcpy(&s_dpp_wifi_config, data, sizeof(s_dpp_wifi_config));
            esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &s_dpp_wifi_config);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
                xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
                return;
            }
            ESP_LOGI(TAG, "DPP Authentication successful, connecting to AP : %s", s_dpp_wifi_config.sta.ssid);
            s_retry_num = 0;
            ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG, "DPP configuration data is NULL");
            xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
        }
    } break;
    case ESP_SUPP_DPP_FAIL: {
        esp_err_t err = (esp_err_t)(intptr_t)data;
        ESP_LOGE(TAG, "DPP Auth failed (Reason: %s), retry count: %d", esp_err_to_name(err), s_retry_num);

        // Add delay between retries to avoid overwhelming the system
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_retry_num < 10) { // Reduce retry count to avoid infinite loops
            esp_err_t ret = esp_supp_dpp_start_listen();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start DPP listen: %s", esp_err_to_name(ret));
                xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
                return;
            }
            s_retry_num++;
        } else {
            ESP_LOGE(TAG, "DPP Authentication failed after %d retries, giving up", s_retry_num);
            xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown DPP event: %d", event);
        break;
    }
}

esp_err_t dpp_enrollee_bootstrap(void)
{
    esp_err_t ret;
    size_t pkey_len = 0;
    char *key = NULL;
    const char *bootstrap_key = NULL;

    ESP_LOGI(TAG, "DPP bootstrap configuration check:");
    ESP_LOGI(TAG, "EXAMPLE_DPP_LISTEN_CHANNEL_LIST: %s", EXAMPLE_DPP_LISTEN_CHANNEL_LIST);
#ifdef CONFIG_ESP_DPP_BOOTSTRAPPING_KEY
    ESP_LOGI(TAG, "CONFIG_ESP_DPP_BOOTSTRAPPING_KEY is defined");
#else
    ESP_LOGI(TAG, "CONFIG_ESP_DPP_BOOTSTRAPPING_KEY is NOT defined");
#endif

    /* Check if EXAMPLE_DPP_BOOTSTRAPPING_KEY is defined and is a valid string */
#ifdef CONFIG_ESP_DPP_BOOTSTRAPPING_KEY
    bootstrap_key = CONFIG_ESP_DPP_BOOTSTRAPPING_KEY;
    if (bootstrap_key != NULL && strlen(bootstrap_key) > 0) {
        pkey_len = strlen(bootstrap_key);
        ESP_LOGI(TAG, "Using configured bootstrapping key, length: %zu", pkey_len);
    } else {
        ESP_LOGI(TAG, "CONFIG_ESP_DPP_BOOTSTRAPPING_KEY is empty");
        pkey_len = 0;
    }
#else
    /* No bootstrapping key configured */
    ESP_LOGI(TAG, "No bootstrapping key configured, DPP will generate random key");
    pkey_len = 0;
#endif

    if (pkey_len && bootstrap_key != NULL) {
        /* Currently only NIST P-256 curve is supported, add prefix/postfix accordingly */
        char prefix[] = "30310201010420";
        char postfix[] = "a00a06082a8648ce3d030107";

        if (pkey_len != CURVE_SEC256R1_PKEY_HEX_DIGITS) {
            ESP_LOGI(TAG, "Invalid key length! Private key needs to be 32 bytes (or 64 hex digits) long");
            return ESP_FAIL;
        }

        key = (char *)malloc(sizeof(prefix) + pkey_len + sizeof(postfix));
        if (!key) {
            ESP_LOGI(TAG, "Failed to allocate for bootstrapping key");
            return ESP_ERR_NO_MEM;
        }
        sprintf(key, "%s%s%s", prefix, bootstrap_key, postfix);
        ESP_LOGI(TAG, "Formatted DER key prepared for DPP bootstrap");
    } else {
        ESP_LOGW(TAG, "No valid bootstrapping key found, QR code will be random each time");
    }

    /* Currently only supported method is QR Code */
    ret = esp_supp_dpp_bootstrap_gen(EXAMPLE_DPP_LISTEN_CHANNEL_LIST, DPP_BOOTSTRAP_QR_CODE, key,
                                     EXAMPLE_DPP_DEVICE_INFO);

    if (key)
        free(key);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate DPP bootstrap: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DPP bootstrap generation successful");
    return ret;
}

static void wifi_init_sta(void)
{
    dpp_start_time = esp_timer_get_time();
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Initialize DPP with better error handling
    if (!dpp_initialized) {
        esp_err_t ret = esp_supp_dpp_init(dpp_enrollee_event_cb);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize DPP: %s", esp_err_to_name(ret));
            xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
            return;
        }
        dpp_initialized = true;

        ret = dpp_enrollee_bootstrap();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to bootstrap DPP: %s", esp_err_to_name(ret));
            esp_supp_dpp_deinit();
            dpp_initialized = false;
            xEventGroupSetBits(s_wifi_event_group, DPP_AUTH_FAIL_BIT);
            return;
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | DPP_AUTH_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", s_dpp_wifi_config.sta.ssid,
                 s_dpp_wifi_config.sta.password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", s_dpp_wifi_config.sta.ssid,
                 s_dpp_wifi_config.sta.password);
    } else if (bits & DPP_AUTH_FAIL_BIT) {
        ESP_LOGI(TAG, "DPP Authentication failed after %d retries", s_retry_num);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    // Clean up DPP resources
    if (dpp_initialized) {
        esp_supp_dpp_deinit();
        dpp_initialized = false;
        ESP_LOGI(TAG, "DPP deinitialized successfully");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler));
    vEventGroupDelete(s_wifi_event_group);

    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "DPP wifi_init_sta execution time: %lld microseconds", end_time - dpp_start_time);
}

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &commissionMgr =
                chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                /* After removing last fabric, this example does not remove the Wi-Fi credentials
                 * and still has IP connectivity so, only advertising on DNS-SD.
                 */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(
                    kTimeoutSeconds, chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    /* Initialize WiFi connection */
    wifi_init_sta();

    /* Initialize driver */
    app_driver_handle_t light_handle = app_driver_light_init();
    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;

    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    /* For M5NanoC6  */
    gpio_reset_pin(GPIO_NUM_19);
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_19, 1);

    extended_color_light::config_t light_config;
    light_config.on_off.on_off = DEFAULT_POWER;
    light_config.on_off.lighting.start_up_on_off = nullptr;
    light_config.level_control.current_level = DEFAULT_BRIGHTNESS;
    light_config.level_control.on_level = DEFAULT_BRIGHTNESS;
    light_config.level_control.lighting.start_up_current_level = DEFAULT_BRIGHTNESS;
    light_config.color_control.color_mode = (uint8_t)ColorControl::ColorMode::kColorTemperature;
    light_config.color_control.enhanced_color_mode = (uint8_t)ColorControl::ColorMode::kColorTemperature;
    light_config.color_control.color_temperature.startup_color_temperature_mireds = nullptr;

    // endpoint handles can be used to add/modify clusters.
    endpoint_t *endpoint = extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, light_handle);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create extended color light endpoint"));

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

    /* Mark deferred persistence for some attributes that might be changed rapidly */
    cluster_t *level_control_cluster = cluster::get(endpoint, LevelControl::Id);
    attribute_t *current_level_attribute =
        attribute::get(level_control_cluster, LevelControl::Attributes::CurrentLevel::Id);
    attribute::set_deferred_persistence(current_level_attribute);

    cluster_t *color_control_cluster = cluster::get(endpoint, ColorControl::Id);
    attribute_t *current_x_attribute = attribute::get(color_control_cluster, ColorControl::Attributes::CurrentX::Id);
    attribute::set_deferred_persistence(current_x_attribute);
    attribute_t *current_y_attribute = attribute::get(color_control_cluster, ColorControl::Attributes::CurrentY::Id);
    attribute::set_deferred_persistence(current_y_attribute);
    attribute_t *color_temp_attribute =
        attribute::get(color_control_cluster, ColorControl::Attributes::ColorTemperatureMireds::Id);
    attribute::set_deferred_persistence(color_temp_attribute);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Enable secondary network interface
    secondary_network_interface::config_t secondary_network_interface_config;
    endpoint = endpoint::secondary_network_interface::create(node, &secondary_network_interface_config,
                                                             ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create secondary network interface endpoint"));
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#if CONFIG_DYNAMIC_PASSCODE_COMMISSIONABLE_DATA_PROVIDER
    /* This should be called before esp_matter::start() */
    esp_matter::set_custom_commissionable_data_provider(&g_dynamic_passcode_provider);

#endif
    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /* Starting driver with default values */
    app_driver_light_set_defaults(light_endpoint_id);

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    // esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif
}
