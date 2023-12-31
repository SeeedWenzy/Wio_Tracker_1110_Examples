/*
 * sensecap_gnss_uplink.ino
 * Copyright (C) 2023 Seeed K.K.
 * MIT License
 */

////////////////////////////////////////////////////////////////////////////////
// Includes

#include <Arduino.h>

#include <LbmWm1110.hpp>
#include <Lbmx.hpp>

#include <Lbm_packet.hpp>


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
static constexpr uint32_t FIRST_UPLINK_DELAY = 20;  // [sec.]
static constexpr uint32_t UPLINK_PERIOD = 15;       // [sec.]


static constexpr uint32_t EXECUTION_PERIOD = 50;    // [msec.]

static constexpr uint32_t GNSS_SCAN_PERIOD = 180;    // [sec.] //2 minutes minimum
bool gnss_scan_end = false;

////////////////////////////////////////////////////////////////////////////////
// Variables

uint32_t position_period = GNSS_SCAN_PERIOD*1000;   // [msec.]
uint32_t gnss_scan_timeout = 120000; // [msec.]

uint8_t DEV_EUI[8];
uint8_t JOIN_EUI[8];
uint8_t APP_KEY[16];

static LbmWm1110& lbmWm1110 = LbmWm1110::getInstance();
static StateType state = StateType::Startup;

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
    void alarm(const LbmxEvent& event) override;
    void time(const LbmxEvent& event) override;
    void almanacUpdate(const LbmxEvent& event) override;  
    void txDone(const LbmxEvent& event);   

    void gnssScanDone(const LbmxEvent& event) override;
    void gnssScanCancelled(const LbmxEvent& event) override;
    void gnssTerminated(const LbmxEvent& event) override;
    void gnssErrorNoTime(const LbmxEvent& event) override;
    void gnssErrorAlmanacUpdate(const LbmxEvent& event) override;
    void gnssScanStopped(const LbmxEvent& event) override;  
    void gnssErrorNoAidingPosition(const LbmxEvent& event) override;

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

    if (smtc_modem_time_set_sync_interval_s(TIME_SYNC_VALID_TIME / 3) != SMTC_MODEM_RC_OK) abort();     // keep call order
    if (smtc_modem_time_set_sync_invalid_delay_s(TIME_SYNC_VALID_TIME) != SMTC_MODEM_RC_OK) abort();    // keep call order

    printf("Start time sync.\n");
    if (smtc_modem_time_start_sync_service(0, SMTC_MODEM_TIME_ALC_SYNC) != SMTC_MODEM_RC_OK) abort();
    
    printf("Start the alarm event.\n");
    if (LbmxEngine::startAlarm(FIRST_UPLINK_DELAY) != SMTC_MODEM_RC_OK) abort();

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
        }

        // Configure ADR and transmissions
        if (smtc_modem_adr_set_profile(0, SMTC_MODEM_ADR_PROFILE_CUSTOM, adr_custom_list_region) != SMTC_MODEM_RC_OK) abort();              //adr_custom_list_region  CUSTOM_ADR
        if (smtc_modem_set_nb_trans(0, 1) != SMTC_MODEM_RC_OK) abort();
        if (smtc_modem_connection_timeout_set_thresholds(0, 0, 0) != SMTC_MODEM_RC_OK) abort();

        first = false;
    }
}
void MyLbmxEventHandlers::alarm(const LbmxEvent& event)
{

    static uint32_t counter = 0;
    app_task_lora_tx_engine();
    if (LbmxEngine::startAlarm(UPLINK_PERIOD) != SMTC_MODEM_RC_OK) abort();
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

    if( event.event_data.txdone.status == SMTC_MODEM_EVENT_TXDONE_CONFIRMED )
    {
        uplink_count++;
    }

    uint32_t tick = smtc_modem_hal_get_time_in_ms( );
}
void MyLbmxEventHandlers::gnssScanDone(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_SCAN_DONE).c_str());

    gnss_mw_event_data_scan_done_t eventData;
    gnss_mw_get_event_data_scan_done(&eventData);

    float latitude_dif, longitude_dif;
    latitude_dif = fabs( eventData.context.aiding_position_latitude - app_task_gnss_aiding_position_latitude );
    longitude_dif = fabs( eventData.context.aiding_position_longitude - app_task_gnss_aiding_position_longitude );

    /* Store the new assistance position only if the difference is greater than the conversion error */
    if(( latitude_dif > ( float ) 0.03 ) || ( longitude_dif > ( float ) 0.03 ))
    {
        app_task_gnss_aiding_position_latitude  = eventData.context.aiding_position_latitude;
        app_task_gnss_aiding_position_longitude = eventData.context.aiding_position_longitude;
        // TODO, save aiding position to nvds
        int32_t lat_temp = app_task_gnss_aiding_position_latitude * 1000000;
        int32_t long_temp = app_task_gnss_aiding_position_longitude * 1000000;
        printf( "New assistance position stored: %d, %d\r\n", lat_temp, long_temp );

    }

    if (eventData.context.almanac_update_required)
    {
        const uint8_t dmAlmanacStatus = SMTC_MODEM_DM_FIELD_ALMANAC_STATUS;
        if (smtc_modem_dm_request_single_uplink(&dmAlmanacStatus, 1) != SMTC_MODEM_RC_OK) abort();
    }
}

