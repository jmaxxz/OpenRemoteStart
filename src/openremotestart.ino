// Copyright 2019 Jmaxxz
// BSD 3-Clause License

/*
* ORS (Open remote start) is an open source implementation
* of the Fortin data-link protocol. While it can run on a
* particle photon the electron will provide a more feature
* rich experience. This is because the electron has 3 uarts
* this allows monitoring and controlling of both the TX and
* RX lines to the remote starter. This is not needed if one
* is just trying to create a wifi remote for a fortin alarm
* however, if one wishes to further explore the data-link
* interface having bidirection monitoring and commanding
* is very useful.
*
* This implementation of the fortin interface was created
* by reverse engineering the communication between a fortin
* evo and several off the shelf rfkits. As such some parts
* of the message structure are still unknown, and there 
* maybe entire commands which are not properly understood.
*
* This firmwware is first and for most a research tool
* and secondly an wifi/cellular antenna. As stated in
* the license use at your own risk.
*/
#include "application.h"
#include "fortin.h"
#include "shell.h"
#include "ringbuffer.h"
#include "fortinprocessor.h"
#include "Particle.h"


SYSTEM_THREAD(ENABLED);

// Log status requests to shell.
// this creates a lot of noise so it
// is recommended to leave this off
// most of the time.
// #define ORS_PRINT_STATUS_REQUEST

// The default amount of time between requests
// to remote starter for status update.
#define ORS_MS_BETWEEN_STATUS_UPDATES 15000

// If insufficient uarts are available monitor
// starter rather than remotes. If in doubt
// leave this on.
#define ORS_PREFER_STARTER_UART

// Enable potentially dangerous commands.
// If in doubt comment out.
#define ORS_DANGER_ZONE



// Platform specific configuration
#if (PLATFORM_ID == 6) // Photon
    #ifdef ORS_PREFER_STARTER_UART
        #define StarterUart Serial1
    #else
        #define RemoteUart Serial1
    #endif

    #define ORS_MS_BETWEEN_STATUS_PUBLISH 15000

#elif (PLATFORM_ID == 10) // Electron
    #include "Serial4/Serial4.h"
    #include "Serial5/Serial5.h"
    #define RemoteUart Serial4
    #define StarterUart Serial5
    #define ORS_MS_BETWEEN_STATUS_PUBLISH 600000
    #define ORS_ASSET_TRACKER
#endif

#ifdef ORS_ASSET_TRACKER
    #include "LIS3DH.h"
    #include "AssetTrackerRK.h" 
#endif

enum parserSate {
  unknown,
  messageStarted,
  messageEnded
};


struct OrsSettings
{
    uint8_t version;
    uint8_t address[3];
    bool cloneAddress:1;
    bool blockAlarm:1;
    uint8_t reserved:6;
    uint16_t checksum;
};

// Used to integrate with https://github.com/jmaxxz/particle-smartthings
// or really anything where you want to be able
// to detect what type of firmware a given particle device
// on your account is running. 
char m_devhandler[] = "ORS";

// We will be using a ringbuffer
// to read in messages from the remote stater
// at this time we do not know of any messages
// which are longer than 32 chars. The protocol
// theoretically supports longer messages.
FortinProcessor *m_remote_msg_proc = new FortinProcessor(new RingBuffer(32), handleValidMessage);
FortinProcessor *m_starter_msg_proc = new FortinProcessor(new RingBuffer(32), handleMessageToStarter);

// Increates every time a invalid packet is observed
// on the fortin data bus. This is primarily used
// to help identify any errors in our messaging parsing
int m_invalid_msg_count = 0;

// is the car's alarm system currently
// armed? This is roughly correlated to
// the lock state of the car.
bool m_car_armed = false;

// Is the car's ignition on?
bool m_car_acc = false;

// Is a door, truck, or hood open?
// If this goes true while the car
// is armed the alarm will go off.
bool m_car_door_opened = false;

// I don't know what this is yet
bool m_engine_started = false;

// Is the car remote started?
// Sometime the car's ignition will be off
// but this will be true. Normally that means
// the remote starter is still working on
// turning the car on.
bool m_car_remote_started = false;

// Is the car in valet mode.
// When this is true both the alarm and remote starter
// are disabled.
bool m_car_valet_mode = false;

// Don't know what this is yet
bool m_trunk_open = false;

