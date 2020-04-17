/*************************************************
 Network connected RGB led line

 complient with Web Thing API
 
 Krzysztof Zurek
 krzzurek@gmail.com
 Gdansk, Jan 30, 2020
**************************************************/
#include <stdio.h>
#include <sys/param.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_attr.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/apps/sntp.h"

#include "simple_web_thing_server.h"
#include "web_thing_softap.h"
#include "reset_button.h"
#include "web_thing_mdns.h"
#include "thing_rgb_led_line.h"

#include "rgb_color.h"
#include "ws2812b.h"

#define LEDS_MIN 5

//ready/not ready green led indicator
#define GPIO_RGB_DATA			(CONFIG_PIN_NUM_MOSI)
#define GPIO_RGB_DATA_MASK		(1ULL << CONFIG_PIN_NUM_MOSI)

#define ESP_INTR_FLAG_DEFAULT	0

//wifi station configuration data
char mdns_hostname[65];

const int IP4_CONNECTED_BIT = BIT0;
const int IP6_CONNECTED_BIT = BIT1;

static int irq_counter = 0;

//wifi data
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static const char *TAG_WIFI = "wifi station";
static int s_retry_num = 0;

static void chipInfo(void);
//network functions
static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data);
void wifi_init_sta(char *ssid, char *pass);
void init_nvs(void);
static void init_sntp(void);

//other tasks
bool thing_server_loaded = false;
static bool node_is_station = false;
void init_things(void);
void ready_led_fun(void *pvParameter);


/***************************************************************
 *
 * MAIN ENTRY POINT
 *
 * **************************************************************/
void app_main(){
	time_t now;
	struct tm timeinfo;
	bool time_zone_set = false;
	char time_buffer[100];

	// Initialize NVS
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK( err );

	//chip information
	chipInfo();
	//spiInit();
	
	//initialize things, properties etc.
	init_things();

	init_nvs();

	//start here additional non-network tasks
	vTaskDelay(1000 / portTICK_PERIOD_MS);

	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	init_reset_button();
	
	int32_t heap, prev_heap, i = 0;
	heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	prev_heap = heap;
	printf("%i, free heap: %i, irq: %i\n", i, heap, irq_counter);

	while (1) {
		i++;
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		
		if (node_is_station == true){
			//read time
			if (time_zone_set == false){
				//setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
				//without daylight savings
				//setenv("TZ", "CET-1", 1);
				setenv("TZ", "CET", 1);
				tzset();
				time_zone_set = true;
			}

			heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
			if (heap != prev_heap){
				time(&now);
				localtime_r(&now, &timeinfo);
				strftime(time_buffer, sizeof(time_buffer), "%Y/%m/%d %H:%M:%S", &timeinfo);
				printf("%s, free heap: %i, irq: %i\n", time_buffer, heap, irq_counter);
				prev_heap = heap;
			}
		}
	}

	printf("Restarting now.\n");
	fflush(stdout);
	esp_wifi_stop();
	esp_restart();
}


/********************************************************
 *
 * initialization of all things
 *
 * *****************************************************/
void init_things(){
	//build up the thing

	root_node_init();

	//initialize thermometer
	add_thing_to_server(init_rgb_led_line());
}


/***************************************************************
 *
 *
 *
 * **************************************************************/
