#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_adc_cal.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "mqtt_client.h"


#define WIFI_SSID      "IoT"
#define WIFI_PASS      "[DATA EXPUNGED]"
#define WIFI_MAX_RETRY  10000

#define CONFIG_BROKER_URL "mqtt://192.168.777.7:1883"

#define IRLED GPIO_NUM_12 
#define AUXLED GPIO_NUM_25

// ADC stuff
#define TIMES              256
#define GET_UNIT(x)        ((x>>3) & 0x1)
#define IR_SAMPLES          4

#define ADC_RESULT_BYTE     2
#define ADC_CONV_LIMIT_EN   1                       //For ESP32, this should always be set to 1
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1  //ESP32 only supports ADC1 DMA mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE1

static uint16_t adc1_chan_mask = BIT(6);
static adc_channel_t channel[1] = {ADC_CHANNEL_6};
volatile int ir_state = 0;

esp_mqtt_client_handle_t mqtt_client; 

static const char *  get_my_id(void)
{
    // Use MAC address for Station as unique ID
    static char my_id[7];
	uint8_t hwaddr[6];
    static bool my_id_done = false;
    if (my_id_done)
        return my_id;
        
    esp_read_mac(hwaddr,ESP_MAC_WIFI_STA);
	snprintf(my_id, sizeof(my_id), "%02X%02X%02X", (hwaddr)[3], (hwaddr)[4], (hwaddr)[5]);
    my_id_done = true;
    return my_id;

}


static void mqtt_push(const char *topic, uint32_t value) {
    char s_topic[30];
    char msg[30];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    sprintf(msg,  "%u",value);
    esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);
    //esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);

}

void mqtt_push(const char *topic, unsigned long value) {
    char s_topic[30];
    char msg[30];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    sprintf(msg,  "%lu",value);
    esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);

}
void mqtt_push(const char *topic, signed long value) {
    char s_topic[30];
    char msg[30];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    sprintf(msg,  "%ld",value);
    esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);

}

static void mqtt_push(const char *topic, float value) {
    char s_topic[30];
    char msg[10];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    value = (int64_t)(value*100);
    value /= 100;
    sprintf(msg,"%g",value);
    esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);
    
}

void mqtt_push(const char *topic, char *value) {
    char s_topic[30];
    char msg[10];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    sprintf(msg,"%s",value);
    esp_mqtt_client_publish(mqtt_client, s_topic, msg, 0, 1, 1);
    
}

void mqtt_push(const char *topic, std::string value) {
    char s_topic[30];
    sprintf(s_topic,  "/esp/%s/%s",get_my_id(),topic);
    esp_mqtt_client_publish(mqtt_client, s_topic, value.c_str(), 0, 1, 1);
    
}

void sysinfo_task(void * pvParameters) {
    uint32_t t_now = 0;
    uint32_t sdk_rst_if = 0;
	while(1) {
		t_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        //printf ("%d: sysinfo tick\n",t_now);
		if (t_now < 3000) {
			printf ("%d: sysinfo waiting for init\n",t_now);
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}
        

		// Send heap size
		uint32_t free_heap = esp_get_free_heap_size();
		mqtt_push("sys-heap",free_heap);

		// Send uptime
		mqtt_push("sys-uptime",t_now);

		// Send reset reason
		sdk_rst_if = esp_reset_reason();

		printf("Reboot reason: %u\n",sdk_rst_if);
		mqtt_push("sys-rres",sdk_rst_if);
        
        // RSSI
        wifi_ap_record_t ap;
        esp_wifi_sta_get_ap_info(&ap);
        printf("Link RSSI: %d\n", ap.rssi);
        mqtt_push("sys-rssi",(signed long)ap.rssi);
       
		// Delay 10 times usual delay
		vTaskDelay(pdMS_TO_TICKS(300*1000));
        
	}
}


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG_W = "wifi client";

static int s_retry_num = 0;


