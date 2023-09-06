#include <Arduino.h>

#include <LbmWm1110.hpp>
#include <Lbmx.hpp>

#include <Lbm_packet.hpp>

//taskhandle

static TaskHandle_t  LORAWAN_ENGINE_Handle;
static TaskHandle_t  LORAWAN_TX_Handle;
static TaskHandle_t  BLE_SCAN_Handle;



void app_ble_scan_task_wakeup( void );
void app_lora_engine_task_wakeup( void );
void app_Lora_tx_task_wakeup( void );


/* most important thing:
// Please follow local regulations to set lorawan duty cycle limitations
// smtc_modem_set_region_duty_cycle()
//
//
*/

////////////////////////////////////////////////////////////////////////////////
// Types

enum class StateType
{
    Startup,
    Joining,
    Joined,
    Failed,
};

////////////////////////////////////////////////////////////////////////////////
// Constants

static constexpr smtc_modem_region_t REGION = SMTC_MODEM_REGION_EU_868;

static constexpr uint32_t TIME_SYNC_VALID_TIME = 60 * 60 * 24;  // [sec.] 

////////////////////////////////////////////////////////////////////////////////
// Variables

static LbmWm1110& lbmWm1110 = LbmWm1110::getInstance();
static StateType state = StateType::Startup;

uint8_t DEV_EUI[8];
uint8_t JOIN_EUI[8];
uint8_t APP_KEY[16];

////////////////////////////////////////////////////////////////////////////////
void init_current_lorawan_param(void)
{
    memcpy(DEV_EUI,app_param.lora_info.DevEui,8);
    memcpy(JOIN_EUI,app_param.lora_info.JoinEui,8);
    memcpy(APP_KEY,app_param.lora_info.AppKey,16);
}

// MyLbmxEventHandlers
class MyLbmxEventHandlers : public LbmxEventHandlers
{
protected:
    void reset(const LbmxEvent& event) override;
    void joined(const LbmxEvent& event) override;
    void joinFail(const LbmxEvent& event) override;
    void time(const LbmxEvent& event) override;
    void almanacUpdate(const LbmxEvent& event) override;  
    void txDone(const LbmxEvent& event);      
};


void MyLbmxEventHandlers::reset(const LbmxEvent& event)
{
    if (LbmxEngine::setRegion(REGION) != SMTC_MODEM_RC_OK) abort();
    if (LbmxEngine::setOTAA(DEV_EUI, JOIN_EUI, APP_KEY) != SMTC_MODEM_RC_OK) abort();

    if (smtc_modem_dm_set_info_interval(SMTC_MODEM_DM_INFO_INTERVAL_IN_DAY, 1) != SMTC_MODEM_RC_OK) abort();
    {
        const uint8_t infoField = SMTC_MODEM_DM_FIELD_ALMANAC_STATUS;
        if (smtc_modem_dm_set_info_fields(&infoField, 1) != SMTC_MODEM_RC_OK) abort();
    }

    printf("Join the LoRaWAN network.\n");
    if (LbmxEngine::joinNetwork() != SMTC_MODEM_RC_OK) abort();

    if((REGION == SMTC_MODEM_REGION_EU_868) || (REGION == SMTC_MODEM_REGION_RU_864))
    {
        smtc_modem_set_region_duty_cycle( false );
    }

    state = StateType::Joining;
}

void MyLbmxEventHandlers::joined(const LbmxEvent& event)
{
    state = StateType::Joined;

    //Configure ADR, It is necessary to set up ADR,Tx useable payload must large than 51 bytes
    app_set_profile_list_by_region(REGION,adr_custom_list_region);
    if (smtc_modem_adr_set_profile(0, SMTC_MODEM_ADR_PROFILE_CUSTOM, adr_custom_list_region) != SMTC_MODEM_RC_OK) abort();              //adr_custom_list_region  CUSTOM_ADR    

    if (smtc_modem_time_set_sync_interval_s(TIME_SYNC_VALID_TIME / 3) != SMTC_MODEM_RC_OK) abort();     // keep call order
    if (smtc_modem_time_set_sync_invalid_delay_s(TIME_SYNC_VALID_TIME) != SMTC_MODEM_RC_OK) abort();    // keep call order

    printf("Start time sync.\n");
    if (smtc_modem_time_start_sync_service(0, SMTC_MODEM_TIME_ALC_SYNC) != SMTC_MODEM_RC_OK) abort();

    app_Lora_tx_task_wakeup( );
}