static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data){ 

	if (event_base == WIFI_EVENT){
		switch(event_id) {
	    	case WIFI_EVENT_STA_START:
	    		//ready to connect with AP
	    		esp_wifi_connect();
	    		break;

    		case WIFI_EVENT_STA_STOP:
	    		ESP_LOGI(TAG_WIFI, "wifi is stopped");
	    		s_retry_num = 0;
	    		break;

    		case WIFI_EVENT_STA_CONNECTED:
	    		/* enable ipv6 */
	    	    tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
	    	    break;

    		case WIFI_EVENT_STA_DISCONNECTED:
	    		//do not give up!
    			vTaskDelay(1000 / portTICK_PERIOD_MS);
	    		esp_wifi_connect();
	    		xEventGroupClearBits(wifi_event_group, IP4_CONNECTED_BIT | IP6_CONNECTED_BIT);
	    		s_retry_num++;
	    		ESP_LOGI(TAG_WIFI,"retry to connect to the AP, %i", s_retry_num);
	    		break;
		    	
	    	default:
    			break;
    	}
    }
	else if (event_base == IP_EVENT){
	    if (event_id == IP_EVENT_STA_GOT_IP){
	    	ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
	    	ESP_LOGI(TAG_WIFI, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
	    	s_retry_num = 0;
	    	xEventGroupSetBits(wifi_event_group, IP4_CONNECTED_BIT);
	    	if (thing_server_loaded == false){
	    		//initialize web thing server
	    		start_web_thing_server(8080, mdns_hostname, MDNS_DOMAIN);
	    		thing_server_loaded = true;
	    		//initialize sntp client
	    		init_sntp();
	    		node_is_station = true;
	    	}
	    }
	    else if (event_id == IP_EVENT_GOT_IP6){
    		xEventGroupSetBits(wifi_event_group, IP6_CONNECTED_BIT);
    	}
    }
}


/**************************************************************
 *
 * wifi initialization
 *
 * ************************************************************/
void wifi_init_sta(char *ssid, char *pass){
	wifi_config_t wifi_config;

    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();
    //ESP_ERROR_CHECK(esp_event_loop_init(event_handler, arg) );
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    
    strcpy((char *)wifi_config.sta.ssid, ssid);
    //printf("wifi ssid: %s\n", wifi_ssid);
    strcpy((char *)wifi_config.sta.password, pass);
    //printf("wifi pass: %s\n", wifi_pass);
    wifi_config.sta.bssid_set = false;
    //wifi_config.sta.channel = 13;
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.threshold.rssi = -100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK){
    	ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");
    	//turn off power savings
    	esp_wifi_set_ps(WIFI_PS_NONE);
    	//TODO: check power savings
    	//esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    else{
    	ESP_LOGI(TAG_WIFI, "connection to AP failed");
    	esp_wifi_deinit();
    }
}


/****************************************************************
 *
 * start NVS on default partition labeled "nvs" (24kB)
 *
 * **************************************************************/
void init_nvs(void){
	esp_err_t err;
	nvs_handle storage_handle = 0;
	
	char wifi_ssid[33];
	char wifi_pass[65];

	// Open
	printf("\n");
	printf("Opening Non-Volatile Storage (NVS) handle... ");

	err = nvs_open("storage", NVS_READWRITE, &storage_handle);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		uint32_t ssid_len = 0, pass_len = 0, mdns_len = 0;

		printf("Done\n");

		// Read
		printf("Reading ssid and password from NVS ... ");
		esp_err_t err1 = nvs_get_str(storage_handle, "ssid", NULL, &ssid_len);
		esp_err_t err2 = nvs_get_str(storage_handle, "pass", NULL, &pass_len);
		esp_err_t err3 = nvs_get_str(storage_handle, "mdns_host", NULL, &mdns_len);
		printf("Done\n");
		printf("errors: %i, %i, %i, \n", err1, err2, err3);

		if ((err1 == ESP_OK) && (err2 == ESP_OK) && (ssid_len > 0) && (pass_len > 0)
				&& (err3 == ESP_OK) && (mdns_len > 0)){
			//password and ssid is defined, connect to network and start
			//web thing server
			if ((ssid_len < 33) && (pass_len < 65) && (mdns_len < 65)){
				nvs_get_str(storage_handle, "ssid", wifi_ssid, &ssid_len);
				nvs_get_str(storage_handle, "pass", wifi_pass, &pass_len);
				nvs_get_str(storage_handle, "mdns_host", mdns_hostname, &mdns_len);
				wifi_ssid[ssid_len - 1] = 0;
				wifi_pass[pass_len - 1] = 0;
				mdns_hostname[mdns_len - 1] = 0;
				vTaskDelay(5 / portTICK_PERIOD_MS);

				//initialize wifi
				wifi_init_sta(wifi_ssid, wifi_pass);
				initialise_mdns(mdns_hostname, false);
			}
			else{
				printf("ssid, password or hostname too long, %i, %i, %i\n",
						ssid_len, pass_len, mdns_len);
			}
		}
		else{
			//ssid, password or node name not defined
			//start AP and server with page for defining these parameters
			wifi_init_softap();
			//initialize mDNS service
			initialise_mdns(NULL, true);
			node_is_station = false;
			//start server
			xTaskCreate(ap_server_task, "ap_server_task", 1024*4, NULL, 1, NULL);
		}
		err = nvs_commit(storage_handle);
		printf((err != ESP_OK) ? "Commit failed!\n" : "Commit done\n");

		// Close
		nvs_close(storage_handle);
	}
	printf("\n");
}


// ************************************************
static void chipInfo(void){
	/* Print chip information */
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
			chip_info.cores,
			(chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
					(chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

	printf("silicon revision %d, ", chip_info.revision);

	printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
			(chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}


static void init_sntp(void)
{
    printf("Initializing SNTP\n");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}