static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG_W, "event received: %d",event_id);
    if (event_base == WIFI_EVENT) {
    switch(event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY) {
                //esp_wifi_disconnect();
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG_W, "retry to connect to the AP #%d",s_retry_num);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGI(TAG_W,"connect to the AP fail");
            vTaskDelay(100 / portTICK_PERIOD_MS);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG_W,"Wi-Fi link UP");
            break;
        default:
            //ESP_LOGI(TAG_W, "Other event id:%d", event_id);
            break;
    }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_W, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_log_level_set("wifi", ESP_LOG_INFO); // disable wifi driver logging
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            {.ssid = WIFI_SSID},
            {.password = WIFI_PASS},
        },
    };

    wifi_config_t sc;
    // Get saved wi-fi config, if absent - write new.
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sc));
    if (strcmp(WIFI_SSID,(char*)sc.sta.ssid) != 0) {
            printf("Saved SSID %s does not match, setting config\n",(char*)sc.sta.ssid);
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    }

    ESP_ERROR_CHECK(esp_wifi_start() );
    //tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, "esp32-f");
    ESP_LOGI(TAG_W, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_W, "connected to ap %s",
                 WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_W, "Failed to connect to %s",
                 WIFI_SSID);
    } else {
        ESP_LOGE(TAG_W, "UNEXPECTED EVENT");
    }
}


static void log_error_if_nonzero(const char *message, int error_code)
{    
    static const char *TAG = "mqtt client";
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 */
static void mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    char s_topic[30];
    
    static const char *TAG = "mqtt client";
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        sprintf(s_topic,  "/esp/%s/set/#",get_my_id());
        msg_id = esp_mqtt_client_subscribe(client, s_topic, 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        mqtt_push("online","online");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        esp_mqtt_client_reconnect(client);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        
        //length("/esp/XXXXXX/set/") = 16, and we cut that from the beginning.
        memcpy(s_topic,&event->topic[16],event->topic_len - 16);
        s_topic[event->topic_len - 16] = '\0';
        
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("   TS=|%s|-\r\n",s_topic);
        printf("DATA=%.*s\r\n",event->data_len,event->data);
        break;
    case MQTT_EVENT_ERROR: 
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            vTaskDelay(pdMS_TO_TICKS(1000));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
   // return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    static const char *TAG = "mqtt client";
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb((esp_mqtt_event_handle_t)event_data);
}

static void mqtt_app_start(void)
{
    char s_topic[30];
    sprintf(s_topic,  "/esp/%s/online",get_my_id());
    /* esp_mqtt_client_config_t mqtt_cfg = {
        //.host = "192.168.77.7",
        //.port= 1883,
        .broker.address.uri = "mqtt://192.168.77.7:1883"
        //.lwt_topic = s_topic,
        //.lwt_msg = "offline",
        //.lwt_retain = 1,
       // .keepalive = 30
    };
    */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    
    
}


static void continuous_adc_init(uint16_t adc1_chan_mask, adc_channel_t *channel, uint8_t channel_num)
{
    adc_digi_init_config_t adc_dma_config = {
        .max_store_buf_size = 40960, // 160024
        .conv_num_each_intr = 8, //TIMES,
        .adc1_chan_mask = adc1_chan_mask,
        .adc2_chan_mask = 0,
    };
    ESP_ERROR_CHECK(adc_digi_initialize(&adc_dma_config));

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en = ADC_CONV_LIMIT_EN,
        .conv_limit_num = 64,//255, or 4 to work
        .sample_freq_hz = 40000, // actually 800,000 with this config
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = GET_UNIT(channel[i]);
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_6;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_digi_controller_configure(&dig_cfg));
}

/// @brief Measure ADC value over given timespan
/// @param measure_time Measuring time in microseconds
/// @param max_samples Max samples to take.
/// @return averaged ADC value, float
float adc_read(int32_t measure_time,int32_t max_samples = 100000)

