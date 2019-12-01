// Copyright 2019 Jmaxxz
// BSD 3-Clause License


#include "fortinprocessor.h"

FortinProcessor::FortinProcessor(RingBuffer *buffer, void (*messageHandler)(uint8_t *message, int length)){
    m_buffer = buffer;
    m_messageHandler = messageHandler;
}

FortinProcessor::~FortinProcessor(){
    delete m_buffer;
}

void FortinProcessor::add(uint8_t b){
    if(m_buffer->length() == 0 && b != 0x0C){
        // This can't be the start of a message
        return;
    }
    if(m_buffer->maxLength() == m_buffer->length()-1){
        m_buffer->reset();
    }

    m_buffer->addToBuffer(b);

    // at a length of 7 we could have a valid message
    if(m_buffer->length() >= 7){
        int payloadSize =  m_buffer->getFromBuffer(4);

        if(m_buffer->length() != payloadSize + 7){
            return;
        }
        uint8_t message[m_buffer->length()];
        for(int i = 0; i < m_buffer->length(); i++)
        {
            message[i] = m_buffer->getFromBuffer(i);
        }

        handlePotentiallyValid(message, m_buffer->length());
        return;
    }
}

void FortinProcessor::reset(){
    m_buffer->reset();
}

void FortinProcessor::handlePotentiallyValid(uint8_t *message, int length){
    if(length < 3 || message[length-1] != 0x0D) {
        //handleInvalidMessage();
        return;
    }

    uint8_t checksum = 0;
    for(int i = 1; i < length-2; i++ ){
        checksum += message[i];
    }

    if(checksum != message[length-2]) {
       //handleInvalidMessage();
       return;
    }

    m_messageHandler(message, length);
    m_buffer->reset();
}
