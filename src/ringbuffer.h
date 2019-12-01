// Copyright 2019 Jmaxxz
// BSD 3-Clause License

#pragma once
#include <stdint.h>
#include <string.h>

class RingBuffer
{
protected:
private:
    uint8_t *m_buffer;
    int m_start_message = 0;
    int m_message_length = 0;
    int m_buffer_length;
public:
	RingBuffer(int size);
	~RingBuffer();
    void addToBuffer(uint8_t b);
    uint8_t getFromBuffer(int index);
    void reset();
    int length();
    int maxLength();
};