{
    int64_t t_now = 0;
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint64_t total = 0;
    uint8_t result[TIMES] = {0};
    memset(result, 0xcc, TIMES);
    float adc_val = 0;

    //ESP_LOGW("ADC", "ADC read starting");
    //esp_timer_get_time();
    memset(result, 0x00, TIMES);
    adc_digi_start();
    int64_t t_start = esp_timer_get_time();
    
    while((t_now - t_start < measure_time) && total < max_samples) {
        ret = adc_digi_read_bytes(result, TIMES, &ret_num, ADC_MAX_DELAY);
        t_now = esp_timer_get_time();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {

            //ESP_LOGI("TASK:", "ret is %x, ret_num is %d", ret, ret_num);
            for (int i = 0; i < ret_num; i += ADC_RESULT_BYTE) {
                adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
                adc_val += (float)p->type1.data;
            }
            
            total += ret_num;
            
            if (ret_num < TIMES) vTaskDelay(1);
        } else if (ret == ESP_ERR_TIMEOUT) {
            /**
             * ``ESP_ERR_TIMEOUT``: If ADC conversion is not finished until Timeout, you'll get this return error.
             * Here we set Timeout ``portMAX_DELAY``, so you'll never reach this branch.
             */
            ESP_LOGW("ADC", "No data, increase timeout or reduce conv_num_each_intr");
            vTaskDelay(1000);
        }

    }
    adc_digi_stop();
    adc_val = adc_val / total;
    ESP_LOGI("ADC", "Collected value of %10.5f over %llu samples in %lld us",adc_val,total,t_now - t_start);

    return adc_val;

}


void measure_task(void * pvParameters) {


    esp_err_t ret;
    float val_on, val_off,delta,dummy;
   

    continuous_adc_init(adc1_chan_mask, channel, sizeof(channel) / sizeof(adc_channel_t));
    //adc_digi_start();
    

    while(1) {

        val_on = 0;
        val_off = 0;
        for (int i = 0; i < IR_SAMPLES; i++) {
            ESP_LOGI("APP", "on");
            gpio_set_level(IRLED, 1);
            ir_state = 1;
            //vTaskDelay(pdMS_TO_TICKS(10));
            val_on += adc_read(4000,512);
            ESP_LOGI("APP", "off");
            gpio_set_level(IRLED, 0);
            ir_state = 0;
            dummy = adc_read(4000,512);
            vTaskDelay(pdMS_TO_TICKS(120));
            //ets_delay_us(2600);
            val_off += adc_read(4000,512);
        }
        val_off /= IR_SAMPLES;
        val_on /= IR_SAMPLES;
        delta = val_on - val_off;
        if (delta < 0) 
            delta = 0;
        ESP_LOGI("APP","Received values: %8.3f - %8.3f (%8.3f) D: %8.2f",val_on, val_off, delta, dummy);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ret = adc_digi_deinitialize();
    assert(ret == ESP_OK);
	
}


void ct_task(void * pvParameters) {


    esp_err_t ret;
    

    while(1) {


            ESP_LOGI("APP", "on ----------------------------------------------------------------------------------------------------------------");
            gpio_set_level(IRLED, 1);
            ir_state = 1;
            vTaskDelay(pdMS_TO_TICKS(500));

            ESP_LOGI("APP", "off >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
            gpio_set_level(IRLED, 0);
            ir_state = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
    }

	
}


void cm_task(void * pvParameters) {


    esp_err_t ret;
    float dummy;
    int gv;
   

    continuous_adc_init(adc1_chan_mask, channel, sizeof(channel) / sizeof(adc_channel_t));
    //adc_digi_start();
    

    while(1) {

       dummy = adc_read(4000,512);
       ESP_LOGI("APP","                                                                      %d Received value: %10.5f",ir_state,dummy);
       vTaskDelay(pdMS_TO_TICKS(50));
    }
    ret = adc_digi_deinitialize();
    assert(ret == ESP_OK);
	
}


extern "C" void app_main(void)
{

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << IRLED) + (1ULL << AUXLED);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI("app","My ID is: %s",get_my_id());
    wifi_init_sta();
    mqtt_app_start();
    //vTaskDelay(10000 / portTICK_PERIOD_MS);
   // mqtt_push("online",(uint32_t)1);
    //sysinfo_send();
    xTaskCreate(sysinfo_task,"sysinfo_task",4096,NULL,1,NULL);
    xTaskCreate(measure_task,"measure_task",40960,NULL,1,NULL);
    //xTaskCreate(cm_task,"cm_task",20480,NULL,1,NULL);
    //xTaskCreate(ct_task,"ct_task",20480,NULL,1,NULL);
   
    
}