// Don't know what this is yet
bool m_car_unknown1 = false;

// When this is true we will try to clone an existing
// address seen on the uart databus. Normally one
// will want to pair with the remote starter. However,
// due to protocol flaws it is relatively easy to just
// repurpose an existing address. There is really no
// downside to repurposing an existing address besides
// that this is not what the protocol designers wanted
// us to do. This will automatically get set to false
// once an address is learned.
bool m_clone_addr = true;

// When this is set to true we will disarm the car
// immediately following alarm activation. This is
// feature is for demonstration purposes only. It
// shows a possible malicious action a rogue device
// of the fortin databus could take.
bool m_block_alarm = false;

uint8_t m_remote_addr[] = {0xDE, 0xAD, 0x01};
// A buffer which holds the current address being used.

// In milliseconds when was the last status message recieved
// from the remote starter/alarm system?
unsigned long m_last_status_request = 0;

// When the car is remote started this will get set to the
// number of seconds remaining before the car will be shutdown
uint16_t m_car_start_countdown = 0;

// In milliseconds when was the last time we published an
// update to the cloud. This is used to help us figure out
// when it is reasonable to publish another update.
unsigned long m_last_published = 0;

// Has the state changed since we last published an update
bool m_state_changed = false;

// The last message we read from the uart databus (as hex string)
char m_last_rx[256];

// A json object representing our current state.
char m_current[256];

// A lookup table to help us quickly build hex strings
static char b_to_hex[] = "0123456789ABCDEF";

// how many milliseconds how long should we wait
// before requesting a status update.
unsigned long m_status_period = ORS_MS_BETWEEN_STATUS_UPDATES;

// Last known position
float m_last_lon = 0;
float m_last_lat = 0;

#ifdef ORS_ASSET_TRACKER
    AssetTracker tracker = AssetTracker();
    LIS3DHSPI accel(SPI, A2, WKP);
    volatile bool movementInterrupt = false;

    // limit gps on time to 10 min
    unsigned long m_max_gps_on_time = 60*10*1000;

    // When the gps turned on. This is used to
    // determine if it is time to shut the gps off
    unsigned long m_gps_on_time;

    // Is the gps currently turned on?
    bool m_is_gps_on;

    unsigned long m_last_gps_update;
    unsigned long m_last_gps_log_time;

    // Threshold for identifying car motion.
    int m_accel_threshold = 32;
#endif

// Our usbuart shell for test/debug
Shell *m_shell = new Shell(m_current, carCommand, set);



void setup() {
    m_last_status_request = millis() - m_status_period;
    m_last_rx[0] = 0;
    m_current[0] = 0;
    Particle.function("car", carCommand);
    Particle.function("set", set);
    Particle.variable("rx", m_last_rx, STRING);
    Particle.variable("current", m_current, STRING);
    Particle.variable("devhandler", m_devhandler, STRING);
    Particle.variable("invMsg", &m_invalid_msg_count, INT);
    Serial.begin(115200);

    #ifdef RemoteUart
    RemoteUart.begin(9600);
    #endif

    #ifdef StarterUart
    StarterUart.begin(9600);
    #endif

    #ifdef ORS_ASSET_TRACKER
    tracker.begin();
    tracker.gpsOn();
    m_is_gps_on = true;
    m_gps_on_time = millis();
    LIS3DHConfig config;
	config.setAccelMode(LIS3DH::RATE_10_HZ);
    config.setLowPowerWakeMode(m_accel_threshold);
    bool setupSuccess = accel.setup(config);
	Serial.printlnf("SetupAccel?=%d", setupSuccess);
    attachInterrupt(WKP, movementInterruptHandler, RISING);
    #endif

    loadSettings();
}

