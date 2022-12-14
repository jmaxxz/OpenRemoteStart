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


// If you need to debug without a cloud connection
// the particle can be put into an offline mode of
// sorts using the set system mode to semi automatic
// SYSTEM_MODE(SEMI_AUTOMATIC)

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
// leave this off.
// #define ORS_PREFER_STARTER_UART

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
    #define WiFi
    #define ORS_MS_BETWEEN_STATUS_PUBLISH 15000

#elif (PLATFORM_ID == 10) // Electron
    #include "Serial4/Serial4.h"
    #include "Serial5/Serial5.h"
    #define RemoteUart Serial4
    #define StarterUart Serial5
    #define ORS_MS_BETWEEN_STATUS_PUBLISH 600000
    #define ORS_ASSET_TRACKER

#elif (PLATFORM_ID == 13) // BORON
    #ifdef ORS_PREFER_STARTER_UART
        #define StarterUart Serial1
    #else
        #define RemoteUart Serial1
    #endif
    #define ORS_MS_BETWEEN_STATUS_PUBLISH 600000
#endif

#ifdef ORS_ASSET_TRACKER
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
    bool verbose:1;
    uint8_t reserved:5;
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

// Is the engine turning over?
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

// Is the trunk open?
bool m_trunk_open = false;

// Don't know what this is yet
bool m_car_unknown1 = false;

// Don't know what this is yet
uint8_t m_car_unknownByte = 0;

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

// When this is set to true all commands sent
// by ORS will be logged to the usb serial in
// there raw form.
bool m_log_all = false;

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

// A json object representing the devices current configuration.
char m_settings[256];

// A lookup table to help us quickly build hex strings
static char b_to_hex[] = "0123456789ABCDEF";

// how many milliseconds how long should we wait
// before requesting a status update.
unsigned long m_status_period = ORS_MS_BETWEEN_STATUS_UPDATES;

// Last known position
float m_last_lon = 0;
float m_last_lat = 0;
float m_last_alt = 0;

// The current number of satellites being tracked
int m_satellite_count = 0;

// Is the gps currently turned on?
bool m_is_gps_on;

#ifdef ORS_ASSET_TRACKER
    AssetTracker tracker = AssetTracker();

    // limit gps on time to 10 min
    unsigned long m_max_gps_on_time = 60*10*1000;

    // When the gps turned on. This is used to
    // determine if it is time to shut the gps off
    unsigned long m_gps_on_time;

    unsigned long m_last_gps_update = 0;
    unsigned long m_last_gps_log_time = 0;

    // Threshold for identifying car motion.
    int m_accel_threshold = 16;
#endif

// Our usbuart shell for test/debug
Shell *m_shell = new Shell(m_current, carCommand, set);



void setup() {
    m_last_status_request = millis() - m_status_period;
    m_last_rx[0] = 0;
    m_current[0] = 0;
    m_settings[0] = 0;
    Particle.function("car", carCommand);
    Particle.function("set", set);
    Particle.variable("rx", m_last_rx, STRING);
    Particle.variable("current", m_current, STRING);
    Particle.variable("settings", m_current, STRING);
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
    tracker.gpsOn();
    tracker.startThreadedMode();
    m_is_gps_on = true;
    m_gps_on_time = millis();
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

    #ifdef RemoteUart
    int readCount = 0;
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
    // If the engine is off and the gps has been on for a while
    // shut it down.
    if(!m_engine_started && m_is_gps_on && (ticks - m_gps_on_time) > m_max_gps_on_time){
        gpsOff();
    }

    if (m_is_gps_on && tracker.gpsFix() && ticks - m_last_gps_log_time > 2000) {
        if(ticks - m_last_gps_log_time > 60000) {
            Serial.println(tracker.readLatLon());
            m_last_gps_log_time = ticks;
        }
        m_last_lat = tracker.readLatDeg();
        m_last_lon = tracker.readLonDeg();
        m_last_alt = tracker.getAltitude();
        m_last_gps_update = ticks;
    }
    if(m_is_gps_on){
        m_satellite_count = tracker.getSatellites();
    }
    #endif
}

void printMessage(String format, uint8_t message[], int messageLength){
    size_t messageSizeAsHex = messageLength*3+1;
    char messageAsHex[messageSizeAsHex];
    for (int i = 0; i < messageLength; i++){
        byte nib1 = (message[i] >> 4) & 0x0F;
        byte nib2 = message[i] & 0x0F;
        messageAsHex[i*3] = b_to_hex[nib1];
        messageAsHex[i*3+1] = b_to_hex[nib2];
        messageAsHex[i*3+2] = ' ';
    }
    messageAsHex[messageLength*3] = 0;
    char strBuffer[messageSizeAsHex+format.length()+10];
    snprintf(strBuffer, sizeof(strBuffer), format, messageAsHex);
    m_shell->println(strBuffer);
}

