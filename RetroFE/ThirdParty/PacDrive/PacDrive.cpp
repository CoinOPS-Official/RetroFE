// PacDrive.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "windows.h"
#include "PacDrive.h"

int main(int argc, char* argv[])
{
	PacInitialize();

	//Pac64SetLEDIntensity(0, -1, 0xff);
	PacSetLEDStates(0, 0xAAAA);
	//Pac64SetLEDState(0, 8, 7, false);

	PacShutdown();

	return 0;
}