// Read messages like: 0C 03 0E 01 00 12 0D
// 0C is a sync characters
// 03 is an address
// 0E is an address
// 01 is the type of message
// 00 is the length of the payload
// 12 is the checksum of (03 0E 01 00)
// All messages are 7 bytes + whatever the size of the payload is.
// Payload size is in the 5th bytes of any message
void loop() {
    unsigned long ticks = millis();

    if(Serial) {
		m_shell->processSerial();
	}

    if(m_last_status_request+m_status_period < ticks){
        sendCommand(remote_command_t::status_request);
        m_last_status_request = ticks;
    }
    // Read count it used to make sure we regularly exit this loop
    int readCount = 0;
    #ifdef RemoteUart
    while(RemoteUart.peek() != -1 && readCount < 32){
        readCount++;
        int b =  RemoteUart.read();
        m_remote_msg_proc->add(b);
    }
    #endif

    #ifdef StarterUart
        int starterReadCount = 0;
        while(StarterUart.peek() != -1 && starterReadCount < 32){
            starterReadCount++;
            int b =  StarterUart.read();
            m_starter_msg_proc->add(b);
        }
    #endif

    #ifdef ORS_ASSET_TRACKER
    tracker.updateGPS();
    if(m_is_gps_on && (ticks - m_gps_on_time) > m_max_gps_on_time){
        tracker.gpsOff();
        m_is_gps_on = false;
        m_shell->println("Shutting off gps");
    }

    if (m_is_gps_on && tracker.gpsFix() && ticks - m_last_gps_log_time > 2000) {
        if(ticks - m_last_gps_log_time > 60000) {
            Serial.println(tracker.readLatLon());
            m_last_gps_log_time = ticks;
        }
        m_last_lat = tracker.readLatDeg();
        m_last_lon = tracker.readLonDeg();
        m_last_gps_update = ticks;
    }
    if (movementInterrupt) {
        accel.clearInterrupt();
        movementInterrupt = false;
        if(!m_is_gps_on){
            m_shell->println("Movement detected waking up gps");
        }
        // As long as there is movement keep the gps on
        gpsOn();
    }
    #endif
}

void handleMessageToStarter(uint8_t *message, int length){
    char prefixBuffer[15];
    prefixBuffer[0] = 0;
    if(message[4]>=3) {
        snprintf(prefixBuffer, sizeof(prefixBuffer), "[%02x%02x%02x > RS] ", message[5], message[6], message[7]);
    }
    switch (static_cast<remote_command_t>(message[3])){
        case remote_command_t::unlock:
            m_shell->print(prefixBuffer);
            m_shell->println("Unlocking/Disarming");
        break;
        case remote_command_t::lock:
            m_shell->print(prefixBuffer);
            m_shell->println("Locking/Arming");
        break;
        case remote_command_t::start:
            m_shell->print(prefixBuffer);
            m_shell->println("Remote Starting");
        break;
        case remote_command_t::trunk_release:
            m_shell->print(prefixBuffer);
            m_shell->println("Releasing Trunk/Disarming");
        break;
        case remote_command_t::unlock2:
            m_shell->print(prefixBuffer);
            m_shell->println("Unlocking(v2)/Disarming");
        break;
        case remote_command_t::toggle_valet_mode:
            m_shell->print(prefixBuffer);
            m_shell->println("Toggling Valet Mode");
        break;
        case remote_command_t::prog_btn_press:
            m_shell->print(prefixBuffer);
            m_shell->println("Pressing Program Button");
        break;
        case remote_command_t::stop:
            m_shell->print(prefixBuffer);
            m_shell->println("Stopping");
        break;
        case remote_command_t::panic:
            m_shell->print(prefixBuffer);
            m_shell->println("Panicking");
        break;
        case remote_command_t::status_request:
            // Don't print this normally, it generates
            // too much noise. 
            #ifdef ORS_PRINT_STATUS_REQUEST
            m_shell->print(prefixBuffer);
            m_shell->println("Requesting Status");
            #endif
        break;
        case remote_command_t::aux1:
            m_shell->print(prefixBuffer);
            m_shell->println("Trigger Aux1");
        break;
        case remote_command_t::aux2:
            m_shell->print(prefixBuffer);
            m_shell->println("Trigger Aux2");
        break; 
        case remote_command_t::aux3:
            m_shell->print(prefixBuffer);
            m_shell->println("Trigger Aux3");
        break; 
        case remote_command_t::aux4:
            m_shell->print(prefixBuffer);
            m_shell->println("Trigger Aux4");
        break; 
        default: // Unknown message
        char messageAsHex[length*3+1];
        char strBuffer[sizeof(messageAsHex)+50];
        for (int i = 0; i < length; i++){
            byte nib1 = (message[i] >> 4) & 0x0F;
            byte nib2 = message[i] & 0x0F;
            messageAsHex[i*3] = b_to_hex[nib1];
            messageAsHex[i*3+1] = b_to_hex[nib2];
            messageAsHex[i*3+2] = ' ';
        }
        messageAsHex[length*3] = 0;
        m_shell->print(prefixBuffer);
        snprintf(strBuffer, sizeof(strBuffer), "Ant>starter:[%s]", messageAsHex);
        m_shell->println(strBuffer);     
        break;
    }

         
}