void handleMessageToStarter(uint8_t *message, int length){
    if(length < 5){
        printMessage("Too Short Message to RS: [%s]", message, length);
        return;
    }
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
        printMessage("Ant>starter:[%s]", message, length);
        break;
    }


}

void handleValidMessage(uint8_t *message, int length){
    bool wasUnkownMessage = false;
    writeMessageToCloudVar(message, length);

    if(length >=5){
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
            if(handleStatusUpdate(message, length) != 0){
                wasUnkownMessage = true;
            }
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
            // sent it being "learned".
            sendCommand(remote_command_t::lock);
            break;

            default: // Unknown message
            wasUnkownMessage = true;
            break;
        }
    } else {
        wasUnkownMessage = true;
    }


    if(wasUnkownMessage) {
        printMessage("unknown command: [%s]", message, length);
    } else if(m_log_all) {
        printMessage("[verbose]starter>atn: [%s]", message, length);
    }
}

int handleStatusUpdate(uint8_t *message, int length){
    if(message[4] != 9){
        // invalid status message
        return -1;
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

    if(payload->unknownByte != m_car_unknownByte){
        m_state_changed = true;
        m_car_unknownByte = payload->unknownByte;
        char buff[50];
        snprintf(buff, sizeof(buff), "Unknown byte [%02x]", payload->unknownByte);
        m_shell->println(buff);
    }

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
        #ifdef ORS_ASSET_TRACKER
        // Any time the car changes state turn on the GPS
        gpsOn();
        #endif
        m_state_changed = false;
    }

    return 0;
}

void updateCurrent(){
	snprintf(m_current, sizeof(m_current), "{\"ar\":%d,\"e\":%d,\"t\":%d,\"u1\":%d,\"ig\":%d,\"do\":%d,\"rs\":%d,\"v\":%d,\"rcd\":%d,\"lon\":%f,\"lat\":%f,\"alt\":%f,\"sat\":%d}",
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
        m_last_lat,
        m_last_alt,
        m_satellite_count
        );

        m_shell->println(m_current);
}

void updateSettings(){
    // cl == clone address on
    // ad == address
    // g == gps on
    // ba == block alarm

 	snprintf(m_settings, sizeof(m_settings), "{\"cl\":%d,\"ad\":\"%02x%02x%02x\",\"g\":%d,\"ba\":%d}",
      m_clone_addr ? 1 : 0,
      m_remote_addr[0],
      m_remote_addr[1],
      m_remote_addr[2],
      m_is_gps_on ? 1 : 0,
      m_block_alarm ? 1 : 0
     );

    m_shell->println(m_settings);
}

void publishUpdate(){
    if (Particle.connected()) {
        m_last_published = millis();
        Particle.publish("state-update", m_current, PRIVATE);
    }
}

void writeMessageToCloudVar(uint8_t *message, int length){
    //Is Valid so assign it to a cloud variable
    for (int i = 0; i < length && i*3 < sizeof(m_last_rx)-1; i++) {
        byte nib1 = (message[i] >> 4) & 0x0F;
        byte nib2 = message[i] & 0x0F;
        m_last_rx[i*3] = b_to_hex[nib1];
        m_last_rx[i*3+1] = b_to_hex[nib2];
        m_last_rx[i*3+2] = ' ';
    }
    m_last_rx[length*3] = 0;
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
    }
    #ifdef ORS_ASSET_TRACKER
    else if(name == "GPS"){
        if(value == "1"){
            gpsOn();
        } else
        {
            gpsOff();
        }

    }
    #endif
    else if(name=="Verbose"){
        m_log_all = value == "1";
    } else {
        // no such command
        return -3;
    }

    saveSettings();
    return 0;
}
int sendRaw(uint8_t message[], size_t messageLength){
    #ifdef RemoteUart
    if(m_log_all){
        printMessage("[verbose]atn>starter: %s",message, messageLength);
    }
    RemoteUart.write(message, messageLength);
    #endif
    return 0;
}

int sendCommand(remote_command_t cmd){
    return sendCommand((int)cmd);
}

int sendCommand(uint8_t cmd){
    uint8_t payload[] {m_remote_addr[0], m_remote_addr[1], m_remote_addr[2], 0x00, 0x33};
    return sendCommand(cmd, payload, sizeof(payload));
}