void MyLbmxEventHandlers::gnssTerminated(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_TERMINATED).c_str());

    gnss_mw_event_data_terminated_t eventData;
    gnss_mw_get_event_data_terminated(&eventData);
    printf("TERMINATED info:\n");
    printf("-- number of scans sent: %u\n", eventData.nb_scans_sent);
    printf("-- aiding position check sent: %d\n", eventData.aiding_position_check_sent);
}
void MyLbmxEventHandlers::gnssScanCancelled(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_TERMINATED).c_str());
}

void MyLbmxEventHandlers::gnssErrorNoTime(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_ERROR_NO_TIME).c_str());

    if (smtc_modem_time_trigger_sync_request(0) != SMTC_MODEM_RC_OK) abort();
    mw_gnss_event_state = GNSS_MW_EVENT_ERROR_NO_TIME;

}

void MyLbmxEventHandlers::gnssErrorAlmanacUpdate(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_ERROR_ALMANAC_UPDATE).c_str());

    const uint8_t dmAlmanacStatus = SMTC_MODEM_DM_FIELD_ALMANAC_STATUS;
    if (smtc_modem_dm_request_single_uplink(&dmAlmanacStatus, 1) != SMTC_MODEM_RC_OK) abort();
    mw_gnss_event_state = GNSS_MW_EVENT_ERROR_ALMANAC_UPDATE;
}
void MyLbmxEventHandlers::gnssErrorNoAidingPosition(const LbmxEvent& event)
{
    printf("----- GNSS - %s -----\n", event.getGnssEventString(GNSS_MW_EVENT_ERROR_ALMANAC_UPDATE).c_str());

}
void MyLbmxEventHandlers::gnssScanStopped(const LbmxEvent& event)
{
    gnss_scan_end = true;
    gnss_mw_custom_clear_scan_busy();
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

////////////////////////////////////////////////////////////////////////////////
// setup and loop

void setup()
{
    default_param_load();
    init_current_lorawan_param();
    delay(1000);
    sensor_init_detect();

    printf("\n---------- STARTUP ----------\n");
    custom_lora_adr_compute(0,6,adr_custom_list_region);

    lbmWm1110.attachGnssPrescan([](void* context){ digitalWrite(PIN_GNSS_LNA, HIGH); });
    lbmWm1110.attachGnssPostscan([](void* context){ digitalWrite(PIN_GNSS_LNA, LOW); });

    lbmWm1110.begin();

    app_gps_scan_init();
    // /* Initialize GNSS middleware */
    gnss_mw_init( lbmWm1110.getRadio(), stack_id );
    gnss_mw_custom_enable_copy_send_buffer();
    gnss_mw_set_constellations( GNSS_MW_CONSTELLATION_GPS_BEIDOU );
    
    // /* Set user defined assistance position */
    // gnss_mw_set_user_aiding_position( app_task_gnss_aiding_position_latitude, app_task_gnss_aiding_position_longitude );
    
    LbmxEngine::begin(lbmWm1110.getRadio(), ModemEventHandler);
    LbmxEngine::printVersions(lbmWm1110.getRadio());

}

void loop()
{
    static uint32_t now_time = 0;
	static uint32_t start_scan_time = 0;  
    bool result = false;  
    uint32_t sleepTime = LbmxEngine::doWork();
    if(is_first_time_sync == true)
    {
        now_time = smtc_modem_hal_get_time_in_ms( );
        if(sleepTime > 500)
        {
            if(position_period<120000) position_period = 120000;
            if(now_time - start_scan_time > position_period ||(start_scan_time == 0))
            {
                printf("start scan gnss\r\n");
                app_gps_scan_start();
                gnss_scan_end = false;
                start_scan_time = smtc_modem_hal_get_time_in_ms( );
            }
            // ledOff(LED_BUILTIN);
            if(((smtc_modem_hal_get_time_in_ms( ) - start_scan_time > gnss_scan_timeout)||(gnss_scan_end)) &&(gps_scan_status == 2))
            {
                result = app_gps_get_results( tracker_gps_scan_data, &tracker_gps_scan_len );
                if( result )
                {
                    app_gps_display_results( );
                }
                printf("stop scan gnss\r\n");
                app_gps_scan_stop( );
                sensor_datas_get();
                //send data to LoRaWAN
                app_task_track_scan_send();
            }
        }
        sleepTime = smtc_modem_hal_get_time_in_ms( )-now_time;
    }
    delay(min(sleepTime, EXECUTION_PERIOD));
}

////////////////////////////////////////////////////////////////////////////////