void MyLbmxEventHandlers::joinFail(const LbmxEvent& event)
{
    state = StateType::Failed;
}

void MyLbmxEventHandlers::time(const LbmxEvent& event)
{
    if (event.event_data.time.status == SMTC_MODEM_EVENT_TIME_NOT_VALID) return;

    static bool first = true;
    if (first)
    {
        printf("time sync ok\r\n");
        if( is_first_time_sync == false )
        {
            is_first_time_sync = true;
            app_ble_scan_task_wakeup();
        }
        // Configure transmissions
        if (smtc_modem_set_nb_trans(0, 1) != SMTC_MODEM_RC_OK) abort();
        if (smtc_modem_connection_timeout_set_thresholds(0, 0, 0) != SMTC_MODEM_RC_OK) abort();

        first = false;
    }
}

void MyLbmxEventHandlers::almanacUpdate(const LbmxEvent& event)
{
    if( event.event_data.almanac_update.status == SMTC_MODEM_EVENT_ALMANAC_UPDATE_STATUS_REQUESTED )
    {
        printf( "Almanac update is not completed: sending new request\n" );
    }
    else
    {
        printf( "Almanac update is completed\n" );
    }
}
void MyLbmxEventHandlers::txDone(const LbmxEvent& event)
{
    static uint32_t uplink_count = 0;
    uint32_t confirmed_count = 0;
    if( event.event_data.txdone.status == SMTC_MODEM_EVENT_TXDONE_CONFIRMED )
    {
        app_lora_confirmed_count_increment();
    }
    uint32_t tick = smtc_modem_hal_get_time_in_ms( );
    confirmed_count = app_lora_get_confirmed_count();
    printf( "LoRa tx done at %u, %u, %u\r\n", tick, ++uplink_count, confirmed_count );    
}

////////////////////////////////////////////////////////////////////////////////
// ModemEventHandler

static void ModemEventHandler()
{
    static LbmxEvent event;
    static MyLbmxEventHandlers handlers;

    while (event.fetch())
    {
        printf("----- %s -----\n", event.getEventString().c_str());
        handlers.invoke(event);
    }
}
//---------------Task---------------- ---------------
void app_ble_scan_task_wakeup( void )
{
    xTaskNotifyGive( BLE_SCAN_Handle );
}

void app_Lora_tx_task_wakeup( void )
{
    xTaskNotifyGive( LORAWAN_TX_Handle );
}
void app_Lora_engine_task_wakeup( void )
{
    xTaskNotifyGive( LORAWAN_ENGINE_Handle );
}

// LoraWan_Engine_Task
void LoraWan_Engine_Task(void *parameter) {
    while (true) 
    {
        uint32_t sleepTime = LbmxEngine::doWork();
        ( void )ulTaskNotifyTake( pdTRUE, sleepTime );
    }
}

// Ble_scan_Task
void Ble_Scan_Task(void *parameter) {

    while (true) 
    {
        if(is_first_time_sync == false)
        {
            ( void )ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
        }
        while(1)
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
            sensor_datas_get();
            //send data to LoRaWAN
            app_task_track_scan_send();
            app_ble_scan_stop( );
            printf("stop scan ibeacon\r\n");
            vTaskDelay(180000);
        }
    }
}

// LoraWan_Tx_Task
void LoraWan_Tx_Task(void *parameter) {
    while (true) 
    {
        ( void )ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
        while(1)
        {
            if(app_task_lora_tx_engine() == true)
            {
                app_Lora_engine_task_wakeup(  );
            }
            vTaskDelay( 15000 );
        }
    }
}

void setup() {


    default_param_load();
    init_current_lorawan_param();
    delay(1000);
    sensor_init_detect();
    
    app_ble_scan_init();  

    printf("\n---------- STARTUP ----------\n");

    lbmWm1110.begin();
    LbmxEngine::begin(lbmWm1110.getRadio(), ModemEventHandler);

    LbmxEngine::printVersions(lbmWm1110.getRadio());

    xTaskCreate(LoraWan_Engine_Task, "LoraWan_engine_Task", 256*8, NULL, 1, &LORAWAN_ENGINE_Handle);


    xTaskCreate(Ble_Scan_Task, "Ble_Scan_Task", 256*4, NULL, 3, &BLE_SCAN_Handle);

    xTaskCreate(LoraWan_Tx_Task, "LoraWan_Tx_Task", 256*2, NULL, 2, &LORAWAN_TX_Handle);

    // vTaskStartScheduler();
}
 
void loop() {
	
}