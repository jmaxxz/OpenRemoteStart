// Copyright 2019 Jmaxxz
// BSD 3-Clause License

#pragma once
#include <stdint.h>
#include <string.h>
#include "ringbuffer.h"
#include <memory>
#include <vector>

class FortinProcessor
{
protected:
private:
    RingBuffer *m_buffer;
    void (*m_messageHandler)(uint8_t *message, int length);
    void handlePotentiallyValid(uint8_t *message, int length);
public:
	FortinProcessor(RingBuffer *buffer, void (*messageHandler)(uint8_t *message, int length));
	~FortinProcessor();
    void add(uint8_t b);
    void reset();
};

// Example payload
//  1  2  3  4  5  6  7  8  9
// FF FF F1 01 84 00 00 01 48
struct statusPayload 
{ 
   // Remote Address
   uint8_t address[3];
   
   // This is likely another set of bit fields.
   // however, I have not decoded it.
   uint8_t unknownByte;
   
   bool valetMode:1;
   bool remoteStarted:1;
   bool engineTurningOver:1;
   bool acc:1;
   // I don't yet know what this flag
   // indicates. 
   bool unknownFlag1:1;
   bool trunkOpen:1;
   bool doorOpened:1;
   bool armed:1;
   
   uint8_t counterType[2];
   uint8_t counter[2];
};
