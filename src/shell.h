// Copyright 2019 Jmaxxz
// BSD 3-Clause License


#pragma once
#include "shell.h"
#include "application.h"
#include "Particle.h"
class Shell
{
protected:
private:
	int _inBufferIndex = 0;
	char _inBuffer[128];
	void _handleInput(String command);
	void _resetInput();
	char *_current;
	int (*_cmd)(String);
	int (*_set)(String);
	void printLinePrefix();
	bool _midline = false;
public:
	Shell(char current[], int (*cmd)(String s), int (*set)(String s));
	~Shell();

	void print(const char* str);
    void println(const char* str);
	void processSerial();
};