int sendCommand(uint8_t cmd, uint8_t payload[], uint8_t payloadLength){

    uint8_t checksum = 0;
    size_t msgLength = 7+payloadLength;
    // When sending commands to the remote starter I use a 5 byte payload
    // the first 3 bytes are the address we are currently using. If the remote starter
    // does not recognize the address it will ignore the message. The last 2 bytes
    // are always 0x00 and 0x33. I suspect these are some type of protocol version
    // specifier. If one does not include them and instead only sends a 3 byte
    // payload consisting only of our address some commands behave differently
    // for example the start command behaves as a toggle command rather than a start command
    // meaning back to back start commands will cause the car to shut off.
    uint8_t message[msgLength];
    message[0] = 0x0C;
    message[1] = 0x0E;
    message[2] = 0x03;
    message[3] = cmd;
    message[4] = payloadLength;
    message[5+payloadLength] = checksum;
    message[6+payloadLength] = 0x0D;

    memcpy(&message[5], payload, payloadLength);
    for(size_t i = 1; i < msgLength-2; i++){
        checksum += message[i];
    }
    message[msgLength-2] = checksum;

    sendRaw(message, sizeof(message));

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

/*
* Converts a lower case hex character to a nibble
*/
uint8_t hexToNib(char c){
    uint8_t result = 0;

    if (c >= '0' && c <= '9') {
        result =  c - '0';
    } else if (c >= 'a' && c <= 'f'){
        result = c - ('a' - 0x0a);
    }
    return result;
}

int carCommand(String command) {
    if(command == "start"){
		return sendCommand(remote_command_t::start);
	} else if(command == "stop"){
		return sendCommand(remote_command_t::stop);
	} else if(command == "panic"){
		return sendCommand(remote_command_t::panic);
	} else if(command == "lock"){
		return sendCommand(remote_command_t::lock);
	} else if(command == "unlock"){
		return sendCommand(remote_command_t::unlock);
	} else if(command == "trunk"){
		return sendCommand(remote_command_t::trunk_release);
	} else if(command == "aux1"){
		return sendCommand(remote_command_t::aux1);
	} else if(command == "aux2"){
		return sendCommand(remote_command_t::aux2);
	} else if(command == "aux3"){
		return sendCommand(remote_command_t::aux3);
	} else if(command == "aux4"){
		return sendCommand(remote_command_t::aux4);
	} else if(command == "unlock2"){ // Don't know what unlock 2 does differently
        return sendCommand(remote_command_t::unlock2);
    } else if(command == "status"){
        return sendCommand(remote_command_t::status_request);
    } else if(command == "valet"){
        return sendCommand(remote_command_t::toggle_valet_mode);
    } else if(command == "refresh"){
        updateCurrent();
        updateSettings();
        return 0;
    }

    // Prepare to treat command as hex
    command.toLowerCase();
    bool isHex = (command.length() % 2) == 0;
    for (size_t i = 0; i < command.length() && isHex; i++) {
        if((command[i] >= '0' && command[i] <= '9') || (command[i] >= 'a' && command[i] <= 'f')) {
        } else {
            isHex = false;
        }
    }

    if(isHex){
        uint8_t hexCommand[command.length()/2];
        for (size_t i = 0; i < sizeof(hexCommand); i++)
        {
            hexCommand[i] = (hexToNib(command[(size_t)i*2]) << 4);
            hexCommand[i] |= hexToNib(command[(size_t)i*2+1]);
        }

        if(sizeof(hexCommand) == 1){ // naked command
            return sendCommand(hexCommand[0]);
        } else {
            return sendRaw(hexCommand, sizeof(hexCommand));
        }
    }

    return -1;
}

uint16_t calculateChecksum(OrsSettings settings){
    uint16_t checksum = 0;
    checksum += settings.version;
    checksum += (uint16_t)settings.address[0]*3;
    checksum += (uint16_t)settings.address[1]*5;
    checksum += (uint16_t)settings.address[2]*7;
    checksum += (settings.blockAlarm | settings.cloneAddress << 1 | settings.verbose) * 11;
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
    settings.verbose = m_log_all;
    settings.checksum = calculateChecksum(settings);
    updateSettings();
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
        m_log_all = settings.verbose;
        m_shell->println("Loaded settings");
    } else {
        m_shell->println("Settings could not be loaded, checksum mismatch.");
    }
    updateSettings();
}

#ifdef ORS_ASSET_TRACKER
void gpsOn(){
    m_gps_on_time = millis();
    m_is_gps_on = true;
    tracker.gpsOn();
}

void gpsOff(){
    tracker.gpsOff();
    m_satellite_count = 0;
    m_is_gps_on = false;
}
#endif