void handleValidMessage(uint8_t *message, int length){
    writeMessageToCloudVar(message, length);
    // Example of valid message
    //  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
    // 0C 03 0E B8 09 FF FF F1 01 84 00 00 01 48 8F 0D
    // Status updates (0xB8) should  have a payload of size 9
    starter_command_t msgType = static_cast<starter_command_t>(message[3]);
    if(m_clone_addr && message[4] > 3 && m_clone_addr){
        m_remote_addr[0] = message[5];
        m_remote_addr[1] = message[6];
        m_remote_addr[2] = message[7];
        m_clone_addr = false;
        char strBuffer[100];
        snprintf(strBuffer, sizeof(strBuffer), "Address cloned [%02x %02x %02x]", m_remote_addr[0], m_remote_addr[1], m_remote_addr[2]);
        saveSettings();
        m_shell->println(strBuffer);
    }
    switch(msgType){
        case starter_command_t::led_on:
        m_shell->println("LED on"); 
        break;

        case starter_command_t::led_off:
        m_shell->println("LED off"); 
        break;

        case starter_command_t::status_update:
        handleStatusUpdate(message, length);
        break;

        case starter_command_t::led_flashing: 
        if(message[4] > 0 && message[5]>1){
            m_shell->println("LED flashing quickly");
        } else {
            m_shell->println("LED flashing slowly");
        }
        break;

        case starter_command_t::remote_pairing:
        m_shell->println("Remote pairing");
        // Wait for a random period to avoid collisions with other remotes
        delay(random(2500));
        
        // Send join request. When the remote starter is in pairing mode
        // any 0x30 command it recieves will result in the address that
        // sent it being learned.
        sendCommand(remote_command_t::lock);
        break;

        default: // Unknown message
        char messageAsHex[length*3+1];
        char strBuffer[sizeof(messageAsHex)+50];
        for (int i = 0; i < length; i++){
            byte nib1 = (message[i] >> 4) & 0x0F;
            byte nib2 = message[i] & 0x0F;
            messageAsHex[i*3] = b_to_hex[nib1];
            messageAsHex[i*3+1] = b_to_hex[nib2];
            messageAsHex[i*3+2] = ' ';
        }
        messageAsHex[length*3] = 0;
        snprintf(strBuffer, sizeof(strBuffer), "unknown command [%s]", messageAsHex);
        m_shell->println(strBuffer);     
        break;
    }
}

void handleStatusUpdate(uint8_t *message, int length){
    if(message[4] != 9){
        // invalid status message
        return;
    }


    int startOfPayload = 5;
    uint8_t payloadBytes[9];
    for(int i = 0; i < 9; i++){
        payloadBytes[i] = message[startOfPayload+i];
    }
    
    statusPayload* payload = (statusPayload *)payloadBytes;
    
    uint16_t counter = (payload->counter[0]<< 8) | (payload->counter[1]);
        
    if(payload->counterType[0] == 0x40 && payload->counterType[1] == 0x20) {
        int delta = m_car_start_countdown - counter;
        if(m_last_published + ORS_MS_BETWEEN_STATUS_PUBLISH < millis() || abs(delta) > 20){ // have not published recently
            m_state_changed = true;
        }
        m_last_status_request = millis();
        m_car_start_countdown = counter;
    }

    char buff[50];
    snprintf(buff, sizeof(buff), "Unknown byte [%02x]", payload->unknownByte);
    //m_shell->println(buff);

    if(payload->armed != m_car_armed){
        m_state_changed = true;
        m_car_armed = payload->armed;
        m_shell->println(m_car_armed ? "Armed": "Disarmed");
    }
    
    if(payload->engineTurningOver != m_engine_started){
        m_state_changed = true;
        m_engine_started = payload->engineTurningOver;
        m_shell->println(m_engine_started ? "Engine Rotating": "Engine Stopped");
    }
    
    if(payload->trunkOpen != m_trunk_open){
        m_state_changed = true;
        m_trunk_open = payload->trunkOpen;
        m_shell->println(m_trunk_open ? "Trunk Open": "Trunk Closed");
    }
    
    if(payload->unknownFlag1 != m_car_unknown1){
        m_state_changed = true;
        m_car_unknown1 = payload->unknownFlag1;
        m_shell->println(m_car_unknown1 ? "Unknown1 On": "Unknown1 Off");
    }
    
    if(payload->acc != m_car_acc){
        m_state_changed = true;
        m_car_acc = payload->acc;
        m_shell->println(m_car_acc ? "Started": "Stopped");
    }
    
    if(payload->doorOpened != m_car_door_opened){
        m_state_changed = true;
        m_car_door_opened = payload->doorOpened;
        m_shell->println(m_car_door_opened ? "Opened": "Closed");
        if(m_car_door_opened && m_car_armed) {
            m_shell->println("Alarming");

#ifdef ORS_DANGER_ZONE
            if(m_block_alarm){
                sendCommand(remote_command_t::unlock);
                m_shell->println("Alarm Suppressed");
            }
#endif
        }
    }
    
    if(payload->remoteStarted != m_car_remote_started){
        m_state_changed = true;
        m_car_remote_started = payload->remoteStarted;
        m_shell->println(m_car_remote_started ? "Remote Started": "Remote Start Ended");
        if(!m_car_remote_started){
            m_car_start_countdown = 0;
        }
    }
    
    if(payload->valetMode != m_car_valet_mode){
        m_state_changed = true;
        m_car_valet_mode = payload->valetMode;
        m_shell->println(m_car_valet_mode ? "Valet On": "Valet Off");
    }
    
    updateCurrent();
    if(m_state_changed == true) {
        publishUpdate();
        m_state_changed = false;
    }
}

