#include <Arduino.h>
#include <LbmWm1110.hpp>
#include <Lbmx.hpp>

#include <Lbm_packet.hpp>

static TaskHandle_t  BLE_SCAN_Handle;

// Ble_scan_Task
void Ble_Scan_Task(void *parameter) {

	while (true) 
	{
		printf("start scan ibeacon\r\n");
		app_ble_scan_start(); 
		vTaskDelay(5000);
		bool result = false; 
		result = app_ble_get_results( tracker_ble_scan_data, &tracker_ble_scan_len );
		if( result )
		{
			app_ble_display_results( );
		}
		app_ble_scan_stop( );
		printf("stop scan ibeacon\r\n");
		vTaskDelay(180000);
	}
}

void setup() 
{
    printf("\n---------- STARTUP ----------\n");
    app_ble_scan_init();  
    xTaskCreate(Ble_Scan_Task, "Ble_Scan_Task", 256*4, NULL, 3, &BLE_SCAN_Handle);

    // vTaskStartScheduler();
}

void loop() 
{

}
