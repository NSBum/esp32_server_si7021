#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
//	drivers
#include "driver/gpio.h"
#include "driver/i2c.h"

#include "esp_log.h"
#include "tcpip_adapter.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "string.h"
#include <stdlib.h>
#include <stdio.h>
//	si7021 i2c temp/humidity component
#include "si7021.h"

#define I2C_SDA	21	//	GPIO_NUM_21
#define I2C_SCL 22	//	GPIO_NUM_22

//	wifi task flags
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

//	global temp and humidity
float temp,hum;

//	signal LED connected to GPIO4
#define LED_SIGNAL GPIO_NUM_4

//	configuration parameters
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_MAX_STA_CONN       CONFIG_MAX_STA_CONN

//	html header
const static char http_html_hdr[] =
    "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
                   
/*
	The HTML code is split into two parts so that we can
	dynamically construct the temperature and humidity line.
*/                        

//	part A of the static HTML code                             
const static char htmlA[] = "<!DOCTYPE html>"
      "<html>\n"
      "<head>\n"
      "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      "  <title>HELLO ESP32</title>\n"
      "</head>\n"
      "<body>\n"
      "		<h1>Hello World, from ESP32!</h1>\n"
      "		<br />\n";

//	part B of the static HTML code
const static char htmlB[] = "</body>\n"
      "</html>\n";
      
//    event handler for wifi task
static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch(event->event_id) {
		case SYSTEM_EVENT_STA_START:
			esp_wifi_connect();
			break;
		case SYSTEM_EVENT_STA_GOT_IP:
			xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
			printf("got ip\n");
			printf("netmask: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.netmask));
        	printf("gw: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.gw));
        	printf("\n");
        	fflush(stdout);
        	break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
        	esp_wifi_connect();
        	xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        	break;
        default:
        	break;
	}
	return ESP_OK;
}

static void initialise_wifi(void) {
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
	wifi_config_t sta_config = {
		.sta = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS,
			.bssid_set = false
		}
	};
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
	ESP_ERROR_CHECK( esp_wifi_start() );
}

void format_html(char *buffer,float t, float h) {
	char f[100];
	sprintf(f,"\t\t<p>Temp = %0.2f Humidity = %0.2f\n", t, h);
	sprintf(buffer,"%s%s%s",htmlA,f,htmlB);
}

static void http_server_netconn_serve(struct netconn *conn) {
	struct netbuf *inbuf;
  	char *buf;
  	u16_t buflen;
  	err_t err;

  	/* Read the data from the port, blocking if nothing yet there.
   	We assume the request (the part we care about) is in one netbuf */
  	err = netconn_recv(conn, &inbuf);

  	if (err == ERR_OK) {
    	netbuf_data(inbuf, (void**)&buf, &buflen);

    	// strncpy(_mBuffer, buf, buflen);

    	/* Is this an HTTP GET command? (only check the first 5 chars, since
    	there are other formats for GET, and we're keeping it very simple )*/
    	printf("buffer = %s \n", buf);
    	if( buflen >= 5 && strstr(buf,"GET /") != NULL ) {
          	printf("buf[5] = %c\n", buf[5]);
      		/* Send the HTML header
             	* subtract 1 from the size, since we dont send the \0 in the string
             	* NETCONN_NOCOPY: our data is const static, so no need to copy it
       		*/
			char *str = malloc(512);
			if( str ) {
				format_html(str,temp,hum);
			}
			else {
				printf("*** ERROR allocating buffer.\n");
				return;
			}
			
      		netconn_write(conn, http_html_hdr, sizeof(http_html_hdr)-1, NETCONN_NOCOPY);

      		if(buf[5]=='h') {
        		gpio_set_level(LED_SIGNAL, 0);
        		/* Send our HTML page */
        		netconn_write(conn, str, strlen(str), NETCONN_NOCOPY);
      		}
      		else if(buf[5] == 'l') {
        		gpio_set_level(LED_SIGNAL, 1);
       	 		/* Send our HTML page */
       			 netconn_write(conn, str, strlen(str), NETCONN_NOCOPY);
      		}
      		else {
          		netconn_write(conn, str, strlen(str), NETCONN_NOCOPY);
      		}
      		free(str);
   		}

  	}
  	/* Close the connection (server closes in HTTP) */
  	netconn_close(conn);

  	/* Delete the buffer (netconn_recv gives us ownership,
   	so we have to make sure to deallocate the buffer) */
  	netbuf_delete(inbuf);
}

//	http server task
static void http_server(void *pvParameters) {
  	struct netconn *conn, *newconn;
  	err_t err;
  	conn = netconn_new(NETCONN_TCP);
  	netconn_bind(conn, NULL, 80);
  	netconn_listen(conn);
  	do {
     	err = netconn_accept(conn, &newconn);
     	if (err == ERR_OK) {
       		http_server_netconn_serve(newconn);
       		netconn_delete(newconn);
     	}
   	} while(err == ERR_OK);
   	netconn_close(conn);
   	netconn_delete(conn);
}

void query_sensor(void *pvParameter) {
    while(1) {
        temp = si7021_read_temperature();
        hum = si7021_read_humidity();
        
        printf("%0.2f degrees C, %0.2f%% RH\n", temp, hum);
        vTaskDelay(5000 / portTICK_RATE_MS);
    }
}

//	application entry point
int app_main(void) {
	//Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


	initialise_wifi();
		
	//	initialize I2C driver/device
    ret = si7021_init(I2C_NUM_0, I2C_SDA,I2C_SCL,GPIO_PULLUP_DISABLE,GPIO_PULLUP_DISABLE);
    ESP_ERROR_CHECK(ret);
    printf("I2C driver initialized\n");

	// 
	gpio_set_direction(LED_SIGNAL, GPIO_MODE_OUTPUT);
	xTaskCreate(&http_server, "http server", 8000, NULL, 5, NULL);
	xTaskCreate(&query_sensor, "sensor_task", 2048, NULL, 5,NULL);
	return 0;
}