void updateCurrent(){
	snprintf(m_current, sizeof(m_current), "{\"ar\":%d,\"e\":%d,\"t\":%d,\"u1\":%d,\"ig\":%d,\"do\":%d,\"rs\":%d,\"v\":%d,\"rcd\":%d,\"lon\":%f,\"lat\":%f}",
        m_car_armed,
        m_engine_started,
        m_trunk_open,
        m_car_unknown1,
        m_car_acc,
        m_car_door_opened,  
        m_car_remote_started,
        m_car_valet_mode,
        m_car_start_countdown,
        m_last_lon,
        m_last_lat);
}

void publishUpdate(){
    m_last_published = millis();
    Particle.publish("state-update", m_current, PRIVATE);
}

void writeMessageToCloudVar(uint8_t *message, int length){
    //Is Valid so assign it to a cloud variable
    for (int i = 0; i < length; i++) {
        byte nib1 = (message[i] >> 4) & 0x0F;
        byte nib2 = message[i] & 0x0F;
        m_last_rx[i*3] = b_to_hex[nib1];
        m_last_rx[i*3+1] = b_to_hex[nib2];
        m_last_rx[i*3+2] = ' ';
    }
    m_last_rx[length*3] = 0;
}

void handleInvalidMessage(){
    // Scan buffer for new starting point and resume.
    m_remote_msg_proc->reset();
    m_invalid_msg_count++;
}

int set(String command) {
	int lenOfName = command.indexOf('=');
    if(lenOfName < 1){
        // name must at least contain one character
        return -1;
    }
    int lenOfValue = command.length() - lenOfName - 1;
    if(lenOfValue < 1){
        // must contain a value
        return -2;
    }
    String name = command.substring(0, lenOfName);
    String value = command.substring(lenOfName + 1);

    if(name == "Addr") {
        unsigned int a1, a2, a3;
        sscanf (value,"%02x %02x %02x", &a1, &a2, &a3);
        m_remote_addr[0] = a1;
        m_remote_addr[1] = a2;
        m_remote_addr[2] = a3;
        // set the addr used by ors
    } else if(name == "CloneAddr") {
        // activate or deactivate address cloning
        m_clone_addr = value == "1";
    } else if(name == "BlockAlarm"){
        m_block_alarm  = value == "1";
    } else if(name == "GPS"){
        
    } else {
        // no such command
        return -3;
    }

    saveSettings();
    return 0;
}

int sendCommand(remote_command_t cmd){
    return sendCommand((int)cmd);
}

