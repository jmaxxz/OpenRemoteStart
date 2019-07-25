// Copyright 2019 Jmaxxz
// BSD 3-Clause License

#pragma once

/*
 * These are the command types which can originate
 * from the antenna. They can be thought of as commands
 * being sent to the car.
 */
enum class remote_command_t: uint8_t
{ 
     lock=0x30,
     unlock=0x31,
     start=0x32,
     stop=0x33,
     trunk_release=0x34,
     panic=0x35,
     unlock2=0x38,
     aux1=0x39,
     aux2=0x3A,
     aux3=0x3B,
     aux4=0x3C,
     status_request=0xAA,
     status_request2=0xAE,
     toggle_valet_mode=0xA8,
     prog_btn_press=0xE3,
};

/*
 * These are the command types which can originate
 * from the remote starter rather than from the remote
 * antenna.
 */
enum class starter_command_t:uint8_t
{
     led_on=0x01,
     led_off=0x02,
     status_update=0xB8,
     led_flashing=0x04,
     remote_pairing=0xBF     
};
