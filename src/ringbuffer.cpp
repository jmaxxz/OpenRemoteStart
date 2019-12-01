// Copyright 2019 Jmaxxz
// BSD 3-Clause License

#include "ringbuffer.h"

RingBuffer::RingBuffer(int size){
    m_buffer = new uint8_t[size];
    memset(m_buffer, 0, size);
    m_buffer_length = size;
    reset();
}
RingBuffer::~RingBuffer(){
    delete m_buffer;
}

void RingBuffer::addToBuffer(uint8_t b){
    m_buffer[(m_start_message + m_message_length) % m_buffer_length] = b;
    m_message_length++;
}

uint8_t RingBuffer::getFromBuffer(int index){
    return m_buffer[(m_start_message + index) % m_buffer_length];
}

int RingBuffer::length(){
    return m_message_length;
}

int RingBuffer::maxLength(){
    return m_buffer_length;
}

void RingBuffer::reset(){
    m_start_message = 0;
    m_message_length = 0;
}