int sendCommand(uint8_t cmd){
    
    uint8_t checksum = 0;
    // When sending commands to the remote starter I use a 5 byte payload
    // the first 3 bytes are the address we are currently using. If the remote starter
    // does not recognize the address it will ignore the message. The last 2 bytes
    // are always 0x00 and 0x33. I suspect these are some type of protocol version
    // specifier. If one does not include them and instead only sends a 3 byte
    // payload consisting only of our address some commands behave differently
    // for example the start command behaves as a toggle command rather than a start command
    // meaning back to back start commands will cause the car to shut off.
    uint8_t message[]  = {0x0C, 0x0E, 0x03, cmd, 0x05, m_remote_addr[0], m_remote_addr[1], m_remote_addr[2], 0x00, 0x33, checksum, 0x0D};
    for(size_t i = 1; i < sizeof(message)-2; i++){
        checksum += message[i];
    }
    message[sizeof(message)-2] = checksum;

    #ifdef RemoteUart
    RemoteUart.write(message, sizeof(message));
    #endif

    switch (static_cast<remote_command_t>(cmd)){
        case remote_command_t::unlock:
            m_shell->println("Unlocking/Disarming");
        break;
        case remote_command_t::lock:
            m_shell->println("Locking/Arming");
        break;
        case remote_command_t::start:
            m_shell->println("Remote Starting");
        break;
        case remote_command_t::trunk_release:
            m_shell->println("Releasing Trunk/Disarming");
        break;
        case remote_command_t::unlock2:
            m_shell->println("Unlocking(v2)/Disarming");
        break;
        case remote_command_t::toggle_valet_mode:
            m_shell->println("Toggling Valet Mode");
        break;
        case remote_command_t::prog_btn_press:
            m_shell->println("Pressing Program Button");
        break;
        case remote_command_t::stop:
            m_shell->println("Stopping");
        break;
        case remote_command_t::panic:
            m_shell->println("Panicking");
        break;
        case remote_command_t::status_request:
        #ifdef ORS_PRINT_STATUS_REQUEST
            // Don't print this normally, it generates
            // too much noise. 
            m_shell->println("Requesting Status");
        #endif
        break;
        case remote_command_t::aux1:
            m_shell->println("Trigger Aux1");
        break;
        case remote_command_t::aux2:
            m_shell->println("Trigger Aux2");
        break; 
        case remote_command_t::aux3:
            m_shell->println("Trigger Aux3");
        break; 
        case remote_command_t::aux4:
            m_shell->println("Trigger Aux4");
        break; 
        default:
            break;
    }
    return 0;
}

int carCommand(String command) {
    int cmd =  command.toInt();
    if (cmd >= 0 && cmd <= 255) {
        sendCommand(cmd);
        return 0;
    }
    return -1;
}

uint16_t calculateChecksum(OrsSettings settings){
    uint16_t checksum = 0;
    checksum += settings.version;
    checksum += (uint16_t)settings.address[0]*3;
    checksum += (uint16_t)settings.address[1]*5;
    checksum += (uint16_t)settings.address[2]*7;
    checksum += (settings.blockAlarm | settings.cloneAddress << 1) * 11;
    return checksum;
}

void saveSettings(){
    OrsSettings settings = OrsSettings();
    settings.version = 1;
    settings.address[0] = m_remote_addr[0];
    settings.address[1] = m_remote_addr[1];
    settings.address[2] = m_remote_addr[2];
    settings.reserved = 0;
    settings.blockAlarm = m_block_alarm;
    settings.cloneAddress = m_clone_addr;
    settings.checksum = calculateChecksum(settings);

    m_shell->println("Saved settings");
    EEPROM.put(0, settings);

}

void loadSettings(){
    OrsSettings settings = OrsSettings();
    EEPROM.get(0, settings);
    uint16_t expectedChecksum = calculateChecksum(settings);
    if(expectedChecksum == settings.checksum){
        m_remote_addr[0] = settings.address[0];
        m_remote_addr[1] = settings.address[1];
        m_remote_addr[2] = settings.address[2];
        m_clone_addr = settings.cloneAddress;
        m_block_alarm = settings.blockAlarm;
        m_shell->println("Loaded settings");
    } else {
        m_shell->println("Settings could not be loaded, checksum mismatch.");
    }
}

#ifdef ORS_ASSET_TRACKER
void gpsOn(){
    m_gps_on_time = millis();
    m_is_gps_on = true;
    tracker.gpsOn();
}

void movementInterruptHandler() {
	movementInterrupt = true;
}
#endif
