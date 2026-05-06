#pragma once

#include "RunContext.h"
#include "../ui/MascotState.h"

bool enterMission(MissionProfile profile);
void exitMission();
bool isMissionActive();
RunContext currentRunContext();
MissionProfile activeMissionProfile();
void syncRuntimePresentation();
const char* currentSessionContextLabel();
