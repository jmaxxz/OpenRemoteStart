// Copyright 2019 Jmaxxz
// BSD 3-Clause License

#include "shell.h"
#include "Particle.h"
//-------------------------------------------
// https://gcc.gnu.org/onlinedocs/cpp/Standard-Predefined-Macros.html
#define COMPILED_TIME __TIME__
#define COMPILED_DATE __DATE__

//-------------------------------------------
Shell::Shell(char current[], int (*cmd)(String), int (*set)(String)){
	_current = current;
	_cmd = cmd;
	_set = set;
	 memset(_inBuffer, 0, sizeof(_inBuffer));
}

bool m_show_prompt = false;
Shell::~Shell() {

}

void Shell::println(const char* str){
	if(!_midline){
		printLinePrefix();
	}

    Serial.println(str);
	_midline = false;
}

void Shell::print(const char* str){
	if(!_midline){
		printLinePrefix();
	}
    Serial.print(str);
	_midline = true;
}

void Shell::printLinePrefix(){
    Serial.print(Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL));
    Serial.print(" ");
}


void Shell::_handleInput(String s) {
	Serial.println("");
	if(s == "version") {
		Serial.printf("Firmware: %s, Application: %s - %s", System.version().c_str(),
		 	COMPILED_DATE, COMPILED_TIME);
	}
	#ifdef WiFi
	else if(s == "wscan") {
		
		WiFiAccessPoint aps[40];
		int found = WiFi.scan(aps, 40);
		for (int i=0; i<found; i++) {
		    WiFiAccessPoint& ap = aps[i];
				Serial.printlnf("ssid,security,channel,rssi");
		    Serial.printlnf("%s,%d,%d,%d", ap.ssid, ap.security, ap.channel, ap.rssi);
		}
	}
	#endif
	else if(s=="dfu"){
		Serial.print("Entering DFU mode");
		Serial.flush();
		System.dfu();
	} else if(s=="prompt"){
        m_show_prompt = !m_show_prompt;
    } else if(s == "safemode"){
		Serial.print("Entering safe mode");
		Serial.flush();
		System.enterSafeMode();
	} else if(s == "configure"){
		#ifdef WiFi
		WiFi.listen();
		#else
		Serial.print("Wifi not supported on this board");
		#endif
	} else if(s == "reset"){
		System.reset();
	} else if(s == "current"){
		Serial.print(_current);
	} else if(s == "time"){
		Serial.print(Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL));
	} else if(s == "rdee") {
		for(int i = 0; i < EEPROM.length(); i++) {
			Serial.print(EEPROM.read(i), HEX);
		}
	} else if(s == "help") {
		Serial.println("Jmaxxz Open Remote Start");
		Serial.println("");
		Serial.print("version, safemode, current, rdee, time, dfu, lock, unlock, panic, trunk, ");

		#ifdef WiFi
		Serial.print("wscan, configure, ");
		#endif

		#ifdef ORS_ASSET_TRACKER
		Serial.print("GPS=[1|0], ");
		#endif

		Serial.print("start, stop, aux1, aux2, aux3, aux4, BlockAlarm=[1|0], CloneAddr=[1|0], Addr=?? ?? ??, Verbose=[1|0]");
	} else if(s==""){
		// do nothing if we get nothing
    } else if(s.indexOf("=") > 0){
		(*_set)(s);
	}else {
		(*_cmd)(s);
	}
}

void Shell::_resetInput() {
	_inBufferIndex = 0;
    if(m_show_prompt){
	    Serial.print("\nors> ");
    }
}

void Shell::processSerial() {
	int iCount = Serial.available();
	for(int i = 0; i < iCount; i++) {
		char in = Serial.read();

		if(in == -1){
			// this should never happen reading -1 in means there is nothing to be read
			// however, the call to Serial.available should ensure there is something
			// to be read.
		} else if(in == '\r'){ // ignore \r
			continue;
		}else if(in == '\n'){
			_inBuffer[_inBufferIndex] = '\0';
			_handleInput(String(_inBuffer));
			// process buffer contents as a command
			_resetInput();
		} else if(in == 0x08 || in == 0x7F){
			// backspace|del key
			if(_inBufferIndex > 0) {
				_inBufferIndex--;
				Serial.write(in);
			}
		} else if(in == 0x03) {
			// ctrl+c clear current buffer and throw
			// out everything
			_resetInput();
		} else {
			_inBuffer[_inBufferIndex++] = in;
			// Handle the general case
			Serial.write(in);
			if(_inBufferIndex == sizeof(_inBuffer)-2) {
				_resetInput();
			}
		}

	}
}
